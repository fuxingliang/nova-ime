// THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
// PARTICULAR PURPOSE.
//
// Copyright (c) Microsoft Corporation. All rights reserved

#include "Private.h"
#include "TfTextLayoutSink.h"
#include "SampleIME.h"
#include "GetTextExtentEditSession.h"
#include "PinyinIpc.h"

// ---------------------------------------------------------------------------
// 位置锚点进程级全局缓存（2026-08-20 修复 Chromium 系宿主候选窗跳动）
//
// Chromium 系应用（Trae/Qoder 等）GetTextExt 间歇失败（异步布局 + 自绘光标无
// 系统 caret），直接落到鼠标光标兜底会让锚点在"文本位置"与"鼠标位置"之间
// 交替 → 候选窗每按键来回跳。
//
// ★ 必须进程级全局（而非挂在 sink 对象上）：乱打拼音会触发组合结束/重建
// （_EndCandidateList → _EndLayout + 新 presenter 新 sink），对象级缓存随之
// 清空 → 重建后首次查询失败即落到鼠标（ime_debug.log 实证：cached=0 时
// SendPos 跳到鼠标位置，cached=1 时窗口纹丝不动）。
// 全局缓存跨重建存活：查询失败时复用新鲜缓存（窗口不动），仅缓存空/过期
// 才用鼠标兜底（此时用户刚点进来，鼠标≈光标处）。
// ★ 缓存只存"真实文本坐标"（TSF 查询成功/插入符回退所得），鼠标兜底不写
// 缓存：否则乱打拼音触发失败时锚点被污染成鼠标位置（ime_debug.log 实证
// 窗口跳到鼠标 (3346,1289)，而真实文本在 (2859,1803)）。
//
// 线程模型：TSF 布局/查询回调都走宿主 UI 线程（每进程一个），无跨线程竞争，
// 故不加锁。TTL 2s：限制切字段/切窗口后陈旧锚点的影响，且下次成功查询即
// 自动刷新（自纠正）。
// ---------------------------------------------------------------------------
struct ExtAnchorCache
{
    RECT rect;      // 物理像素锚点（与 SendSetPosition 同坐标系）
    DWORD tick;     // GetTickCount 时间戳
    BOOL valid;
};

static ExtAnchorCache g_extAnchor = { { 0, 0, 0, 0 }, 0, FALSE };
static const DWORD kAnchorTtlMs = 2000;

CTfTextLayoutSink::CTfTextLayoutSink(_In_ CSampleIME *pTextService)
{
    _pTextService = pTextService;
    _pTextService->AddRef();

    _pRangeComposition = nullptr;
    _pContextDocument = nullptr;
    _tfEditCookie = TF_INVALID_EDIT_COOKIE;

    _dwCookieTextLayoutSink = TF_INVALID_COOKIE;

    _refCount = 1;

    _suppressPositionUpdate = FALSE;

    DllAddRef();
}

CTfTextLayoutSink::~CTfTextLayoutSink()
{
    if (_pTextService)
    {
        _pTextService->Release();
    }

    DllRelease();
}

STDAPI CTfTextLayoutSink::QueryInterface(REFIID riid, _Outptr_ void **ppvObj)
{
    if (ppvObj == nullptr)
    {
        return E_INVALIDARG;
    }

    *ppvObj = nullptr;

    if (IsEqualIID(riid, IID_IUnknown) ||
        IsEqualIID(riid, IID_ITfTextLayoutSink))
    {
        *ppvObj = (ITfTextLayoutSink *)this;
    }

    if (*ppvObj)
    {
        AddRef();
        return S_OK;
    }

    return E_NOINTERFACE;
}

STDAPI_(ULONG) CTfTextLayoutSink::AddRef()
{
    return ++_refCount;
}

STDAPI_(ULONG) CTfTextLayoutSink::Release()
{
    LONG cr = --_refCount;

    assert(_refCount >= 0);

    if (_refCount == 0)
    {
        delete this;
    }

    return cr;
}

//+---------------------------------------------------------------------------
//
// ITfTextLayoutSink::OnLayoutChange
//
//----------------------------------------------------------------------------

