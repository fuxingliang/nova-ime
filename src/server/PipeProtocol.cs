using System.IO;
using System.Text;

namespace PinyinPlus.Server;

/// <summary>
/// DLL(TSF) 与服务进程之间的 IPC 协议。
/// 帧格式（小端）：
///   [uint32 magic = 0x5050494D "PPIM"]
///   [uint32 version = 1]
///   [uint32 messageType]  1=Show 2=Hide 3=SetSelection 4=SetPosition 5=SelectCandidate
///   [uint32 payloadLen]
///   [payload...]
/// 1-4 由 DLL→服务进程（写）；5-7 由服务进程→DLL
/// （5=鼠标点击候选回发选字、6=右键删除用户词、7=符号面板插入文本）。
/// </summary>
public enum PipeMessageType : uint
{
    Show = 1,
    Hide = 2,
    SetSelection = 3,
    SetPosition = 4,
    SelectCandidate = 5,
    DeleteUserWord = 6,
    InsertText = 7,
    DemoteWord = 8,
}

public sealed class ShowCandidatesMessage
{
    public string Buffer { get; set; } = "";
    public int SelectedIndex { get; set; }
    public int PageStart { get; set; }
    public List<string> Candidates { get; set; } = new();
}

public sealed class HideMessage
{
}

public sealed class SetSelectionMessage
{
    public int Index { get; set; }
}

public sealed class SetPositionMessage
{
    public int X { get; set; }
    public int Y { get; set; }
}

public sealed class SelectCandidateMessage
{
    public int Index { get; set; }
}

public static class PipeProtocol
{
    public const uint Magic = 0x5050494D;
    public const uint Version = 1;

    public static readonly byte[] MagicBytes = BitConverter.GetBytes(Magic);
    public static readonly byte[] VersionBytes = BitConverter.GetBytes(Version);

    private static void WriteInt32(Stream s, int v) => s.Write(BitConverter.GetBytes(v));
    private static void WriteUInt32(Stream s, uint v) => s.Write(BitConverter.GetBytes(v));
    private static void WriteString(Stream s, string text)
    {
        byte[] bytes = Encoding.UTF8.GetBytes(text);
        WriteInt32(s, bytes.Length);
        s.Write(bytes, 0, bytes.Length);
    }

    public static byte[] EncodeShow(ShowCandidatesMessage msg)
    {
        using var ms = new MemoryStream();
        WriteUInt32(ms, Magic);
        WriteUInt32(ms, Version);
        WriteUInt32(ms, (uint)PipeMessageType.Show);

        using var payload = new MemoryStream();
        WriteString(payload, msg.Buffer);
        WriteInt32(payload, msg.SelectedIndex);
        WriteInt32(payload, msg.PageStart);
        WriteInt32(payload, msg.Candidates.Count);
        foreach (var c in msg.Candidates)
            WriteString(payload, c);

        WriteUInt32(ms, (uint)payload.Length);
        payload.Position = 0;
        payload.CopyTo(ms);
        return ms.ToArray();
    }

    public static byte[] EncodeHide() => EncodeSimple(PipeMessageType.Hide);
    public static byte[] EncodeSetSelection(int index) => EncodeInt(PipeMessageType.SetSelection, index);
    public static byte[] EncodeSelectCandidate(int index) => EncodeInt(PipeMessageType.SelectCandidate, index);

    /// <summary>右键删除用户词：负载 = 待删词（UTF-8 字符串）。</summary>
    public static byte[] EncodeDeleteUserWord(string word) => EncodeString(PipeMessageType.DeleteUserWord, word);

    /// <summary>右键"降低排位"：负载 = 目标词（UTF-8 字符串）。</summary>
    public static byte[] EncodeDemoteWord(string word) => EncodeString(PipeMessageType.DemoteWord, word);

    /// <summary>符号面板插入文本：负载 = 要插入的符号/文本（UTF-8 字符串）。</summary>
    public static byte[] EncodeInsertText(string text) => EncodeString(PipeMessageType.InsertText, text);

