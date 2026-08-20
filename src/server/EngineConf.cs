using System.IO;

namespace PinyinPlus.Server;

/// <summary>
/// %AppData%\NovaInput\engine.conf 读写工具（引擎/ DLL 实时读取的配置）。
/// 键：learn（自学习）、bigdict（大字库）、tradition（繁体输出）、punct（中文标点）、width（全角）、charsel（搜狗式拆字）。
/// 设置面板与候选窗状态按钮都经此读写——整体读写避免单键覆盖丢另一键。
/// </summary>
public static class EngineConf
{
    public static string Path => Paths.EngineConf;

    public static bool Read(string key, bool defaultValue)
    {
        try
        {
            if (File.Exists(Path))
            {
                foreach (string line in File.ReadAllLines(Path))
                {
                    if (line.StartsWith(key + "=", StringComparison.OrdinalIgnoreCase) && line.Length > key.Length + 1)
                    {
                        return line[key.Length + 1] == '1';
                    }
                }
            }
        }
        catch
        {
            // 读取失败 → 默认值
        }
        return defaultValue;
    }

    /// <summary>原子写全部键（临时文件 + 改名，避免引擎/DLL 读到半截内容）。</summary>
    public static void WriteAll(bool learning, bool bigdict, bool tradition, bool punct, bool width, bool charsel)
    {
        try
        {
            string dir = System.IO.Path.GetDirectoryName(Path)!;
            if (!Directory.Exists(dir))
            {
                Directory.CreateDirectory(dir);
            }
            string tmp = Path + ".tmp";
            File.WriteAllText(tmp,
                $"learn={(learning ? 1 : 0)}\nbigdict={(bigdict ? 1 : 0)}\n" +
                $"tradition={(tradition ? 1 : 0)}\npunct={(punct ? 1 : 0)}\n" +
                $"width={(width ? 1 : 0)}\ncharsel={(charsel ? 1 : 0)}\n");
            File.Move(tmp, Path, true);
        }
        catch
        {
            // 写入失败不阻断：引擎/DLL 默认值兜底
        }
    }

    // 无配置时的默认值：learn 开、bigdict 关、tradition 关、punct 开（中文标点）、width 关（半角）、charsel 开（搜狗式拆字）
    public static bool ReadLearning() => Read("learn", true);
    public static bool ReadBigDict() => Read("bigdict", false);
    public static bool ReadTradition() => Read("tradition", false);
    public static bool ReadPunct() => Read("punct", true);
    public static bool ReadWidth() => Read("width", false);
    public static bool ReadCharSplit() => Read("charsel", true);
}
