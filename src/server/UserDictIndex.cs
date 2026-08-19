using System.Collections.Generic;
using System.IO;
using System.Text;

namespace PinyinPlus.Server;

/// <summary>
/// 用户词索引：读取 userdict.txt（引擎在每次用户词变更后立即落盘），
/// 供候选窗标记"自定义词"。文件 mtime/length 变化才重读，命中为哈希比对（微秒级）。
/// </summary>
public static class UserDictIndex
{
    private static readonly object Sync = new();
    private static HashSet<string>? _words;
    private static DateTime _lastWriteUtc;
    private static long _lastLength = -1;
    private static bool _loadedOnce;

    /// <summary>候选词是否为用户自定义词（造词 / 自学习入库的新词）。</summary>
    public static bool IsUserWord(string word)
    {
        if (string.IsNullOrEmpty(word))
            return false;
        EnsureFresh();
        return _words?.Contains(word) ?? false;
    }

    private static void EnsureFresh()
    {
        try
        {
            string? path = LocateUserDict();
            if (path is null)
            {
                _words = null;
                return;
            }
            var fi = new FileInfo(path);
            if (!fi.Exists)
            {
                _words = null;
                return;
            }
            if (fi.LastWriteTimeUtc == _lastWriteUtc && fi.Length == _lastLength)
                return;

            lock (Sync)
            {
                fi.Refresh();   // 重新取 stat，避免与引擎并发落盘竞态
                if (fi.LastWriteTimeUtc == _lastWriteUtc && fi.Length == _lastLength)
                    return;

                var set = new HashSet<string>();
                foreach (string line in File.ReadLines(path, Encoding.UTF8))
                {
                    if (string.IsNullOrWhiteSpace(line))
                        continue;
                    // 行格式：pinyin \t word \t freq \t initial（畸形行跳过）
                    int firstTab = line.IndexOf('\t');
                    if (firstTab < 0)
                        continue;
                    int secondTab = line.IndexOf('\t', firstTab + 1);
                    if (secondTab < 0)
                        continue;
                    set.Add(line.Substring(firstTab + 1, secondTab - firstTab - 1));
                }
                _words = set;
                _lastWriteUtc = fi.LastWriteTimeUtc;
                _lastLength = fi.Length;

                if (!_loadedOnce)
                {
                    _loadedOnce = true;
                    try
                    {
                        File.AppendAllText(System.IO.Path.Combine(Paths.DataDir, "server_debug.log"),
                            $"{DateTime.Now:HH:mm:ss.fff} UserDictIndex loaded {set.Count} words from {path}\r\n");
                    }
                    catch { }
                }
            }
        }
        catch
        {
            // 读失败（文件被占用等）时保留旧集合，候选窗不受影响
        }
    }

    /// <summary>定位 userdict.txt：数据目录 %AppData%\NovaInput\userdict.txt（引擎/ Server 共用）。
    /// 数据目录由引擎与 Server 共同维护，安装目录只读（Program Files 场景），用户数据独立。</summary>
    private static string? LocateUserDict()
    {
        return Paths.UserDict;
    }
}
