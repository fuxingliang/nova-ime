using System.IO;
using System.Text;
using System.Windows;
using System.Windows.Controls;

namespace PinyinPlus.Server;

/// <summary>
/// Nova 输入法设置面板。
/// 外观配置（候选数/候选字大小）→ config.json 热生效；
/// 自学习开关/大字库模式 → %AppData%\NovaInput\engine.conf（引擎实时读取，无需重启）；
/// 大字库切换 → 通知引擎双缓冲热重载换词库（打字零阻塞）；
/// 词库导出 → 复制数据目录 userdict.txt 到用户指定位置（备份迁移）。
/// </summary>
public partial class SettingsWindow : Window
{
    private static readonly string UserDictPath = Paths.UserDict;

    private static SettingsWindow? _instance;

    /// <summary>单例：重复打开时激活已有窗口，避免堆积多个设置面板。</summary>
    public static SettingsWindow Open()
    {
        if (_instance is null || !_instance.IsLoaded)
        {
            _instance = new SettingsWindow();
            _instance.Closed += (_, _) => _instance = null;
        }
        _instance.Show();
        _instance.Activate();
        return _instance;
    }

    public SettingsWindow()
    {
        InitializeComponent();
        LoadConfigToUi();
        FontSizeSlider.ValueChanged += (_, _) =>
            FontSizeValue.Text = ((int)FontSizeSlider.Value).ToString();
    }

    private void LoadConfigToUi()
    {
        var cfg = AppConfig.Current;
        // 候选数下拉
        int idx = -1;
        foreach (ComboBoxItem item in PageSizeCombo.Items)
        {
            if (int.TryParse(item.Content?.ToString(), out int v) && v == cfg.PageSize)
            {
                idx = PageSizeCombo.Items.IndexOf(item);
                break;
            }
        }
        PageSizeCombo.SelectedIndex = idx >= 0 ? idx : 4;

        FontSizeSlider.Value = Math.Clamp(cfg.CandidateFontSize, 12, 22);
        FontSizeValue.Text = ((int)FontSizeSlider.Value).ToString();

        LearningCheck.IsChecked = EngineConf.ReadLearning();
        BigDictCheck.IsChecked = EngineConf.ReadBigDict();
        TraditionCheck.IsChecked = EngineConf.ReadTradition();
    }

    private void OnSaveClick(object sender, RoutedEventArgs e)
    {
        var cfg = AppConfig.Current;
        if (PageSizeCombo.SelectedItem is ComboBoxItem item &&
            int.TryParse(item.Content?.ToString(), out int pageSize))
        {
            cfg.PageSize = pageSize;
        }
        cfg.CandidateFontSize = (int)FontSizeSlider.Value;
        cfg.Save();

        bool learning = LearningCheck.IsChecked == true;
        bool big = BigDictCheck.IsChecked == true;
        bool tradition = TraditionCheck.IsChecked == true;
        bool bigChanged = big != EngineConf.ReadBigDict();
        bool traditionChanged = tradition != EngineConf.ReadTradition();
        EngineConf.WriteAll(learning, big, tradition, EngineConf.ReadPunct(), EngineConf.ReadWidth());

        // 大字库模式切换：通知引擎双缓冲热重载换词库
        //（默认 pinyin-plus.txt ↔ 大字库 pinyin-plus-big.txt，后台重建打字零阻塞）
        if (bigChanged)
        {
            bool reloaded = EnginePipeClient.ReloadUserDict();
            MessageBox.Show(reloaded
                ? "大字库模式已切换，词库热重载完成（生僻字即时可达）。"
                : "大字库模式已保存，但引擎暂不可达，将在下次引擎启动时生效。",
                "大字库模式", MessageBoxButton.OK, MessageBoxImage.Information);
        }

        // 繁体输出切换：type 15 消息即时生效（引擎无需重启/重载词库）
        if (traditionChanged)
        {
            bool sent = EnginePipeClient.SetTradition(tradition);
            if (!sent)
            {
                MessageBox.Show("繁体开关已保存，但引擎暂不可达，将在下次引擎启动时生效。",
                    "繁体输出", MessageBoxButton.OK, MessageBoxImage.Information);
            }
        }

        // 热生效：候选窗立即使用新配置
        (Application.Current as App)?.ApplyCandidateConfig();

        Close();
    }

    private void OnCloseClick(object sender, RoutedEventArgs e)
    {
        Close();
    }

