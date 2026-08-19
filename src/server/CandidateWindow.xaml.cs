using System.Collections;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Media;

namespace PinyinPlus.Server;

public class CandidateItemViewModel
{
    public string Number { get; set; } = "";
    public string Text { get; set; } = "";
    public string Hint { get; set; } = "";
    public bool IsSelected { get; set; }
    public bool IsHovered { get; set; }
    public double FontSize { get; set; } = 15;
    public Brush TextBrush { get; set; } = Brushes.Transparent;
    public bool IsUser { get; set; }
    public Visibility UserBadgeVisibility => IsUser ? Visibility.Visible : Visibility.Collapsed;
}

public partial class CandidateWindow : Window
{
    private int _pageSize;               // 每页候选数（设置面板可调，热生效）
    private double _fontSize;            // 候选字大小（设置面板可调，热生效）
    private readonly List<string> _allCandidates = new();
    private int _pageIndex;                  // 当前页（从 0 起）

    // 状态条配色：青=开启（繁体/中文标点），灰=关闭
    private static readonly Brush AccentBrush = new SolidColorBrush(Color.FromRgb(0x55, 0xD8, 0xFF));
    private static readonly Brush MutedBrush = new SolidColorBrush(Color.FromRgb(0x7A, 0x82, 0x8E));

    // ---- 用户手动拖动偏移（DIP）：候选窗在 DLL 定位基础上叠加，右键"重置位置"清零 ----
    private double _dragOffsetX;
    private double _dragOffsetY;
    private bool _dragging;              // 拖动进行中：暂停 MoveToPoint 移动（否则被定位拉回）
    private Point _dragStartWindowPos;   // 按下时窗口位置
    private Point _dragStartMouse;       // 按下时鼠标位置
    private bool _dragMoved;             // 是否真拖动（位移超阈值；区分点击与拖动）
    private bool _hasLastPos;            // 是否收到过 DLL 定位点
    private int _lastPosX;               // 最近一次 DLL 定位点（物理像素）
    private int _lastPosY;
    private int _lastAppliedX = int.MinValue;   // 上次实际应用定位的 DLL 坐标（抖动抑制基准）
    private int _lastAppliedY = int.MinValue;
    private long _lastAppliedTick;       // 上次实际定位时间戳（限频：抗高频微抖）
    // 候选窗拖动偏移记忆（用户数据）→ 数据目录
    private static string OffsetFile => System.IO.Path.Combine(Paths.DataDir, "candidate_offset.json");

    /// <summary>鼠标点击某个候选（参数为全局候选索引）→ 由 App 回发 DLL 执行选字。</summary>
    public event Action<int>? CandidateClicked;

    /// <summary>右键菜单删除用户词 → 由 App 转发 DLL（type 6 → 引擎 type 13 删词）。</summary>
    public event Action<string>? DeleteUserWordRequested;

    /// <summary>右键菜单"降低排位" → 由 App 转发 DLL（type 8 → 引擎 type 16 降权沉底）。</summary>
    public event Action<string>? DemoteWordRequested;

    /// <summary>符号面板选择符号/文本 → 由 App 转发 DLL 插入编辑器。</summary>
    public event Action<string>? InsertTextRequested;

    /// <summary>点击 ❖ 按钮请求打开符号面板 → 由 App 显示 SymbolWindow。</summary>
    public event Action? OpenSymbolPanelRequested;

