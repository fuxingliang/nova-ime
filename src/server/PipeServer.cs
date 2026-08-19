using System.IO;
using System.IO.Pipes;
using System.Reflection;
using System.Security.AccessControl;
using System.Text;

namespace PinyinPlus.Server;

/// <summary>
/// 命名管道服务器：接收 TSF DLL 发来的帧，解析后通过回调分发；
/// 同时支持回写（鼠标点击候选 → 向活跃连接发 SelectCandidate）。
/// </summary>
public sealed class PipeServer
{
    public const string PipeName = "PinyinPlus.Service";

    // ---- 管道安全描述符（反射调用 .NET internal 构造函数）----
    // .NET 9 的 NamedPipeServerStream(String, PipeDirection, Int32, PipeTransmissionMode,
    // PipeOptions, Int32, Int32, PipeSecurity) 构造函数是 internal，无法直接编译调用。
    // 用反射调用它（内部走 CreateNamedPipe + InitializeHandle，完全复用 .NET 生命周期）。
    //
    // 安全描述符只用 DACL（Everyone 全权），不用 SACL：
    //   D:(A;;GA;;;WD)      允许 Everyone 全权访问
    //   ★ 不用 S:(ML;;NW;;;LW) —— 设置 SACL 需要 SeSecurityPrivilege（仅管理员且需显式启用），
    //     Server 以普通权限启动时会抛"客户端没有所需的特权"，管道根本建不起来
    //     （2026-08-18 实测验证）。
    // 为什么够用：DACL Everyone 全权已允许任何完整性级别的客户端连接，
    // 完整性标签只在"同一资源被更高完整性进程独占"时拦截——这里我们显式放开了 DACL。
    private static readonly ConstructorInfo? s_pipeCtor = typeof(NamedPipeServerStream).GetConstructor(
        BindingFlags.Instance | BindingFlags.NonPublic,
        null,
        new[]
        {
            typeof(string), typeof(PipeDirection), typeof(int), typeof(PipeTransmissionMode),
            typeof(PipeOptions), typeof(int), typeof(int), typeof(PipeSecurity),
            typeof(HandleInheritability), typeof(PipeAccessRights),
        },
        null);

    private static PipeSecurity CreateLoosePipeSecurity()
    {
        var ps = new PipeSecurity();
        ps.SetSecurityDescriptorSddlForm("D:(A;;GA;;;WD)");
        return ps;
    }

    private static NamedPipeServerStream CreateServerPipe()
    {
        var pipe = (NamedPipeServerStream?)s_pipeCtor?.Invoke(new object?[]
        {
            PipeName, PipeDirection.InOut, NamedPipeServerStream.MaxAllowedServerInstances,
            PipeTransmissionMode.Byte, PipeOptions.Asynchronous, 0, 0, CreateLoosePipeSecurity(),
            HandleInheritability.None, default(PipeAccessRights),
        });
        if (pipe is null)
        {
            throw new InvalidOperationException("NamedPipeServerStream internal ctor not found");
        }
        return pipe;
    }

    private readonly Action<object> _onMessage;
    private CancellationTokenSource? _cts;
    private readonly object _gate = new();
    private NamedPipeServerStream? _activePipe;   // 最近发 Show 的客户端（候选窗所属 DLL 实例）

    public PipeServer(Action<object> onMessage)
    {
        _onMessage = onMessage;
    }

    public void Start()
    {
        _cts = new CancellationTokenSource();
        _ = AcceptLoopAsync(_cts.Token);
    }

    public void Stop()
    {
        _cts?.Cancel();
    }

    /// <summary>向 DLL 发删除用户词命令（右键菜单：误造词直接删）。</summary>
    public void SendDeleteUserWord(string word)
    {
        SendToActive(PipeProtocol.EncodeDeleteUserWord(word), $"DeleteUserWord word={word}");
    }

    /// <summary>向 DLL 发"降低排位"命令（右键菜单：目标词降权沉底）。</summary>
    public void SendDemoteWord(string word)
    {
        SendToActive(PipeProtocol.EncodeDemoteWord(word), $"DemoteWord word={word}");
    }

    /// <summary>向 DLL 发插入文本命令（符号面板选符号上屏）。</summary>
    public void SendInsertText(string text)
    {
        SendToActive(PipeProtocol.EncodeInsertText(text), $"InsertText text={text}");
    }

    private void SendToActive(byte[] data, string desc)
    {
        lock (_gate)
        {
            var pipe = _activePipe;
            if (pipe is null)
            {
                PLog($"{desc} SKIP (no active client)");
                return;
            }
            try
            {
                // 异步写：避免同步 Write 阻塞（对端 DLL 不读时）卡住锁与读循环
                _ = pipe.WriteAsync(data, 0, data.Length).ContinueWith(t =>
                {
                    if (t.IsFaulted)
                    {
                        PLog($"{desc} write failed: {t.Exception?.GetBaseException()?.Message}");
                    }
                    else
                    {
                        PLog($"{desc} -> active");
                    }
                });
            }
            catch (Exception ex)
            {
                PLog($"{desc} FAILED: {ex.Message}");
            }
        }
    }

