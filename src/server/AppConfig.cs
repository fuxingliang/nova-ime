using System.IO;
using System.Text.Json;

namespace PinyinPlus.Server;

/// <summary>
/// 服务端配置。配置文件位于 %AppData%\NovaInput\config.json（不存在时使用默认值）。
/// 设置面板修改后调用 Save() 写回，候选窗 ApplyConfig() 热生效。
/// </summary>
public sealed class AppConfig
{
    /// <summary>候选窗每页显示的候选数。</summary>
    public int PageSize { get; set; } = 9;

    /// <summary>候选字字体大小（DIP）。</summary>
    public double CandidateFontSize { get; set; } = 15;

    private static readonly string ConfigPath = Paths.ConfigFile;

    private static AppConfig? _instance;

    public static AppConfig Current => _instance ??= Load();

    private static AppConfig Load()
    {
        var cfg = new AppConfig();
        try
        {
            if (File.Exists(ConfigPath))
            {
                var loaded = JsonSerializer.Deserialize<AppConfig>(File.ReadAllText(ConfigPath));
                if (loaded is not null)
                {
                    if (loaded.PageSize is >= 1 and <= 20)
                    {
                        cfg.PageSize = loaded.PageSize;
                    }
                    if (loaded.CandidateFontSize is >= 10 and <= 26)
                    {
                        cfg.CandidateFontSize = loaded.CandidateFontSize;
                    }
                }
            }
        }
        catch
        {
            // 配置损坏时回退默认值
        }
        return cfg;
    }

    /// <summary>把当前配置写回 config.json（含校验后的有效值）。</summary>
    public void Save()
    {
        try
        {
            PageSize = Math.Clamp(PageSize, 1, 20);
            CandidateFontSize = Math.Clamp(CandidateFontSize, 10, 26);
            string dir = Path.GetDirectoryName(ConfigPath)!;
            if (!Directory.Exists(dir))
            {
                Directory.CreateDirectory(dir);
            }
            File.WriteAllText(ConfigPath, JsonSerializer.Serialize(this, new JsonSerializerOptions { WriteIndented = true }));
        }
        catch
        {
            // 保存失败不阻断：下次启动回退默认值
        }
    }
}
