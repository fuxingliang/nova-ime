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
    // 均返回 TF_E_NOLAYOUT（0x80040201）。此时用鼠标光标位置兜底，保证候选窗
    // 始终有定位坐标（Server 端 CandidatePlacement 据此贴边/翻转/DPI 换算）。
    // GetCursorPos 在 PerMonitorV2 感知进程返回物理像素，无需再 _NormalizeToPhysical。
    BOOL cursorFallback = FALSE;
    if (FAILED(hr))
    {
        POINT cur = { 0, 0 };
        if (GetCursorPos(&cur))
        {
            lpRect->left = cur.x;
            lpRect->top = cur.y;
            lpRect->right = cur.x + 4;
            lpRect->bottom = cur.y + 20;
            hr = S_OK;
            usedFallback = TRUE;
            cursorFallback = TRUE;
            CPinyinIpc::DebugLog(L"Layout: GetTextExt cursor-fallback x=%d y=%d", cur.x, cur.y);
        }
    }

    if (FAILED(hr))
    {
        CPinyinIpc::DebugLog(L"Layout: GetTextExt FAILED hr=0x%08x fallback=%d", hr, usedFallback);
    }
    else
    {
        CPinyinIpc::DebugLog(L"Layout: GetTextExt raw=(%d,%d)-(%d,%d) fallback=%d cursor=%d",
            lpRect->left, lpRect->top, lpRect->right, lpRect->bottom, usedFallback, cursorFallback);
    }

    // 将 TSF 坐标统一换算为"物理屏幕像素"（Server 端按物理像素换算 DIP）。
    // 游戏（如虚幻引擎）多为 DPI unaware，Windows 虚拟化坐标需在此还原，
    // 否则候选窗与光标相差一个 DPI 缩放偏移。
    // 鼠标光标兜底已是物理像素，跳过换算。
    if (SUCCEEDED(hr) && !cursorFallback)
    {
        _NormalizeToPhysical(pContextView, lpRect);
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
