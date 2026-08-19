using System.Runtime.InteropServices;
using System.Windows.Forms;

namespace PinyinPlus.Server;

/// <summary>
/// 候选窗定位模块：专业处理"光标位置 → 候选窗显示位置"的换算。
///
/// 输入：DLL 传来的光标屏幕坐标（物理像素，来自 TSF GetTextExt）。
/// 输出：WPF 候选窗的 Left/Top（DIP 单位）。
///
/// 要点：
///  - 用 Win32 GetDpiForMonitor 取光标所在屏幕的 DPI 做 物理像素→DIP 换算，
///    不依赖窗口渲染状态（PresentationSource 在窗口未显示时为 null，
///    旧实现会跳过换算导致高 DPI/副屏位置偏移——候选窗乱跑的根因）。
///  - 多屏：每个屏幕独立 DPI 与工作区，光标在哪个屏就按哪个屏约束。
///  - 边界：默认放在光标下方，底部放不下则翻到上方；不越出屏幕工作区。
/// </summary>
public static class CandidatePlacement
{
    private const double Padding = 8.0;       // 距屏幕边缘的最小间距（DIP）
    private const double OffsetBelow = 6.0;   // 距光标下方的间距（DIP）

    private const int MONITOR_DEFAULTTONEAREST = 2;
    private const int MDT_EFFECTIVE_DPI = 0;

    [StructLayout(LayoutKind.Sequential)]
    private struct POINT { public int X; public int Y; }

    [DllImport("user32.dll")]
    private static extern IntPtr MonitorFromPoint(POINT pt, int dwFlags);

    [DllImport("shcore.dll")]
    private static extern int GetDpiForMonitor(IntPtr hMonitor, int dpiType, out uint dpiX, out uint dpiY);

    /// <summary>
    /// 根据光标屏幕坐标（物理像素）与窗口尺寸（DIP），计算窗口显示位置（DIP）。
    /// </summary>
    public static (double Left, double Top) Compute(
        int cursorX, int cursorY, double windowWidth, double windowHeight)
    {
        // 1. 定位光标所在屏幕（工作区，物理像素）；找不到则回退主屏
        Screen screen = ScreenFromPoint(cursorX, cursorY) ?? Screen.PrimaryScreen!;
        var wa = screen.WorkingArea;

        // 2. 物理像素 → DIP：取光标所在屏幕的有效 DPI
        double scale = DpiScaleForPoint(cursorX, cursorY);
        double x = cursorX / scale;
        double y = cursorY / scale;
        double waLeft = wa.Left / scale;
        double waTop = wa.Top / scale;
        double waRight = wa.Right / scale;
        double waBottom = wa.Bottom / scale;

        // 3. 首选位置：光标下方（略错开，不遮住光标）
        double left = x;
        double top = y + OffsetBelow;

        // 4. 边界约束：水平放不下 → 左移；垂直放不下 → 翻到光标上方
        if (left + windowWidth > waRight)
        {
            left = waRight - windowWidth - Padding;
        }
        if (left < waLeft)
        {
            left = waLeft + Padding;
        }
        if (top + windowHeight > waBottom)
        {
            top = y - windowHeight - OffsetBelow;
        }
        if (top < waTop)
        {
            top = waTop + Padding;
        }

        return (left, top);
    }

    /// <summary>返回包含指定屏幕坐标（物理像素）的显示器；未命中返回 null。</summary>
    private static Screen? ScreenFromPoint(int x, int y)
    {
        foreach (Screen s in Screen.AllScreens)
        {
            var b = s.Bounds;
            if (x >= b.Left && x < b.Right && y >= b.Top && y < b.Bottom)
            {
                return s;
            }
        }
        return null;
    }

    /// <summary>取指定物理坐标所在屏幕的 DPI 缩放系数（GetDpiForMonitor，失败回退 1.0）。</summary>
    private static double DpiScaleForPoint(int x, int y)
    {
        try
        {
            IntPtr hMonitor = MonitorFromPoint(new POINT { X = x, Y = y }, MONITOR_DEFAULTTONEAREST);
            if (hMonitor != IntPtr.Zero &&
                GetDpiForMonitor(hMonitor, MDT_EFFECTIVE_DPI, out uint dpiX, out _) == 0 &&
                dpiX > 0)
            {
                return dpiX / 96.0;
            }
        }
        catch
        {
            // 回退 1.0（96 DPI）
        }
        return 1.0;
    }
}
