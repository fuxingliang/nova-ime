using System.Windows;

namespace PinyinPlus.Server;

/// <summary>
/// Nova 输入法 - 快捷键速查窗（深色主题，与设置面板同款）。
/// 单一数据源：快捷键表定义在 ShortcutsWindow.xaml——三个入口
/// （候选窗右键菜单 / 托盘菜单 / 设置面板按钮）都调用 Open() 复用同一窗。
/// 加新快捷键：改本窗 XAML + README「使用」章节，两处同步即可。
/// </summary>
public partial class ShortcutsWindow : Window
{
    private static ShortcutsWindow? _instance;

    /// <summary>单例：重复打开时激活已有窗口，避免堆积多个速查窗。</summary>
    public static ShortcutsWindow Open()
    {
        if (_instance is null || !_instance.IsLoaded)
        {
            _instance = new ShortcutsWindow();
            _instance.Closed += (_, _) => _instance = null;
        }
        _instance.Show();
        _instance.Activate();
        return _instance;
    }

    public ShortcutsWindow()
    {
        InitializeComponent();
    }

    private void OnCloseClick(object sender, RoutedEventArgs e)
    {
        Close();
    }
}
