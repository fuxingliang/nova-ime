using System.IO.Pipes;

namespace PinyinPlus.Server;

/// <summary>
/// 引擎管道客户端（PPIM 帧协议）。设置面板导入词库后发送"词库重载"消息，
/// 让引擎进程立即重建主词库+用户词库，无需重启引擎。
/// </summary>
public static class EnginePipeClient
{
    /// <summary>向引擎发送 RequestReloadUserDict（type=11），成功后返回 true。</summary>
    public static bool ReloadUserDict()
    {
        try
        {
            using var client = new NamedPipeClientStream(".", "PinyinPlus.Engine",
                PipeDirection.InOut, PipeOptions.None);
            client.Connect(2000);

            // 帧头：magic(0x5050494D "PPIM") + version(1) + type(11) + payloadLen(0)
            byte[] frame = new byte[16];
            WriteU32(frame, 0, 0x5050494D);
            WriteU32(frame, 4, 1);
            WriteU32(frame, 8, 11);
            WriteU32(frame, 12, 0);
            client.Write(frame, 0, frame.Length);
            client.Flush();

            // 读响应帧头（16 字节），确认引擎已处理
            byte[] header = new byte[16];
            int read = 0;
            while (read < 16)
            {
                int n = client.Read(header, read, 16 - read);
                if (n <= 0)
                {
                    return false;
                }
                read += n;
            }
            return true;
        }
        catch
        {
            return false;   // 引擎暂不可达：词库文件已写入，下次引擎启动会自动加载
        }
    }

    /// <summary>向引擎发送 SetTradition（type=15），简/繁输出开关即时生效（引擎无需重启）。</summary>
    public static bool SetTradition(bool on)
    {
        try
        {
            using var client = new NamedPipeClientStream(".", "PinyinPlus.Engine",
                PipeDirection.InOut, PipeOptions.None);
            client.Connect(2000);

            // 帧头 + 1 字节 payload（'0'/'1'）
            byte[] frame = new byte[17];
            WriteU32(frame, 0, 0x5050494D);
            WriteU32(frame, 4, 1);
            WriteU32(frame, 8, 15);
            WriteU32(frame, 12, 1);
            frame[16] = on ? (byte)'1' : (byte)'0';
            client.Write(frame, 0, frame.Length);
            client.Flush();

            // 读响应帧头（16 字节），确认引擎已处理
            byte[] header = new byte[16];
            int read = 0;
            while (read < 16)
            {
                int n = client.Read(header, read, 16 - read);
                if (n <= 0)
                {
                    return false;
                }
                read += n;
            }
            return true;
        }
        catch
        {
            return false;   // 引擎暂不可达：engine.conf 已写入，下次引擎启动自动生效
        }
    }

    private static void WriteU32(byte[] buf, int offset, uint value)
    {
        buf[offset] = (byte)(value & 0xFF);
        buf[offset + 1] = (byte)((value >> 8) & 0xFF);
        buf[offset + 2] = (byte)((value >> 16) & 0xFF);
        buf[offset + 3] = (byte)((value >> 24) & 0xFF);
    }
}