STDAPI CTfTextLayoutSink::OnLayoutChange(_In_ ITfContext *pContext, TfLayoutCode lcode, _In_ ITfContextView *pContextView)
{
    // we're interested in only document context.
    if (pContext != _pContextDocument)
    {
        return S_OK;
    }

    switch (lcode)
    {
    case TF_LC_CHANGE:
        {
            CGetTextExtentEditSession* pEditSession = nullptr;
            pEditSession = new (std::nothrow) CGetTextExtentEditSession(_pTextService, pContext, pContextView, _pRangeComposition, this);
            if (nullptr != (pEditSession))
            {
                HRESULT hr = S_OK;
                pContext->RequestEditSession(_pTextService->_GetClientId(), pEditSession, TF_ES_SYNC | TF_ES_READ, &hr);

                pEditSession->Release();
            }
        }
        break;

    case TF_LC_DESTROY:
        _LayoutDestroyNotification();
        break;

    }
    return S_OK;
}

HRESULT CTfTextLayoutSink::_StartLayout(_In_ ITfContext *pContextDocument, TfEditCookie ec, _In_ ITfRange *pRangeComposition)
{
    _pContextDocument = pContextDocument;
    _pContextDocument->AddRef();

    _pRangeComposition = pRangeComposition;
    _pRangeComposition->AddRef();

    _tfEditCookie = ec;

    return _AdviseTextLayoutSink();
}

VOID CTfTextLayoutSink::_EndLayout()
{
    if (_pRangeComposition)
    {
        _pRangeComposition->Release();
        _pRangeComposition = nullptr;
    }

    if (_pContextDocument)
    {
        _UnadviseTextLayoutSink();
        _pContextDocument->Release();
        _pContextDocument = nullptr;
    }

    // 注：锚点缓存（g_extAnchor）是进程级全局，刻意不在此处清除——
    // 组合结束/重建（乱打拼音触发）后新 sink 首次查询失败时仍需复用它，
    // 否则会落到鼠标兜底导致候选窗跳走。陈旧锚点由 2s TTL + 下次成功
    // 查询刷新双保险兜住（提交后继续打字 → 新查询成功 → 缓存立即更新）。
}

HRESULT CTfTextLayoutSink::_AdviseTextLayoutSink()
{
    HRESULT hr = S_OK;
    ITfSource* pSource = nullptr;

    hr = _pContextDocument->QueryInterface(IID_ITfSource, (void **)&pSource);
    if (FAILED(hr))
    {
        return hr;
    }

    hr = pSource->AdviseSink(IID_ITfTextLayoutSink, (ITfTextLayoutSink *)this, &_dwCookieTextLayoutSink);
    if (FAILED(hr))
    {
        pSource->Release();
        return hr;
    }

    pSource->Release();

    return hr;
}

HRESULT CTfTextLayoutSink::_UnadviseTextLayoutSink()
{
    HRESULT hr = S_OK;
    ITfSource* pSource = nullptr;

    if (nullptr == _pContextDocument)
    {
        return E_FAIL;
    }

    hr = _pContextDocument->QueryInterface(IID_ITfSource, (void **)&pSource);
    if (FAILED(hr))
    {
        return hr;
    }

    hr = pSource->UnadviseSink(_dwCookieTextLayoutSink);
    if (FAILED(hr))
    {
        pSource->Release();
        return hr;
    }

    pSource->Release();

    return hr;
}

