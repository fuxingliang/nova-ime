using System.Diagnostics;
using System.IO;
using System.Threading;
using System.Windows;
using System.Windows.Threading;

namespace PinyinPlus.Server;

public partial class App : Application
{
    private PipeServer? _pipeServer;
    private CandidateWindow? _candidateWindow;
    private SymbolWindow? _symbolWindow;
    private System.Threading.Timer? _engineWatchdog;
    private System.Windows.Forms.NotifyIcon? _trayIcon;
    private Mutex? _instanceMutex;
    private long _lastEngineLaunchTick;   // 上次看门狗拉起引擎的时间戳（防崩溃循环降频）
    private long _lastPosLogTick;         // SetPosition 日志限流：位置消息高频，防磁盘 IO 拖慢 UI 线程（拖动卡顿根源）

    // 引擎路径 = 安装目录根（与 DLL/引擎同根，运行时定位）
    private static string EngineExePath => System.IO.Path.Combine(Paths.InstallDir, "PinyinPlus.Engine.exe");

    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        Trace("OnStartup begin");

        // 全局异常兜底：任何线程的未处理异常都记录日志；UI 线程异常标记已处理，
        // 避免候选窗服务静默崩溃（此前出现两次 .NET 未处理异常退出，候选窗消失）。
        AppDomain.CurrentDomain.UnhandledException += (_, ex) =>
            Trace($"FATAL UnhandledException: {ex.ExceptionObject}");
        DispatcherUnhandledException += (_, ex) =>
        {
            Trace($"FATAL DispatcherUnhandledException: {ex.Exception}");
            ex.Handled = true;   // 防止整个进程退出；候选窗下一帧自动恢复
        };
        TaskScheduler.UnobservedTaskException += (_, ex) =>
        {
            Trace($"FATAL UnobservedTaskException: {ex.Exception}");
            ex.SetObserved();
        };

        // 单实例：命名 Mutex（内核级，无进程枚举的 TOCTOU 竞态——两个实例几乎同时
        // 启动时进程枚举会双双通过检查导致双 Server 争抢管道）。
        // abandoned（原实例崩溃）时 WaitOne 抛 AbandonedMutexException 或返回 true，
        // 说明互斥体已释放 → 本实例接管，避免"无实例却永远判定冲突"。
        _instanceMutex = new Mutex(true, @"Local\PinyinPlus.Server.SingleInstance", out bool createdNew);
        if (!createdNew)
        {
            bool acquired = false;
            try
            {
                acquired = _instanceMutex.WaitOne(0);
            }
            catch (AbandonedMutexException)
            {
                acquired = true;   // 原实例崩溃：互斥体已移交本实例
            }
            if (!acquired)
            {
                Trace("another instance running -> shutdown");
                Shutdown(0);
                return;
            }
        }

        _candidateWindow = new CandidateWindow();
        _candidateWindow.CandidateClicked += OnCandidateClicked;
        _candidateWindow.DeleteUserWordRequested += OnDeleteUserWordRequested;
        _candidateWindow.DemoteWordRequested += OnDemoteWordRequested;
        _candidateWindow.InsertTextRequested += OnInsertTextRequested;
        _candidateWindow.OpenSymbolPanelRequested += OnOpenSymbolPanel;

        _symbolWindow = new SymbolWindow();
        _symbolWindow.InsertTextRequested += OnInsertTextRequested;

        _pipeServer = new PipeServer(OnPipeMessage);
        _pipeServer.Start();
        Trace("OnStartup end (running)");

        // 看门狗：引擎进程崩溃后 1 秒内自动拉起（沙箱自愈的监督者）
        StartEngineWatchdog();

