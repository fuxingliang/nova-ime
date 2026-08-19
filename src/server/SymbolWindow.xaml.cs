using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Media;

namespace PinyinPlus.Server;

/// <summary>
/// 符号面板：分类浏览符号表（标点/数学/货币/箭头/图形/特殊/emoji），
/// 点击符号经 App → DLL(type 7) 插入编辑器光标处。
/// 窗口"永不激活"（WS_EX_NOACTIVATE）：点击不抢焦点 → 编辑器 TSF 上下文保持 → 插入可靠。
/// </summary>
public partial class SymbolWindow : Window
{
    /// <summary>点击符号 → 由 App 转发 DLL 插入编辑器。</summary>
    public event Action<string>? InsertTextRequested;

    private readonly List<SymbolData.SymbolCategory> _categories;
    private Button? _activeCatButton;

    public SymbolWindow()
    {
        InitializeComponent();

        _categories = (List<SymbolData.SymbolCategory>)SymbolData.Load();
        BuildCategoryTabs();
        ShowCategory(0);

        // 永不激活（同候选窗，保证编辑器不失焦）
        SourceInitialized += (_, _) =>
        {
            var source = (HwndSource)PresentationSource.FromVisual(this)!;
            source.AddHook(WndProc);
            IntPtr exStyle = GetWindowLongPtrW(source.Handle, GWL_EXSTYLE);
            SetWindowLongPtrW(source.Handle, GWL_EXSTYLE, (IntPtr)((long)exStyle | WS_EX_NOACTIVATE));
        };
    }

    private void BuildCategoryTabs()
    {
        for (int i = 0; i < _categories.Count; i++)
        {
            string name = _categories[i].Name;
            int idx = i;
            var btn = new Button
            {
                Style = (Style)FindResource("CatButton"),
                Content = name,
                Tag = idx,
            };
            btn.Click += (_, _) => ShowCategory(idx);
            CatRow.Children.Add(btn);
        }
    }

    private void ShowCategory(int index)
    {
        if (index < 0 || index >= _categories.Count)
            return;

        SymbolPanel.Children.Clear();
        var cat = _categories[index];
        foreach (string symbol in cat.Symbols)
        {
            var item = new Border
            {
                CornerRadius = new CornerRadius(6),
                Padding = new Thickness(6, 2, 6, 2),
                Margin = new Thickness(1, 1, 1, 1),
                Background = Brushes.Transparent,
                Child = new TextBlock
                {
                    Text = symbol,
                    FontSize = 16,
                    Foreground = new SolidColorBrush(Color.FromRgb(0xE6, 0xEB, 0xEF)),
                },
                Cursor = Cursors.Hand,
            };
            item.MouseEnter += (_, _) =>
                item.Background = new SolidColorBrush(Color.FromRgb(0x1E, 0x2A, 0x38));
            item.MouseLeave += (_, _) =>
                item.Background = Brushes.Transparent;
            item.MouseLeftButtonUp += (_, _) =>
                InsertTextRequested?.Invoke(symbol);   // 面板保持打开，可连续插入
            SymbolPanel.Children.Add(item);
        }

        // 高亮当前分类：青底白字 vs 透明灰字
        if (_activeCatButton is not null)
        {
            _activeCatButton.Background = Brushes.Transparent;
            _activeCatButton.Foreground = new SolidColorBrush(Color.FromRgb(0x9A, 0xA7, 0xB4));
        }
        if (CatRow.Children.Count > index && CatRow.Children[index] is Button b)
        {
            b.Background = new SolidColorBrush(Color.FromRgb(0x1E, 0x2A, 0x38));
            b.Foreground = new SolidColorBrush(Color.FromRgb(0x55, 0xD8, 0xFF));
            _activeCatButton = b;
        }
    }

    private void OnCloseClicked(object sender, MouseButtonEventArgs e) => Hide();

    // ---------- 鼠标拖动（手动捕获，不受 WS_EX_NOACTIVATE 影响） ----------
    private bool _dragging;
    private Point _dragStartPos;

    private void OnHeaderMouseLeftDown(object sender, MouseButtonEventArgs e)
    {
        // 关闭按钮区域不触发拖动
        if (e.OriginalSource is DependencyObject src && IsDescendantOf(CloseBtn, src))
            return;
        _dragging = true;
        _dragStartPos = e.GetPosition(this);
        ((UIElement)sender).CaptureMouse();
        e.Handled = true;
    }

    private void OnHeaderMouseMove(object sender, MouseEventArgs e)
    {
        if (!_dragging) return;
        Point p = e.GetPosition(this);
        Left += p.X - _dragStartPos.X;
        Top += p.Y - _dragStartPos.Y;
    }

    private void OnHeaderMouseUp(object sender, MouseButtonEventArgs e)
    {
        if (!_dragging) return;
        _dragging = false;
        ((UIElement)sender).ReleaseMouseCapture();
    }

    private static bool IsDescendantOf(DependencyObject root, DependencyObject? node)
    {
        while (node is not null)
        {
            if (node == root) return true;
            node = VisualTreeHelper.GetParent(node);
        }
        return false;
    }

    private const int GWL_EXSTYLE = -20;
    private const long WS_EX_NOACTIVATE = 0x08000000;
    private const int WM_MOUSEACTIVATE = 0x0021;
    private const int MA_NOACTIVATE = 3;

    [DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW")]
    private static extern IntPtr GetWindowLongPtrW(IntPtr hWnd, int nIndex);

    [DllImport("user32.dll", EntryPoint = "SetWindowLongPtrW")]
    private static extern IntPtr SetWindowLongPtrW(IntPtr hWnd, int nIndex, IntPtr dwNewLong);

    private static IntPtr WndProc(IntPtr hwnd, int msg, IntPtr wParam, IntPtr lParam, ref bool handled)
    {
        if (msg == WM_MOUSEACTIVATE)
        {
            handled = true;
            return new IntPtr(MA_NOACTIVATE);
        }
        return IntPtr.Zero;
    }
}
