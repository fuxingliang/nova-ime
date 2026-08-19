using System.IO;

namespace PinyinPlus.Server;

/// <summary>
/// 运行时路径（安装包化：程序目录 + 数据目录分离，与 DLL/引擎侧约定一致）。
/// 安装目录 = 含词库/符号表的目录（Server 位于 <安装目录>\server\，向上逐级搜索）；
/// 数据目录 = %AppData%\NovaInput（engine.conf / userdict.txt / config.json，可写）。
/// 数据独立于安装目录：升级/卸载安装程序不触碰数据目录，用户词库与配置天然保留。
/// </summary>
public static class Paths
{
    /// <summary>数据目录（%AppData%\NovaInput，首次访问即创建）。</summary>
    public static string DataDir { get; } = InitDataDir();

    /// <summary>安装目录（含词库/符号表的目录，逐级向上搜索）。</summary>
    public static string InstallDir { get; } = LocateInstallDir();

    /// <summary>engine.conf 完整路径（引擎/ DLL 实时读取的配置）。</summary>
    public static string EngineConf => System.IO.Path.Combine(DataDir, "engine.conf");

    /// <summary>用户词库完整路径。</summary>
    public static string UserDict => System.IO.Path.Combine(DataDir, "userdict.txt");

    /// <summary>服务端外观配置 config.json。</summary>
    public static string ConfigFile => System.IO.Path.Combine(DataDir, "config.json");

    private static string InitDataDir()
    {
        string dir = System.IO.Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
            "NovaInput");
        try
        {
            Directory.CreateDirectory(dir);   // 每用户可写，无需提权
        }
        catch
        {
            // 创建失败（异常环境）→ 回退 Server 目录，不阻断启动
            dir = AppContext.BaseDirectory;
        }
        return dir;
    }

    private static string LocateInstallDir()
    {
        DirectoryInfo? dir = new(AppContext.BaseDirectory);
        for (int i = 0; i < 8 && dir is not null; i++)
        {
            // 安装目录标志：词库或符号表在根下（Server 在 <根>\server\）
            if (File.Exists(System.IO.Path.Combine(dir.FullName, "pinyin-plus.txt")) ||
                File.Exists(System.IO.Path.Combine(dir.FullName, "symbols.txt")))
            {
                return dir.FullName;
            }
            dir = dir.Parent;
        }
        return AppContext.BaseDirectory;
    }
}