HRESULT CTfTextLayoutSink::_GetTextExt(_Out_ RECT *lpRect)
{
    HRESULT hr = S_OK;
    BOOL isClipped = TRUE;
    BOOL usedFallback = FALSE;
    ITfContextView* pContextView = nullptr;

    hr = _pContextDocument->GetActiveView(&pContextView);
    if (FAILED(hr))
    {
        return hr;
    }

    hr = pContextView->GetTextExt(_tfEditCookie, _pRangeComposition, lpRect, &isClipped);

    // 空组合（拼音不写入编辑区的搜狗式隐藏模式）下 GetTextExt 可能失败
    // 或返回零矩形：回退用当前插入符（selection）位置定位候选窗，
    // 保证候选窗始终跟住光标。
    if (FAILED(hr) ||
        (lpRect->right == lpRect->left && lpRect->bottom == lpRect->top))
    {
        TF_SELECTION sel;
        ULONG fetched = 0;
        if (SUCCEEDED(_pContextDocument->GetSelection(_tfEditCookie, TF_DEFAULT_SELECTION, 1, &sel, &fetched)) && fetched == 1)
        {
            hr = pContextView->GetTextExt(_tfEditCookie, sel.range, lpRect, &isClipped);
            sel.range->Release();
            usedFallback = TRUE;
        }
    }

    if (FAILED(hr))
    {
        // 最后一层兜底：应用 TSF 布局不可用（UE 等游戏常返回 TF_E_NOLAYOUT）
        // 且 selection 也取不到时，直接读焦点窗口的系统插入符（caret）位置。
        HWND hwnd = nullptr;
        if (SUCCEEDED(pContextView->GetWnd(&hwnd)) && hwnd != nullptr)
        {
            DWORD tid = GetWindowThreadProcessId(hwnd, nullptr);
            GUITHREADINFO gti = { sizeof(GUITHREADINFO) };
            if (tid != 0 && GetGUIThreadInfo(tid, &gti) &&
                (gti.flags & GUI_CARETBLINKING) && gti.hwndCaret != nullptr)
            {
                POINT pt = { gti.rcCaret.left, gti.rcCaret.top };
                ClientToScreen(hwnd, &pt);   // rcCaret 是客户区坐标
                lpRect->left = pt.x;
                lpRect->top = pt.y;
                lpRect->right = pt.x + (gti.rcCaret.right - gti.rcCaret.left);
                lpRect->bottom = pt.y + (gti.rcCaret.bottom - gti.rcCaret.top);
                hr = S_OK;
                usedFallback = TRUE;
            }
        }
    }

    // 最终兜底：Chromium 系应用（Trae/Qoder 等）自绘光标，系统 caret 不可用
    // （GetGUIThreadInfo 取不到 GUI_CARETBLINKING），且 GetTextExt/GetSelection
    // 均返回 TF_E_NOLAYOUT（0x80040201）。
    // 2026-08-20 修复"Chromium 宿主候选窗跳动"（对象级缓存被组合重建清空后
    // 仍落到鼠标 → 乱打拼音时窗口跳走）：改用进程级全局锚点缓存 g_extAnchor，
    // 跨 sink/presenter 重建存活。查询失败时：
    //   1) 有新鲜（TTL 内）锚点缓存 → 复用缓存（窗口纹丝不动）；
    //   2) 缓存空/过期 → 用鼠标（此时用户刚点进来，鼠标≈光标处，定位正确）。
    //      鼠标仅作单次定位，绝不写入缓存——否则锚点被污染成鼠标位置，
    //      乱打拼音触发失败时窗口会跳向鼠标（20:51 日志实证：跳到 (3346,1289)）。
    BOOL cursorFallback = FALSE;
    BOOL usedCached = FALSE;
    if (FAILED(hr))
    {
        POINT cur = { 0, 0 };
        DWORD now = GetTickCount();
        if (g_extAnchor.valid && (now - g_extAnchor.tick) < kAnchorTtlMs)
        {
            *lpRect = g_extAnchor.rect;
            hr = S_OK;
            usedFallback = TRUE;
            usedCached = TRUE;
            CPinyinIpc::DebugLog(L"Layout: GetTextExt anchor-cached rect=(%d,%d)-(%d,%d) age=%lums",
                lpRect->left, lpRect->top, lpRect->right, lpRect->bottom,
                now - g_extAnchor.tick);
        }
        else if (GetCursorPos(&cur))
        {
            lpRect->left = cur.x;
            lpRect->top = cur.y;
            lpRect->right = cur.x + 4;
            lpRect->bottom = cur.y + 20;
            hr = S_OK;
            usedFallback = TRUE;
            cursorFallback = TRUE;
            CPinyinIpc::DebugLog(L"Layout: GetTextExt cursor-fallback x=%d y=%d (no anchor)", cur.x, cur.y);
        }
    }

    if (FAILED(hr))
    {
        CPinyinIpc::DebugLog(L"Layout: GetTextExt FAILED hr=0x%08x fallback=%d", hr, usedFallback);
    }
    else
    {
        CPinyinIpc::DebugLog(L"Layout: GetTextExt raw=(%d,%d)-(%d,%d) fallback=%d cursor=%d cached=%d",
            lpRect->left, lpRect->top, lpRect->right, lpRect->bottom,
            usedFallback, cursorFallback, usedCached);
    }

    // 将 TSF 坐标统一换算为"物理屏幕像素"（Server 端按物理像素换算 DIP）。
    // 游戏（如虚幻引擎）多为 DPI unaware，Windows 虚拟化坐标需在此还原，
    // 否则候选窗与光标相差一个 DPI 缩放偏移。
    // 鼠标光标兜底已是物理像素，跳过换算；缓存复用也已是物理像素，
    // 同样跳过（避免二次换算）。
    if (SUCCEEDED(hr) && !cursorFallback && !usedCached)
    {
        _NormalizeToPhysical(pContextView, lpRect);
    }

    // 成功取到真实 TSF 坐标（物理像素换算后）→ 写入全局锚点缓存，供下次
    // 查询失败时复用（缓存复用/鼠标兜底不重复写入，避免 TTL 被无限续期）。
    if (SUCCEEDED(hr) && !cursorFallback && !usedCached)
    {
        g_extAnchor.rect = *lpRect;
        g_extAnchor.tick = GetTickCount();
        g_extAnchor.valid = TRUE;
    }

    pContextView->Release();

    return hr;
}