    /// <summary>导出用户词库（备份/迁移）。</summary>
    private void OnExportDictClick(object sender, RoutedEventArgs e)
    {
        var dlg = new Microsoft.Win32.SaveFileDialog
        {
            Title = "导出 Nova 用户词库",
            Filter = "Nova 用户词库 (*.txt)|*.txt|所有文件 (*.*)|*.*",
            FileName = $"nova-userdict-{DateTime.Now:yyyyMMdd-HHmm}.txt",
        };
        if (dlg.ShowDialog() != true)
        {
            return;
        }
        try
        {
            if (!File.Exists(UserDictPath))
            {
                MessageBox.Show("用户词库尚为空（还没有产生用户词）。", "导出", MessageBoxButton.OK, MessageBoxImage.Information);
                return;
            }
            File.Copy(UserDictPath, dlg.FileName, true);
            MessageBox.Show($"已导出到：\n{dlg.FileName}", "导出成功", MessageBoxButton.OK, MessageBoxImage.Information);
        }
        catch (Exception ex)
        {
            MessageBox.Show($"导出失败：{ex.Message}", "错误", MessageBoxButton.OK, MessageBoxImage.Error);
        }
    }

    /// <summary>导入用户词库：与现有词库合并（同词累加词频）后写回，并通知引擎热重载。</summary>
    private void OnImportDictClick(object sender, RoutedEventArgs e)
    {
        var dlg = new Microsoft.Win32.OpenFileDialog
        {
            Title = "导入 Nova 用户词库",
            Filter = "Nova 用户词库 (*.txt)|*.txt|所有文件 (*.*)|*.*",
        };
        if (dlg.ShowDialog() != true)
        {
            return;
        }
        try
        {
            var merged = new Dictionary<(string Py, string Wd), (double Freq, string Initial)>();
            LoadDictFileInto(UserDictPath, merged);
            LoadDictFileInto(dlg.FileName, merged);
            if (merged.Count == 0)
            {
                MessageBox.Show("导入文件没有可识别的词条（格式：拼音\\t汉字\\t词频）。", "导入", MessageBoxButton.OK, MessageBoxImage.Warning);
                return;
            }
            WriteDictFile(UserDictPath, merged);

            bool reloaded = EnginePipeClient.ReloadUserDict();
            MessageBox.Show(reloaded
                ? $"已导入，词库已热重载（{merged.Count} 条词）。"
                : $"已写入词库文件（{merged.Count} 条词），但引擎暂不可达，下次启动自动加载。",
                "导入完成", MessageBoxButton.OK, MessageBoxImage.Information);
        }
        catch (Exception ex)
        {
            MessageBox.Show($"导入失败：{ex.Message}", "错误", MessageBoxButton.OK, MessageBoxImage.Error);
        }
    }

    private static void LoadDictFileInto(string path, Dictionary<(string Py, string Wd), (double Freq, string Initial)> merged)
    {
        if (string.IsNullOrEmpty(path) || !File.Exists(path))
        {
            return;
        }
        foreach (string line in File.ReadAllLines(path))
        {
            if (string.IsNullOrWhiteSpace(line))
            {
                continue;
            }
            string[] parts = line.Split('\t');
            if (parts.Length < 3)
            {
                continue;
            }
            string py = parts[0].Trim();
            string wd = parts[1].Trim();
            if (py.Length == 0 || wd.Length == 0)
            {
                continue;
            }
            double freq = double.TryParse(parts[2].Trim(), out double f) ? f : 1.0;
            string initial = parts.Length >= 4 ? parts[3].Trim() : "";
            var key = (py, wd);
            if (merged.TryGetValue(key, out var cur))
            {
                merged[key] = (cur.Freq + freq, cur.Initial.Length > 0 ? cur.Initial : initial);
            }
            else
            {
                merged[key] = (freq, initial);
            }
        }
    }

    private static void WriteDictFile(string path, Dictionary<(string Py, string Wd), (double Freq, string Initial)> merged)
    {
        var sb = new StringBuilder();
        foreach (var kv in merged.OrderBy(k => k.Key.Py, StringComparer.Ordinal).ThenByDescending(k => k.Value.Freq))
        {
            sb.Append(kv.Key.Py).Append('\t').Append(kv.Key.Wd).Append('\t').Append(kv.Value.Freq.ToString("F2"));
            if (kv.Value.Initial.Length > 0)
            {
                sb.Append('\t').Append(kv.Value.Initial);
            }
            sb.AppendLine();
        }
        // 原子写：临时文件 + 改名，避免引擎/其他进程读到半截内容
        string tmp = path + ".tmp";
        File.WriteAllText(tmp, sb.ToString(), new UTF8Encoding(false));
        File.Move(tmp, path, true);
    }
}