        // 系统托盘：Nova 图标，双击/右键菜单打开设置面板
        SetupTrayIcon();
    }

    /// <summary>托盘图标 + 设置入口（Nova 品牌）。</summary>
    private void SetupTrayIcon()
    {
        try
        {
            _trayIcon = new System.Windows.Forms.NotifyIcon
            {
                Text = "Nova 输入法",
                Visible = true,
            };
            try
            {
                var stream = Application.GetResourceStream(
                    new Uri("pack://application:,,,/Assets/Nova.ico"))?.Stream;
                if (stream is not null)
                {
                    _trayIcon.Icon = new System.Drawing.Icon(stream);
                }
            }
            catch
            {
                // 图标加载失败不影响托盘功能（系统用默认图标）
            }

            _trayIcon.DoubleClick += (_, _) => Dispatcher.BeginInvoke(OpenSettings);

            var menu = new System.Windows.Forms.ContextMenuStrip();
            menu.Items.Add("打开设置", null, (_, _) => Dispatcher.BeginInvoke(OpenSettings));
            menu.Items.Add("快捷键", null, (_, _) => Dispatcher.BeginInvoke(() => ShortcutsWindow.Open()));
            menu.Items.Add(new System.Windows.Forms.ToolStripSeparator());
            menu.Items.Add("关于 Nova 输入法", null, (_, _) => Dispatcher.BeginInvoke(ShowAbout));
            _trayIcon.ContextMenuStrip = menu;
        }
        catch (Exception ex)
        {
            Trace($"tray icon FAILED: {ex.Message}");
        }
    }

    private void OpenSettings() => SettingsWindow.Open();

    private void ShowAbout()
    {
        MessageBox.Show(
            "Nova 输入法\n\n" +
            "Nova 系列软件 · 拼音输入法\n" +
            "架构：TSF 瘦 DLL + 独立引擎进程 + 候选窗服务\n" +
            "引擎崩溃自动拉起，宿主应用零重启。",
            "关于 Nova 输入法",
            MessageBoxButton.OK,
            MessageBoxImage.Information);
    }

    /// <summary>设置面板保存后：候选窗热应用新配置（候选数/字体）。</summary>
    public void ApplyCandidateConfig() => _candidateWindow?.ApplyConfig();

    /// <summary>看门狗：周期检查引擎进程，消失则拉起。崩溃恢复不依赖用户恰好打字触发 DLL 重连。</summary>
    private void StartEngineWatchdog()
    {
        _engineWatchdog = new System.Threading.Timer(
            _ => EnsureEngineRunning(), null,
            TimeSpan.Zero, TimeSpan.FromSeconds(1));
    }

    private void EnsureEngineRunning()
    {
        try
        {
            if (Process.GetProcessesByName("PinyinPlus.Engine").Length > 0)
            {
                return;
            }
            // 防崩溃循环：5 秒内已拉起过一次则不再拉起（引擎反复崩溃时降频，避免疯狂重启）
            long now = Environment.TickCount64;
            if (now - Interlocked.Read(ref _lastEngineLaunchTick) < 5000)
            {
                return;
            }
            Interlocked.Exchange(ref _lastEngineLaunchTick, now);
            Trace("watchdog: engine not running, start it");
            Process.Start(new ProcessStartInfo(EngineExePath)
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                WindowStyle = ProcessWindowStyle.Hidden,
            });
        }
        catch (Exception ex)
        {
            Trace($"watchdog FAILED: {ex.Message}");
        }
    }

    private static readonly object TraceLock = new();
    private static void Trace(string msg)
    {
        try
        {
            lock (TraceLock)
            {
                File.AppendAllText(System.IO.Path.Combine(Paths.DataDir, "app_trace.log"),
                    $"{DateTime.Now:HH:mm:ss.fff} {msg}\r\n");
            }
        }
        catch { }
    }

    private void OnCandidateClicked(int index)
    {
        Log($"   CandidateClicked {index} → SendSelectCandidate");
        _pipeServer?.SendSelectCandidate(index);
    }

    private void OnDeleteUserWordRequested(string word)
    {
        Log($"   DeleteUserWordRequested '{word}' → SendDeleteUserWord");
        _pipeServer?.SendDeleteUserWord(word);
    }

    private void OnDemoteWordRequested(string word)
    {
        Log($"   DemoteWordRequested '{word}' → SendDemoteWord");
        _pipeServer?.SendDemoteWord(word);
    }

    private void OnInsertTextRequested(string text)
    {
        Log($"   InsertTextRequested '{text}' → SendInsertText");
        _pipeServer?.SendInsertText(text);
    }

    private void OnOpenSymbolPanel()
    {
        Log("   OpenSymbolPanel → Show SymbolWindow");
        _symbolWindow?.Show();   // 窗口 Topmost + 永不激活，直接显示在最上层
    }

    private void OnPipeMessage(object message)
    {
        Log($"<< {message.GetType().Name}");
        Dispatcher.BeginInvoke(DispatcherPriority.Normal, () => HandleMessage(message));
    }

    private void HandleMessage(object message)
    {
        switch (message)
        {
            case ShowCandidatesMessage show:
                Log($"   ShowCandidates buffer='{show.Buffer}' sel={show.SelectedIndex} count={show.Candidates.Count}");
                _candidateWindow?.ShowCandidates(show.Buffer, show.Candidates, show.SelectedIndex, show.PageStart);
                break;
            case HideMessage:
                Log("   Hide");
                _candidateWindow?.Hide();
                break;
            case SetSelectionMessage sel:
                Log($"   SetSelection {sel.Index}");
                _candidateWindow?.UpdateSelection(sel.Index);
                break;
            case SetPositionMessage pos:
                {
                    // 位置消息最高频（打字时每帧可达数百条），同步写日志会阻塞 UI 线程
                    // 导致候选窗拖动卡顿/抖动：限流到 200ms 打一条
                    long now = Environment.TickCount64;
                    if (now - _lastPosLogTick >= 200)
                    {
                        Log($"   SetPosition {pos.X},{pos.Y}");
                        _lastPosLogTick = now;
                    }
                    _candidateWindow?.MoveToPoint(pos.X, pos.Y);
                    break;
                }
        }
    }

    private static readonly object LogLock = new();
    private static void Log(string msg)
    {
        try
        {
            lock (LogLock)
            {
                File.AppendAllText(System.IO.Path.Combine(Paths.DataDir, "server_debug.log"),
                    $"{DateTime.Now:HH:mm:ss.fff} {msg}\r\n");
            }
        }
        catch { }
    }

    protected override void OnExit(ExitEventArgs e)
    {
        _engineWatchdog?.Dispose();
        _pipeServer?.Stop();
        if (_trayIcon is not null)
        {
            _trayIcon.Visible = false;
            _trayIcon.Dispose();
            _trayIcon = null;
        }
        base.OnExit(e);
    }
}
