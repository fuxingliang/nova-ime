// THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
// PARTICULAR PURPOSE.
//
// Copyright (c) Microsoft Corporation. All rights reserved

#pragma once

class CSampleIME;

class CTfTextLayoutSink : public ITfTextLayoutSink
{
public:
    CTfTextLayoutSink(_In_ CSampleIME *pTextService);
    virtual ~CTfTextLayoutSink();

    // IUnknown methods
    STDMETHODIMP QueryInterface(REFIID riid, _Outptr_ void **ppvObj);
    STDMETHODIMP_(ULONG) AddRef(void);
    STDMETHODIMP_(ULONG) Release(void);

    // ITfTextLayoutSink
    STDMETHODIMP OnLayoutChange(_In_ ITfContext *pContext, TfLayoutCode lcode, _In_ ITfContextView *pContextView);

    HRESULT _StartLayout(_In_ ITfContext *pContextDocument, TfEditCookie ec, _In_ ITfRange *pRangeComposition);
    VOID _EndLayout();

    HRESULT _GetTextExt(_Out_ RECT *lpRect);
    ITfContext* _GetContextDocument() { return _pContextDocument; };

    // 位置抑制：_MoveWindowToTextExt 已发送精确位置后，阻止 _LayoutChangeNotification
    // 用 _GetWindowExtent 的估算坐标覆盖（虚拟窗口尺寸不准，导致坐标偏移）。
    void _SuppressPositionUpdate(BOOL suppress) { _suppressPositionUpdate = suppress; }

    VOID _NormalizeToPhysical(_In_ ITfContextView *pContextView, _Inout_ RECT *lpRect);

    virtual VOID _LayoutChangeNotification(_In_ RECT *lpRect) = 0;
    virtual VOID _LayoutDestroyNotification() = 0;

protected:
    BOOL _suppressPositionUpdate;  // TRUE = _LayoutChangeNotification 跳过 SendSetPosition

private:
    HRESULT _AdviseTextLayoutSink();
    HRESULT _UnadviseTextLayoutSink();

private:
    ITfRange* _pRangeComposition;
    ITfContext* _pContextDocument;
    TfEditCookie _tfEditCookie;
    CSampleIME* _pTextService;
    DWORD _dwCookieTextLayoutSink;
    LONG _refCount;
};