    public CandidateWindow()
    {
        InitializeComponent();
        ApplyConfig();
        LoadDragOffset();

        // 关键：候选窗必须"永不激活"——否则鼠标点击候选/翻页箭头时，
        // Windows 会把前台窗口切到候选窗 → 编辑器失焦 → TSF 组合被取消
        // → DLL 发 Hide → 候选窗消失、选字/翻页全失效。
        // 双保险：WS_EX_NOACTIVATE 窗口样式 + 拦截 WM_MOUSEACTIVATE 返回 MA_NOACTIVATE。
        SourceInitialized += (_, _) =>
        {
            var source = (HwndSource)PresentationSource.FromVisual(this)!;
            source.AddHook(WndProc);
            IntPtr exStyle = GetWindowLongPtrW(source.Handle, GWL_EXSTYLE);
            SetWindowLongPtrW(source.Handle, GWL_EXSTYLE,
                (IntPtr)((long)exStyle | WS_EX_NOACTIVATE));
        };
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

    /// <summary>重新读取配置并热应用到候选窗（设置面板保存后调用）。</summary>
    public void ApplyConfig()
    {
        _pageSize = AppConfig.Current.PageSize;
        _fontSize = AppConfig.Current.CandidateFontSize;
        RefreshStatusIndicators();
        if (IsVisible && CandidateList.ItemsSource is not null)
        {
            // 页大小可能变化：按当前选中项重算所在页
            int global = _pageIndex * Math.Max(1, _pageSize);
            _pageIndex = Math.Clamp(global / Math.Max(1, _pageSize), 0, PageCount - 1);
            RenderPage(Math.Min(global, Math.Max(0, _allCandidates.Count - 1)));
        }
    }

    // ---- 简繁输出 / 中英标点状态（engine.conf 持久化；候选窗状态条 + 右键菜单切换）----

    /// <summary>从 engine.conf 读取当前状态并刷新候选窗状态条（简/繁 + 中/英标点）。</summary>
    public void RefreshStatusIndicators()
    {
        bool tradition = EngineConf.ReadTradition();
        bool punct = EngineConf.ReadPunct();
        bool width = EngineConf.ReadWidth();

        TraditionBadge.Text = tradition ? "繁" : "简";
        TraditionBadge.Foreground = tradition ? AccentBrush : MutedBrush;

        PunctToggle.Text = punct ? "，" : ",";
        PunctToggle.Foreground = punct ? AccentBrush : MutedBrush;

        WidthToggle.Text = width ? "全" : "半";
        WidthToggle.Foreground = width ? AccentBrush : MutedBrush;
    }

    /// <summary>设置简繁输出（true=繁体）。写 engine.conf + 通知引擎 type 15 即时生效。</summary>
    private void SetTraditionMode(bool tradition)
    {
        if (tradition == EngineConf.ReadTradition())
        {
            return;
        }
        EngineConf.WriteAll(EngineConf.ReadLearning(), EngineConf.ReadBigDict(),
            tradition, EngineConf.ReadPunct(), EngineConf.ReadWidth());
        EnginePipeClient.SetTradition(tradition);   // 引擎即时生效（候选/上屏转繁体）
        RefreshStatusIndicators();
    }

    private void OnCtxSetSimplified(object sender, MouseButtonEventArgs e)
    {
        SetTraditionMode(false);
        CloseCtxMenu();
    }

    private void OnCtxSetTraditional(object sender, MouseButtonEventArgs e)
    {
        SetTraditionMode(true);
        CloseCtxMenu();
    }

    private void OnTraditionBadgeClicked(object sender, MouseButtonEventArgs e)
    {
        SetTraditionMode(!EngineConf.ReadTradition());
    }

    private void OnPunctToggleClicked(object sender, MouseButtonEventArgs e)
    {
        bool next = !EngineConf.ReadPunct();
        EngineConf.WriteAll(EngineConf.ReadLearning(), EngineConf.ReadBigDict(),
            EngineConf.ReadTradition(), next, EngineConf.ReadWidth());
        RefreshStatusIndicators();
        // DLL 各宿主进程打标点键时实时读 engine.conf，无需消息，下次按键立即生效
    }

    private void OnWidthToggleClicked(object sender, MouseButtonEventArgs e)
    {
        bool next = !EngineConf.ReadWidth();
        EngineConf.WriteAll(EngineConf.ReadLearning(), EngineConf.ReadBigDict(),
            EngineConf.ReadTradition(), EngineConf.ReadPunct(), next);
        RefreshStatusIndicators();
        // DLL 上屏/标点时实时读 engine.conf，无需消息，下次输入立即生效
    }

    /// <summary>展示候选。候选为空则隐藏窗口。</summary>
    public void ShowCandidates(string buffer, IReadOnlyList<string> candidates, int selectedIndex, int pageStart)
    {
        if (candidates.Count == 0)
        {
            Hide();
            return;
        }

        _allCandidates.Clear();
        _allCandidates.AddRange(candidates);

        BufferText.Text = buffer;
        bool hasBuffer = !string.IsNullOrEmpty(buffer);
        BufferRow.Visibility = hasBuffer ? Visibility.Visible : Visibility.Collapsed;

        // 根据选中项（全局索引）定位到所在页
        _pageIndex = Math.Clamp(selectedIndex / _pageSize, 0, PageCount - 1);
        RenderPage(selectedIndex);

        if (!IsVisible)
        {
            Show();
        }
    }

    public void UpdateSelection(int selectedIndex)
    {
        // 方向键翻页/移动时 DLL 发来全局索引，自动切换所在页并高亮
        int page = Math.Clamp(selectedIndex / _pageSize, 0, PageCount - 1);
        if (page != _pageIndex)
        {
            _pageIndex = page;
            RenderPage(selectedIndex);
            return;
        }
        ApplySelection(selectedIndex % _pageSize);
    }

    private int PageCount => Math.Max(1, (_allCandidates.Count + _pageSize - 1) / _pageSize);

    private void RenderPage(int globalSelectedIndex)
    {
        int start = _pageIndex * _pageSize;
        int count = Math.Min(_pageSize, _allCandidates.Count - start);

        var items = new List<CandidateItemViewModel>(count);
        for (int i = 0; i < count; i++)
        {
            items.Add(new CandidateItemViewModel
            {
                Number = NumberLabel(i + 1),
                Text = _allCandidates[start + i],
                Hint = "",
                FontSize = _fontSize,
                IsSelected = (start + i == globalSelectedIndex),
                IsUser = UserDictIndex.IsUserWord(_allCandidates[start + i]),
            });
        }
        CandidateList.ItemsSource = items;

        // 页码导航：❖ 符号面板入口常驻；◀/▶ 页码仅多页时显示
        PageIndicator.Text = $"{_pageIndex + 1}/{PageCount}";
        bool multi = PageCount > 1;
        PagerRow.Visibility = Visibility.Visible;
        PrevPageBtn.Visibility = multi ? Visibility.Visible : Visibility.Collapsed;
        NextPageBtn.Visibility = multi ? Visibility.Visible : Visibility.Collapsed;
        PageIndicator.Visibility = multi ? Visibility.Visible : Visibility.Collapsed;
    }

    private void ApplySelection(int indexInPage)
    {
        if (CandidateList.ItemsSource is not IList items)
            return;

        for (int i = 0; i < items.Count; i++)
        {
            if (items[i] is CandidateItemViewModel vm)
            {
                vm.IsSelected = (i == indexInPage);
            }
        }
        CandidateList.Items.Refresh();
    }

    /// <summary>鼠标滚轮翻页：向上滚=上一页，向下滚=下一页。翻页后高亮当前页第一项。</summary>
    private void OnPreviewMouseWheel(object sender, MouseWheelEventArgs e)
    {
        if (PageCount <= 1)
        {
            return;
        }

        int target = _pageIndex + (e.Delta > 0 ? -1 : 1);
        target = Math.Clamp(target, 0, PageCount - 1);
        if (target == _pageIndex)
        {
            return;
        }

        _pageIndex = target;
        int globalSelected = _pageIndex * _pageSize;   // 高亮当前页第一项
        RenderPage(globalSelected);
        e.Handled = true;
    }

    private static string NumberLabel(int index)
    {
        if (index >= 1 && index <= 9)
            return index.ToString();
        if (index == 10)
            return "0";
        return index.ToString();  // 超过 10 用后续数字
    }

    /// <summary>鼠标悬停候选：淡色高亮预览（不改变键盘选中态，点击上屏仍走原有链路）。</summary>
    private void OnItemMouseEnter(object sender, MouseEventArgs e)
    {
        if (sender is FrameworkElement fe && fe.DataContext is CandidateItemViewModel vm && !vm.IsHovered)
        {
            vm.IsHovered = true;
            CandidateList.Items.Refresh();
        }
    }

    private void OnItemMouseLeave(object sender, MouseEventArgs e)
    {
        if (sender is FrameworkElement fe && fe.DataContext is CandidateItemViewModel vm && vm.IsHovered)
        {
            vm.IsHovered = false;
            CandidateList.Items.Refresh();
        }
    }

    // ---- 右键菜单（复制 / 搜索 / 删除用户词）----
    private string? _ctxWord;      // 右键目标候选词
    private bool _ctxCanDelete;    // 目标是否为用户自定义词（★，可删除）

    /// <summary>右键候选：弹出浮动菜单（复制/搜索/删除/简繁切换）。</summary>
    private void OnItemRightButtonUp(object sender, MouseButtonEventArgs e)
    {
        if (sender is FrameworkElement fe && fe.DataContext is CandidateItemViewModel vm)
        {
            _ctxWord = vm.Text;
            _ctxCanDelete = vm.IsUser;
            CtxDeleteItem.Visibility = vm.IsUser ? Visibility.Visible : Visibility.Collapsed;

            // 简繁菜单项：当前模式青色高亮
            bool trad = EngineConf.ReadTradition();
            CtxSimplifiedItem.Foreground = trad ? MutedBrush : AccentBrush;
            CtxTraditionalItem.Foreground = trad ? AccentBrush : MutedBrush;

            Point p = e.GetPosition(this);
            // 菜单定位于鼠标处，向上展开（菜单较长，避免超出窗口底部被裁剪）
            const double menuH = 240;   // 估算菜单高度（复制/搜索/删除/降位/简繁两项/重置 + 分隔线）
            double mx = Math.Clamp(p.X, 0, Math.Max(0, ActualWidth - 130));
            double my = Math.Clamp(p.Y - menuH, 0, Math.Max(0, ActualHeight - menuH));
            CtxMenu.Margin = new Thickness(mx, my, 0, 0);
            CtxMenu.Visibility = Visibility.Visible;
            e.Handled = true;
        }
    }

    /// <summary>左键点击窗口其他区域 → 关闭右键菜单。</summary>
    private void OnMainGridMouseLeftDown(object sender, MouseButtonEventArgs e)
    {
        CloseCtxMenu();
    }

    /// <summary>左键按下（普通冒泡阶段）：空白区域启动拖动。候选词/翻页/❖/右键菜单排除。</summary>
    private void OnMainGridMouseDown(object sender, MouseButtonEventArgs e)
    {
        if (e.OriginalSource is DependencyObject src)
        {
            if (IsWithin(src, CandidateList) || IsWithin(src, PagerRow) || IsWithin(src, CtxMenu))
            {
                return;   // 交互元素：点击上屏/翻页/符号/右键菜单，不启动拖动
            }
        }
        StartDrag(sender, e);
    }

    private void OnDragHandleDown(object sender, MouseButtonEventArgs e)
    {
        CloseCtxMenu();
        StartDrag(sender, e);
    }

    /// <summary>启动拖动（纯光标模式）：不显示幽灵卡，只切换十字光标反馈；
    /// 原窗口保持不动，松手时一次性定位到鼠标位置。</summary>
    private void StartDrag(object sender, MouseButtonEventArgs e)
    {
        if (_dragging)
        {
            // 防呆：_dragging 卡死残留（LostMouseCapture 未兜住的情形）且当前无任何
            // 鼠标捕获 → 复位后继续；否则视为真拖动中/捕获被占，忽略本次请求。
            if (Mouse.Captured != null) return;
            _dragging = false;
            Cursor = null;
        }
        _dragging = true;
        _dragMoved = false;
        _dragStartWindowPos = new Point(Left, Top);
        _dragStartMouse = e.GetPosition(this);
        Cursor = Cursors.SizeAll;   // 拖动反馈：十字光标（窗口级覆盖全部子元素）
        if (sender is UIElement ue)
        {
            ue.CaptureMouse();
        }
        e.Handled = true;
    }

    private void OnMainGridMouseMove(object sender, MouseEventArgs e)
    {
        if (!_dragging) return;
        // 区分"点击"与"拖动"：鼠标位移超过阈值才算真拖动（点击空白不移动窗口）
        if (!_dragMoved)
        {
            Point p = e.GetPosition(this);
            if (Math.Abs(p.X - _dragStartMouse.X) > 6 || Math.Abs(p.Y - _dragStartMouse.Y) > 6)
            {
                _dragMoved = true;
            }
        }
        e.Handled = true;
    }

    private void OnMainGridMouseUp(object sender, MouseButtonEventArgs e)
    {
        if (!_dragging) return;
        _dragging = false;
        Cursor = null;   // 恢复默认光标
        if (sender is UIElement ue)
        {
            ue.ReleaseMouseCapture();
        }

        if (!_dragMoved)
        {
            // 点击空白（未拖动）：只关菜单，不移动窗口
            CloseCtxMenu();
            e.Handled = true;
            return;
        }

        // 真拖动：候选窗以鼠标为中心一次性就位（窗口拖动全程未移动）
        Point p = e.GetPosition(this);
        Left = (Left + p.X) - ActualWidth / 2;
        Top = (Top + p.Y) - ActualHeight / 2;

        // 偏移 = 窗口当前位置 - DLL 定位位置（基于最近一次定位点重算基准）
        if (_hasLastPos)
        {
            var (baseLeft, baseTop) = CandidatePlacement.Compute(
                _lastPosX, _lastPosY, ActualWidth, ActualHeight);
            _dragOffsetX = Left - baseLeft;
            _dragOffsetY = Top - baseTop;
            SaveDragOffset();
        }
        e.Handled = true;
    }

    /// <summary>鼠标捕获丢失兜底：任何原因（松手落在候选词/翻页按钮上被 Preview 吞掉
    /// MouseUp、候选上屏窗口隐藏、右键菜单 Popup 抢捕获等）导致 MouseUp 复位逻辑
    /// 永远不执行时，捕获释放必然触发本事件 → 强制复位拖动状态。
    /// 否则 _dragging 卡死为 true → 之后永远拖不动、窗口也不跟随光标。</summary>
    private void OnMainGridLostMouseCapture(object sender, MouseEventArgs e)
    {
        if (_dragging)
        {
            _dragging = false;
            Cursor = null;
        }
    }

    /// <summary>node 是否为 root 或其视觉子树成员（用于排除交互元素）。</summary>
    private static bool IsWithin(DependencyObject node, DependencyObject root)
    {
        while (node is not null)
        {
            if (node == root) return true;
            node = VisualTreeHelper.GetParent(node);
        }
        return false;
    }

    private void OnCtxCopy(object sender, MouseButtonEventArgs e)
    {
        if (!string.IsNullOrEmpty(_ctxWord))
        {
            try { Clipboard.SetText(_ctxWord); } catch { }
        }
        CloseCtxMenu();
    }

    private void OnCtxSearch(object sender, MouseButtonEventArgs e)
    {
        if (!string.IsNullOrEmpty(_ctxWord))
        {
            try
            {
                Process.Start(new ProcessStartInfo(
                    "https://www.bing.com/search?q=" + Uri.EscapeDataString(_ctxWord))
                { UseShellExecute = true });
            }
            catch { }
        }
        CloseCtxMenu();
    }

    private void OnCtxDelete(object sender, MouseButtonEventArgs e)
    {
        CloseCtxMenu();
        if (_ctxCanDelete && !string.IsNullOrEmpty(_ctxWord))
        {
            DeleteUserWord(_ctxWord);   // 实现见下方（协议扩展后启用）
        }
    }

    /// <summary>右键"降低排位"：目标词降权沉底（词库自带词/用户词均可），由 App 转发 DLL → 引擎。</summary>
    private void OnCtxDemote(object sender, MouseButtonEventArgs e)
    {
        CloseCtxMenu();
        if (!string.IsNullOrEmpty(_ctxWord))
        {
            DemoteWordRequested?.Invoke(_ctxWord);
        }
    }

    private void CloseCtxMenu()
    {
        CtxMenu.Visibility = Visibility.Collapsed;
    }

    /// <summary>向 DLL 发删除用户词命令（右键菜单：误造词直接删，type 6）。</summary>
    private void DeleteUserWord(string word) => DeleteUserWordRequested?.Invoke(word);

    /// <summary>鼠标左键点击候选：换算全局索引并回发 DLL（等效按数字键选字）。</summary>
    private void OnCandidatePreviewMouseUp(object sender, MouseButtonEventArgs e)
    {
        Log($"Click entered: sender={sender.GetType().Name} src={e.OriginalSource?.GetType().Name}");

        if (CandidateList.ItemsSource is not IList items)
        {
            Log("Click: ItemsSource not IList");
            return;
        }

        // 找到被点击项（ItemsControl 的容器 → DataContext）
        if (e.OriginalSource is not DependencyObject src)
        {
            Log("Click: OriginalSource not DO");
            return;
        }
        var container = CandidateList.ContainerFromElement(src);
        Log($"Click: container={container?.GetType().Name ?? "null"}");
        if (container is not FrameworkElement fe || fe.DataContext is not CandidateItemViewModel vm)
        {
            Log("Click: no ViewModel found");
            return;
        }

        int indexInPage = items.IndexOf(vm);
        if (indexInPage < 0)
        {
            Log($"Click: indexInPage={indexInPage} not found");
            return;
        }

        int globalIndex = _pageIndex * _pageSize + indexInPage;
        Log($"Click: invoking CandidateClicked global={globalIndex}");
        CandidateClicked?.Invoke(globalIndex);
        e.Handled = true;
    }

    /// <summary>翻页箭头：上一页。</summary>
    private void OnPrevPageClicked(object sender, MouseButtonEventArgs e)
    {
        if (PageCount <= 1)
            return;
        _pageIndex = Math.Max(0, _pageIndex - 1);
        RenderPage(_pageIndex * _pageSize);
        e.Handled = true;
    }

    /// <summary>翻页箭头：下一页。</summary>
    private void OnNextPageClicked(object sender, MouseButtonEventArgs e)
    {
        if (PageCount <= 1)
            return;
        _pageIndex = Math.Min(PageCount - 1, _pageIndex + 1);
        RenderPage(_pageIndex * _pageSize);
        e.Handled = true;
    }

    /// <summary>❖ 符号面板入口。</summary>
    private void OnOpenSymbolPanel(object sender, MouseButtonEventArgs e)
    {
        OpenSymbolPanelRequested?.Invoke();
        e.Handled = true;
    }

    private static readonly object ClickLogLock = new();
    private static void Log(string msg)
    {
        try
        {
            lock (ClickLogLock)
            {
                File.AppendAllText(System.IO.Path.Combine(Paths.DataDir, "server_debug.log"),
                    $"{DateTime.Now:HH:mm:ss.fff} {msg}\r\n");
            }
        }
        catch { }
    }

    /// <summary>位置抖动抑制阈值（像素，DLL 原始坐标差）：光标移动小于该值不重定位。</summary>
    private const double MoveThreshold = 8.0;

    /// <summary>重定位限频：40ms 内最多应用一次位置（即使坐标在抖，窗口也不会高频跳动）。</summary>
    private const long MinRepositionIntervalMs = 40;

    /// <summary>把窗口移到光标位置（C++ 端为物理像素，由定位模块统一换算 DIP 并约束屏幕）。</summary>
    public void MoveToPoint(int x, int y)
    {
        _lastPosX = x;
        _lastPosY = y;
        _hasLastPos = true;

        // 用户正在拖动窗口：只记录 DLL 定位点，不移动（否则每次 SetPosition 都把窗口拉回）
        if (_dragging)
        {
            return;
        }

        // 抖动抑制 + 限频：以 DLL 原始定位点（光标）为基准。
        // 候选内容变化 → SizeToContent 窗口尺寸变化 → 贴边约束重算的位置会跳变；
        // 且游戏里 GetTextExt 坐标可能高频微跳。因此：
        //  1) 40ms 限频窗口内一律不重定位（位置更新频率上限 ~25Hz）；
        //  2) 限频外光标坐标差 < 阈值（8px）也不重定位。
        // 结果：静止输入（即使尺寸伸缩/坐标微抖）窗口纹丝不动，光标真移动才跟随。
        long now = Environment.TickCount64;
        if (_lastAppliedX != int.MinValue &&
            (now - _lastAppliedTick < MinRepositionIntervalMs ||
             (Math.Abs(x - _lastAppliedX) < MoveThreshold && Math.Abs(y - _lastAppliedY) < MoveThreshold)))
        {
            return;
        }

        var (left, top) = CandidatePlacement.Compute(x, y, ActualWidth, ActualHeight);
        left += _dragOffsetX;
        top += _dragOffsetY;

        Left = left;
        Top = top;
        _lastAppliedX = x;
        _lastAppliedY = y;
        _lastAppliedTick = now;
    }

    // ---- 拖动调整位置（记忆偏移，重启保留；右键"重置位置"清零）----

    private void OnDragHandleEnter(object sender, MouseEventArgs e)
    {
        if (sender is Border b)
        {
            b.Background = new SolidColorBrush(Color.FromRgb(0x30, 0x3D, 0x4E));
        }
    }

    private void OnDragHandleLeave(object sender, MouseEventArgs e)
    {
        if (sender is Border b)
        {
            b.Background = new SolidColorBrush(Color.FromRgb(0x1A, 0x1A, 0x1A));
        }
    }

    /// <summary>右键菜单：重置位置（清除用户拖动偏移，候选窗强制回到光标定位）。</summary>
    private void OnCtxResetPosition(object sender, MouseButtonEventArgs e)
    {
        CloseCtxMenu();
        _dragOffsetX = 0;
        _dragOffsetY = 0;
        SaveDragOffset();
        if (_hasLastPos)
        {
            // 强制移动：不走 MoveToPoint 的抖动抑制（否则与当前位置差 <5px 时不生效）
            var (left, top) = CandidatePlacement.Compute(_lastPosX, _lastPosY, ActualWidth, ActualHeight);
            Left = left;
            Top = top;
        }
    }

    private sealed class DragOffsetData
    {
        public double X { get; set; }
        public double Y { get; set; }
    }

    private void LoadDragOffset()
    {
        try
        {
            if (File.Exists(OffsetFile))
            {
                var d = JsonSerializer.Deserialize<DragOffsetData>(File.ReadAllText(OffsetFile));
                if (d is not null)
                {
                    _dragOffsetX = d.X;
                    _dragOffsetY = d.Y;
                }
            }
        }
        catch { }
    }

    private void SaveDragOffset()
    {
        try
        {
            File.WriteAllText(OffsetFile,
                JsonSerializer.Serialize(new DragOffsetData { X = _dragOffsetX, Y = _dragOffsetY }));
        }
        catch { }
    }
}