    /// <summary>鼠标点击候选后，向当前活跃的 DLL 实例回发选字命令（index 为全局候选索引）。</summary>
    public void SendSelectCandidate(int index)
    {
        lock (_gate)
        {
            var pipe = _activePipe;
            if (pipe is null)
            {
                PLog("SendSelectCandidate SKIP (no active client)");
                return;
            }
            var data = PipeProtocol.EncodeSelectCandidate(index);
            try
            {
                // 异步写：避免同步 Write 阻塞（对端 DLL 不读时）卡住锁与读循环
                _ = pipe.WriteAsync(data, 0, data.Length).ContinueWith(t =>
                {
                    if (t.IsFaulted)
                    {
                        PLog($"SendSelectCandidate write failed: {t.Exception?.GetBaseException()?.Message}");
                    }
                    else
                    {
                        PLog($"SendSelectCandidate index={index} -> active");
                    }
                });
            }
            catch (Exception ex)
            {
                PLog($"SendSelectCandidate FAILED: {ex.Message}");
            }
        }
    }

    private async Task AcceptLoopAsync(CancellationToken ct)
    {
        while (!ct.IsCancellationRequested)
        {
            try
            {
                // 反射调用 .NET internal 构造函数创建带宽松安全描述符的管道
                //（与引擎侧 EnginePipe.cpp 的 SDDL 完全一致，见类注释）。
                var pipe = CreateServerPipe();

                PLog("AcceptLoop: waiting for connection...");
                await pipe.WaitForConnectionAsync(ct);
                PLog("AcceptLoop: connection accepted, starting ReadLoop");
                _ = ReadLoopAsync(pipe, ct);   // 每个客户端独立读循环，互不阻塞
            }
            catch (OperationCanceledException)
            {
                break;
            }
            catch (Exception ex)
            {
                PLog($"AcceptLoop exception: {ex.Message}");
                // 客户端断开或管道错误：等待后重连
                try { await Task.Delay(200, ct); } catch (OperationCanceledException) { break; }
            }
        }
    }

    private async Task ReadLoopAsync(NamedPipeServerStream pipe, CancellationToken ct)
    {
        int clientNo = pipe.SafePipeHandle.IsInvalid ? 0 : pipe.GetHashCode();
        PLog($"ReadLoop: started for client={clientNo}");
        try
        {
            var buffer = new byte[64 * 1024];
            var pending = new List<byte>();

            while (!ct.IsCancellationRequested)
            {
                int read;
                try
                {
                    read = await pipe.ReadAsync(buffer, 0, buffer.Length, ct);
                }
                catch (Exception ex)
                {
                    PLog($"ReadLoop client={clientNo} ReadAsync exception: {ex.Message}");
                    break;
                }
                if (read <= 0)
                {
                    PLog($"ReadLoop client={clientNo} read={read}, exiting");
                    break;
                }

                PLog($"client={clientNo} read={read} bytes");
                if (read > 16)
                {
                    PLog($"client={clientNo} hex={Convert.ToHexString(buffer, 0, Math.Min(read, 4096))}");
                }
                pending.AddRange(buffer.AsSpan(0, read).ToArray());

                int start = 0;
                while (true)
                {
                    (int consumed, object message)? parsed;
                    try
                    {
                        parsed = PipeProtocol.TryParse(pending.ToArray(), start, pending.Count - start);
                    }
                    catch (Exception ex)
                    {
                        PLog($"client={clientNo} PARSE EXCEPTION: {ex}");
                        break;
                    }

                    if (parsed is null)
                    {
                        PLog($"client={clientNo} parse=insufficient (start={start} pend={pending.Count - start})");
                        break;
                    }

                    var (consumed, message) = parsed.Value;
                    PLog($"client={clientNo} parse=consumed({consumed}) type={message?.GetType().Name ?? "null"}");
                    start += consumed;
                    if (message is not null)
                    {
                        // 候选窗 Show 来自哪个连接，谁就是当前活跃输入法实例（点击命令回发给它）
                        if (message is ShowCandidatesMessage)
                        {
                            lock (_gate) { _activePipe = pipe; }
                        }
                        try { _onMessage(message); }
                        catch (Exception ex) { PLog($"client={clientNo} DISPATCH EXCEPTION: {ex}"); }
                    }
                }

                if (start > 0)
                {
                    pending.RemoveRange(0, start);
                }
            }
        }
        finally
        {
            PLog($"ReadLoop client={clientNo} exiting, disposing pipe");
            lock (_gate)
            {
                if (ReferenceEquals(_activePipe, pipe))
                {
                    _activePipe = null;
                }
            }
            await pipe.DisposeAsync();
            PLog("client disconnected");
        }
    }

    private static readonly object PipeLogLock = new();
    private static void PLog(string msg)
    {
        try
        {
            lock (PipeLogLock)
            {
                File.AppendAllText(System.IO.Path.Combine(Paths.DataDir, "pipe_debug.log"),
                    $"{DateTime.Now:HH:mm:ss.fff} {msg}\r\n");
            }
        }
        catch { }
    }
}