    private static byte[] EncodeString(PipeMessageType type, string text)
    {
        using var ms = new MemoryStream();
        WriteUInt32(ms, Magic);
        WriteUInt32(ms, Version);
        WriteUInt32(ms, (uint)type);
        using var payload = new MemoryStream();
        WriteString(payload, text);
        WriteUInt32(ms, (uint)payload.Length);
        payload.Position = 0;
        payload.CopyTo(ms);
        return ms.ToArray();
    }
    public static byte[] EncodeSetPosition(int x, int y)
    {
        using var ms = new MemoryStream();
        WriteUInt32(ms, Magic);
        WriteUInt32(ms, Version);
        WriteUInt32(ms, (uint)PipeMessageType.SetPosition);
        using var payload = new MemoryStream();
        WriteInt32(payload, x);
        WriteInt32(payload, y);
        WriteUInt32(ms, (uint)payload.Length);
        payload.Position = 0;
        payload.CopyTo(ms);
        return ms.ToArray();
    }

    private static byte[] EncodeSimple(PipeMessageType type)
    {
        using var ms = new MemoryStream();
        WriteUInt32(ms, Magic);
        WriteUInt32(ms, Version);
        WriteUInt32(ms, (uint)type);
        WriteUInt32(ms, 0);
        return ms.ToArray();
    }

    private static byte[] EncodeInt(PipeMessageType type, int value)
    {
        using var ms = new MemoryStream();
        WriteUInt32(ms, Magic);
        WriteUInt32(ms, Version);
        WriteUInt32(ms, (uint)type);
        using var payload = new MemoryStream();
        WriteInt32(payload, value);
        WriteUInt32(ms, (uint)payload.Length);
        payload.Position = 0;
        payload.CopyTo(ms);
        return ms.ToArray();
    }

    private static int ReadInt32(byte[] buf, int offset, ref int pos)
    {
        int v = BitConverter.ToInt32(buf, offset + pos);
        pos += 4;
        return v;
    }

    private static string ReadString(byte[] buf, int offset, ref int pos)
    {
        int len = ReadInt32(buf, offset, ref pos);
        string s = Encoding.UTF8.GetString(buf, offset + pos, len);
        pos += len;
        return s;
    }

    /// <summary>解析一帧。数据不足返回 null；返回 (消耗字节数, 消息对象)。</summary>
    public static (int consumed, object message)? TryParse(byte[] data, int offset, int length)
    {
        const int headerLen = 16;
        if (length < headerLen)
            return null;

        uint magic = BitConverter.ToUInt32(data, offset);
        if (magic != Magic)
            return null;

        uint version = BitConverter.ToUInt32(data, offset + 4);
        uint type = BitConverter.ToUInt32(data, offset + 8);
        uint payloadLen = BitConverter.ToUInt32(data, offset + 12);
        int totalLen = headerLen + (int)payloadLen;
        if (length < totalLen)
            return null;

        int pos = headerLen;
        switch ((PipeMessageType)type)
        {
            case PipeMessageType.Show:
            {
                var msg = new ShowCandidatesMessage();
                msg.Buffer = ReadString(data, offset, ref pos);
                msg.SelectedIndex = ReadInt32(data, offset, ref pos);
                msg.PageStart = ReadInt32(data, offset, ref pos);

                int candidateStart = pos;
                int count = ReadInt32(data, offset, ref pos);
                try
                {
                    for (int i = 0; i < count; i++)
                        msg.Candidates.Add(ReadString(data, offset, ref pos));
                }
                catch (ArgumentOutOfRangeException)
                {
                    // 兼容旧版 DLL：旧版 Show 帧无 count 字段，count 位置实为第一个候选的长度前缀。
                    // 回退为从候选区起点起逐个读取字符串，直到 payload 末尾。
                    msg.Candidates.Clear();
                    pos = candidateStart;
                    int payloadEnd = headerLen + (int)payloadLen;
                    while (pos + 4 <= payloadEnd)
                    {
                        msg.Candidates.Add(ReadString(data, offset, ref pos));
                    }
                }
                return (totalLen, msg);
            }
            case PipeMessageType.Hide:
                return (totalLen, new HideMessage());
            case PipeMessageType.SetSelection:
                return (totalLen, new SetSelectionMessage { Index = ReadInt32(data, offset, ref pos) });
            case PipeMessageType.SetPosition:
                return (totalLen, new SetPositionMessage { X = ReadInt32(data, offset, ref pos), Y = ReadInt32(data, offset, ref pos) });
            case PipeMessageType.SelectCandidate:
                return (totalLen, new SelectCandidateMessage { Index = ReadInt32(data, offset, ref pos) });
            default:
                return (totalLen, null!);
        }
    }
}