//+---------------------------------------------------------------------------
//
// _NormalizeToPhysical
//
// 把 TSF GetTextExt 返回的屏幕坐标统一换算为"物理屏幕像素"。
//
// 背景：TSF 规范要求 GetTextExt 返回屏幕坐标，但坐标所在的"空间"随应用
// DPI 感知度不同：
//   - PerMonitor(V2)：应用直接使用物理像素 → 无需换算。
//   - SystemAware：应用在"系统 DPI"空间 → 物理 = 坐标 × 屏幕DPI/系统DPI。
//   - DPI unaware（多数游戏，如虚幻引擎）：Windows 对其窗口做 DPI 虚拟化，
//     上报的是 96 DPI 的虚拟化坐标 → 必须用 LogicalToPhysicalPointForPerMonitorDPI
//     还原成物理坐标。
//
// 候选窗服务（PerMonitorV2）按"物理像素"解释 DLL 传来的坐标并换算 DIP，
// 若这里不统一，游戏里候选窗就会与光标相差一个 DPI 缩放偏移。
//
//----------------------------------------------------------------------------

VOID CTfTextLayoutSink::_NormalizeToPhysical(_In_ ITfContextView *pContextView, _Inout_ RECT *lpRect)
{
    HWND hwnd = nullptr;
    if (FAILED(pContextView->GetWnd(&hwnd)) || hwnd == nullptr)
    {
        return;
    }

    DPI_AWARENESS_CONTEXT ctx = GetWindowDpiAwarenessContext(hwnd);
    DPI_AWARENESS awareness = GetAwarenessFromDpiAwarenessContext(ctx);

    switch (awareness)
    {
    case DPI_AWARENESS_UNAWARE:
    {
        // DPI 虚拟化：逻辑（虚拟化）坐标 → 物理坐标（按窗口所在屏幕比例放大）
        POINT ptTL = { lpRect->left, lpRect->top };
        POINT ptBR = { lpRect->right, lpRect->bottom };
        LogicalToPhysicalPointForPerMonitorDPI(hwnd, &ptTL);
        LogicalToPhysicalPointForPerMonitorDPI(hwnd, &ptBR);
        lpRect->left = ptTL.x;
        lpRect->top = ptTL.y;
        lpRect->right = ptBR.x;
        lpRect->bottom = ptBR.y;
        break;
    }
    case DPI_AWARENESS_SYSTEM_AWARE:
    {
        // 系统 DPI 空间 → 物理：乘以 屏幕DPI/系统DPI
        UINT sysDpi = GetDpiForSystem();
        UINT winDpi = GetDpiForWindow(hwnd);
        if (sysDpi > 0 && winDpi > 0 && winDpi != sysDpi)
        {
            double s = (double)winDpi / (double)sysDpi;
            lpRect->left = (LONG)(lpRect->left * s);
            lpRect->top = (LONG)(lpRect->top * s);
            lpRect->right = (LONG)(lpRect->right * s);
            lpRect->bottom = (LONG)(lpRect->bottom * s);
        }
        break;
    }
    default:   // PER_MONITOR / PER_MONITOR_V2：物理像素，无需换算
        break;
    }

    CPinyinIpc::DebugLog(L"Layout: GetTextExt rect=(%d,%d)-(%d,%d) dpiAwareness=%d (0=UNAWARE 1=SYSTEM 2=PM 3=PMV2)",
        lpRect->left, lpRect->top, lpRect->right, lpRect->bottom, awareness);
}
