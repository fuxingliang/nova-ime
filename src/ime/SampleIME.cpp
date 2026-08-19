// THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
// PARTICULAR PURPOSE.
//
// Copyright (c) Microsoft Corporation. All rights reserved

#include "Private.h"
#include "globals.h"
#include "SampleIME.h"
#include "CandidateListUIPresenter.h"
#include "CompositionProcessorEngine.h"
#include "Compartment.h"
#include "EditSession.h"
#include "PinyinIpc.h"

//+---------------------------------------------------------------------------
//
// CreateInstance
//
//----------------------------------------------------------------------------

/* static */
HRESULT CSampleIME::CreateInstance(_In_ IUnknown *pUnkOuter, REFIID riid, _Outptr_ void **ppvObj)
{
    CSampleIME* pSampleIME = nullptr;
    HRESULT hr = S_OK;

    if (ppvObj == nullptr)
    {
        return E_INVALIDARG;
    }

    *ppvObj = nullptr;

    if (nullptr != pUnkOuter)
    {
        return CLASS_E_NOAGGREGATION;
    }

    pSampleIME = new (std::nothrow) CSampleIME();
    if (pSampleIME == nullptr)
    {
        return E_OUTOFMEMORY;
    }

    hr = pSampleIME->QueryInterface(riid, ppvObj);

    pSampleIME->Release();

    return hr;
}

//+---------------------------------------------------------------------------
//
// ctor
//
//----------------------------------------------------------------------------

CSampleIME::CSampleIME()
{
    DllAddRef();

    _pThreadMgr = nullptr;

    _threadMgrEventSinkCookie = TF_INVALID_COOKIE;

    _pTextEditSinkContext = nullptr;
    _textEditSinkCookie = TF_INVALID_COOKIE;

    _activeLanguageProfileNotifySinkCookie = TF_INVALID_COOKIE;

    _dwThreadFocusSinkCookie = TF_INVALID_COOKIE;

    _pComposition = nullptr;

    _pCompositionProcessorEngine = nullptr;

    _candidateMode = CANDIDATE_NONE;
    _pCandidateListUIPresenter = nullptr;
    _isCandidateWithWildcard = FALSE;

    _pDocMgrLastFocused = nullptr;

    _pSIPIMEOnOffCompartment = nullptr;
    _dwSIPIMEOnOffCompartmentSinkCookie = 0;
    _msgWndHandle = nullptr;

    _pContext = nullptr;

    _hCandidateSelectWnd = nullptr;
    _wmCandidateSelect = 0;

    _refCount = 1;
}

//+---------------------------------------------------------------------------
//
// 鼠标点击候选：message-only 窗口 + IPC 回调
//
// IPC 读线程收到服务进程的 SelectCandidate(index) 命令后，回调投递到
// message-only 窗口；窗口过程运行在创建窗口的线程（即 TSF 活跃线程），
// 在那里获取焦点上下文并 RequestEditSession 执行选字——避免从 IPC 读线程
// 直接跨线程调用 TSF。
//
//----------------------------------------------------------------------------

BOOL CSampleIME::_InitCandidateSelectWindow()
{
    if (_hCandidateSelectWnd != nullptr)
    {
        return TRUE;
    }

    _wmCandidateSelect = RegisterWindowMessage(L"PinyinPlus.CandidateSelect");
    _wmDeleteUserWord = RegisterWindowMessage(L"PinyinPlus.DeleteUserWord");
    _wmInsertText = RegisterWindowMessage(L"PinyinPlus.InsertText");

    HINSTANCE hInst = Global::dllInstanceHandle;
    static bool s_classRegistered = false;
    if (!s_classRegistered)
    {
        WNDCLASSEXW wc = {};
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = CSampleIME::_CandidateSelectWndProc;
        wc.hInstance     = hInst;
        wc.lpszClassName = L"PinyinPlus.CandidateMsgWnd";
        if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            CPinyinIpc::DebugLog(L"_InitCandidateSelectWindow RegisterClass FAILED err=%lu", GetLastError());
            return FALSE;
        }
        s_classRegistered = true;
    }

    _hCandidateSelectWnd = CreateWindowExW(0, L"PinyinPlus.CandidateMsgWnd", L"",
        WS_POPUP, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hInst, nullptr);
    if (_hCandidateSelectWnd == nullptr)
    {
        CPinyinIpc::DebugLog(L"_InitCandidateSelectWindow CreateWindow FAILED err=%lu", GetLastError());
        return FALSE;
    }
    SetWindowLongPtrW(_hCandidateSelectWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    CPinyinIpc::SetCandidateSelectCallback([this](int index) {
        if (_hCandidateSelectWnd != nullptr && _wmCandidateSelect != 0)
        {
            PostMessageW(_hCandidateSelectWnd, _wmCandidateSelect, static_cast<WPARAM>(index), 0);
        }
    });

    // 文本命令（右键删词 / 符号上屏）：字符串在堆上分配，PostMessage 传指针，WndProc 接收后释放。
    // 注意 _running 仍为真时 _onDeleteUserWord/_onInsertText 不会被并发清空（Uninitialize 先停线程）。
    CPinyinIpc::SetDeleteUserWordCallback([this](const std::wstring& word) {
        if (_hCandidateSelectWnd != nullptr && _wmDeleteUserWord != 0)
        {
            auto* p = new std::wstring(word);
            if (!PostMessageW(_hCandidateSelectWnd, _wmDeleteUserWord,
                              reinterpret_cast<WPARAM>(p), 0))
            {
                delete p;
            }
        }
    });

    CPinyinIpc::SetDemoteWordCallback([this](const std::wstring& word) {
        if (_hCandidateSelectWnd != nullptr && _wmDemoteWord != 0)
        {
            auto* p = new std::wstring(word);
            if (!PostMessageW(_hCandidateSelectWnd, _wmDemoteWord,
                              reinterpret_cast<WPARAM>(p), 0))
            {
                delete p;
            }
        }
    });

    CPinyinIpc::SetInsertTextCallback([this](const std::wstring& text) {
        if (_hCandidateSelectWnd != nullptr && _wmInsertText != 0)
        {
            auto* p = new std::wstring(text);
            if (!PostMessageW(_hCandidateSelectWnd, _wmInsertText,
                              reinterpret_cast<WPARAM>(p), 0))
            {
                delete p;
            }
        }
    });

    CPinyinIpc::DebugLog(L"_InitCandidateSelectWindow OK hwnd=%p", _hCandidateSelectWnd);
    return TRUE;
}

void CSampleIME::_UninitCandidateSelectWindow()
{
    if (_hCandidateSelectWnd != nullptr)
    {
        DestroyWindow(_hCandidateSelectWnd);
        _hCandidateSelectWnd = nullptr;
    }
    CPinyinIpc::SetCandidateSelectCallback(nullptr);
    CPinyinIpc::SetDeleteUserWordCallback(nullptr);
    CPinyinIpc::SetInsertTextCallback(nullptr);
}

// static
LRESULT CALLBACK CSampleIME::_CandidateSelectWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    CSampleIME* pIme = reinterpret_cast<CSampleIME*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (pIme != nullptr)
    {
        if (msg == pIme->_wmCandidateSelect)
        {
            pIme->_OnCandidateSelectMessage(static_cast<int>(wParam));
            return 0;
        }
        if (msg == pIme->_wmDeleteUserWord)
        {
            // wParam = heap 上 new 的 std::wstring*（回调线程分配，此处接收并释放）
            std::wstring* p = reinterpret_cast<std::wstring*>(wParam);
            if (p != nullptr)
            {
                pIme->_OnDeleteUserWordMessage(*p);
                delete p;
            }
            return 0;
        }
        if (msg == pIme->_wmInsertText)
        {
            std::wstring* p = reinterpret_cast<std::wstring*>(wParam);
            if (p != nullptr)
            {
                pIme->_OnInsertTextMessage(*p);
                delete p;
            }
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void CSampleIME::_OnCandidateSelectMessage(int index)
{
    CPinyinIpc::DebugLog(L"_OnCandidateSelectMessage index=%d", index);
    if (index < 0 || _pCandidateListUIPresenter == nullptr)
    {
        return;   // 无候选窗（非活跃输入法实例）→ 忽略
    }

    ITfThreadMgr* pThreadMgr = _GetThreadMgr();
    if (pThreadMgr == nullptr)
    {
        return;
    }
    ITfDocumentMgr* pDocMgr = nullptr;
    if (FAILED(pThreadMgr->GetFocus(&pDocMgr)) || pDocMgr == nullptr)
    {
        return;
    }
    ITfContext* pContext = nullptr;
    if (FAILED(pDocMgr->GetBase(&pContext)) || pContext == nullptr)
    {
        pDocMgr->Release();
        return;
    }

    CCandidateSelectEditSession* pSession = new (std::nothrow) CCandidateSelectEditSession(this, pContext, static_cast<UINT>(index));
    if (pSession != nullptr)
    {
        HRESULT hr = S_OK;
        pContext->RequestEditSession(_tfClientId, pSession, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hr);
        pSession->Release();
    }

    pContext->Release();
    pDocMgr->Release();
}

//+---------------------------------------------------------------------------
//
// _OnDemoteWordMessage — 右键"降低排位"（服务进程 type 8 → 引擎 case 16）
//
// 与删除用户词同款投递：TSF 线程 RequestEditSession 执行降权并刷新候选窗。
//
//----------------------------------------------------------------------------

void CSampleIME::_OnDemoteWordMessage(const std::wstring& word)
{
    CPinyinIpc::DebugLog(L"_OnDemoteWordMessage word=%s", word.c_str());
    if (word.empty() || _pCandidateListUIPresenter == nullptr)
    {
        return;
    }

    ITfThreadMgr* pThreadMgr = _GetThreadMgr();
    if (pThreadMgr == nullptr)
    {
        return;
    }
    ITfDocumentMgr* pDocMgr = nullptr;
    if (FAILED(pThreadMgr->GetFocus(&pDocMgr)) || pDocMgr == nullptr)
    {
        return;
    }
    ITfContext* pContext = nullptr;
    if (FAILED(pDocMgr->GetBase(&pContext)) || pContext == nullptr)
    {
        pDocMgr->Release();
        return;
    }

    CCandidateTextCommandEditSession* pSession =
        new (std::nothrow) CCandidateTextCommandEditSession(this, pContext, TextCommandKind::DemoteWord, word);
    if (pSession != nullptr)
    {
        HRESULT hr = S_OK;
        pContext->RequestEditSession(_tfClientId, pSession, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hr);
        pSession->Release();
    }

    pContext->Release();
    pDocMgr->Release();
}

//+---------------------------------------------------------------------------
//
// _OnDeleteUserWordMessage — 右键删除用户词（服务进程 type 6）
//
// 经 message-only 窗口投递到 TSF 线程，获取焦点上下文并 RequestEditSession，
// 在编辑会话里删除指定用户词并刷新候选。
//
//----------------------------------------------------------------------------

void CSampleIME::_OnDeleteUserWordMessage(const std::wstring& word)
{
    CPinyinIpc::DebugLog(L"_OnDeleteUserWordMessage word=%s", word.c_str());
    if (word.empty() || _pCandidateListUIPresenter == nullptr)
    {
        return;
    }

    ITfThreadMgr* pThreadMgr = _GetThreadMgr();
    if (pThreadMgr == nullptr)
    {
        return;
    }
    ITfDocumentMgr* pDocMgr = nullptr;
    if (FAILED(pThreadMgr->GetFocus(&pDocMgr)) || pDocMgr == nullptr)
    {
        return;
    }
    ITfContext* pContext = nullptr;
    if (FAILED(pDocMgr->GetBase(&pContext)) || pContext == nullptr)
    {
        pDocMgr->Release();
        return;
    }

    CCandidateTextCommandEditSession* pSession =
        new (std::nothrow) CCandidateTextCommandEditSession(this, pContext, TextCommandKind::DeleteUserWord, word);
    if (pSession != nullptr)
    {
        HRESULT hr = S_OK;
        pContext->RequestEditSession(_tfClientId, pSession, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hr);
        pSession->Release();
    }

    pContext->Release();
    pDocMgr->Release();
}

//+---------------------------------------------------------------------------
//
// _OnInsertTextMessage — 符号面板插入文本（服务进程 type 7）
//
//----------------------------------------------------------------------------

void CSampleIME::_OnInsertTextMessage(const std::wstring& text)
{
    CPinyinIpc::DebugLog(L"_OnInsertTextMessage text=%s", text.c_str());
    if (text.empty())
    {
        return;
    }

    ITfThreadMgr* pThreadMgr = _GetThreadMgr();
    if (pThreadMgr == nullptr)
    {
        return;
    }
    ITfDocumentMgr* pDocMgr = nullptr;
    if (FAILED(pThreadMgr->GetFocus(&pDocMgr)) || pDocMgr == nullptr)
    {
        return;
    }
    ITfContext* pContext = nullptr;
    if (FAILED(pDocMgr->GetBase(&pContext)) || pContext == nullptr)
    {
        pDocMgr->Release();
        return;
    }

    CCandidateTextCommandEditSession* pSession =
        new (std::nothrow) CCandidateTextCommandEditSession(this, pContext, TextCommandKind::InsertText, text);
    if (pSession != nullptr)
    {
        HRESULT hr = S_OK;
        pContext->RequestEditSession(_tfClientId, pSession, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hr);
        pSession->Release();
    }

    pContext->Release();
    pDocMgr->Release();
}

//+---------------------------------------------------------------------------
//
// dtor
//
//----------------------------------------------------------------------------

CSampleIME::~CSampleIME()
{
    if (_pCandidateListUIPresenter)
    {
        delete _pCandidateListUIPresenter;
        _pCandidateListUIPresenter = nullptr;
    }
    DllRelease();
}

//+---------------------------------------------------------------------------
//
// QueryInterface
//
//----------------------------------------------------------------------------

STDAPI CSampleIME::QueryInterface(REFIID riid, _Outptr_ void **ppvObj)
{
    if (ppvObj == nullptr)
    {
        return E_INVALIDARG;
    }

    *ppvObj = nullptr;

    if (IsEqualIID(riid, IID_IUnknown) ||
        IsEqualIID(riid, IID_ITfTextInputProcessor))
    {
        *ppvObj = (ITfTextInputProcessor *)this;
    }
    else if (IsEqualIID(riid, IID_ITfTextInputProcessorEx))
    {
        *ppvObj = (ITfTextInputProcessorEx *)this;
    }
    else if (IsEqualIID(riid, IID_ITfThreadMgrEventSink))
    {
        *ppvObj = (ITfThreadMgrEventSink *)this;
    }
    else if (IsEqualIID(riid, IID_ITfTextEditSink))
    {
        *ppvObj = (ITfTextEditSink *)this;
    }
    else if (IsEqualIID(riid, IID_ITfKeyEventSink))
    {
        *ppvObj = (ITfKeyEventSink *)this;
    }
    else if (IsEqualIID(riid, IID_ITfActiveLanguageProfileNotifySink))
    {
        *ppvObj = (ITfActiveLanguageProfileNotifySink *)this;
    }
    else if (IsEqualIID(riid, IID_ITfCompositionSink))
    {
        *ppvObj = (ITfKeyEventSink *)this;
    }
    else if (IsEqualIID(riid, IID_ITfDisplayAttributeProvider))
    {
        *ppvObj = (ITfDisplayAttributeProvider *)this;
    }
    else if (IsEqualIID(riid, IID_ITfThreadFocusSink))
    {
        *ppvObj = (ITfThreadFocusSink *)this;
    }
    else if (IsEqualIID(riid, IID_ITfFunctionProvider))
    {
        *ppvObj = (ITfFunctionProvider *)this;
    }
    else if (IsEqualIID(riid, IID_ITfFunction))
    {
        *ppvObj = (ITfFunction *)this;
    }
    else if (IsEqualIID(riid, IID_ITfFnGetPreferredTouchKeyboardLayout))
    {
        *ppvObj = (ITfFnGetPreferredTouchKeyboardLayout *)this;
    }

    if (*ppvObj)
    {
        AddRef();
        return S_OK;
    }

    return E_NOINTERFACE;
}


//+---------------------------------------------------------------------------
//
// AddRef
//
//----------------------------------------------------------------------------

STDAPI_(ULONG) CSampleIME::AddRef()
{
    return ++_refCount;
}

//+---------------------------------------------------------------------------
//
// Release
//
//----------------------------------------------------------------------------

STDAPI_(ULONG) CSampleIME::Release()
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
// ITfTextInputProcessorEx::ActivateEx
//
//----------------------------------------------------------------------------

STDAPI CSampleIME::ActivateEx(ITfThreadMgr *pThreadMgr, TfClientId tfClientId, DWORD dwFlags)
{
    _pThreadMgr = pThreadMgr;
    _pThreadMgr->AddRef();

    _tfClientId = tfClientId;
    _dwActivateFlags = dwFlags;

    if (!_InitThreadMgrEventSink())
    {
        goto ExitError;
    }

    ITfDocumentMgr* pDocMgrFocus = nullptr;
    if (SUCCEEDED(_pThreadMgr->GetFocus(&pDocMgrFocus)) && (pDocMgrFocus != nullptr))
    {
        _InitTextEditSink(pDocMgrFocus);
        pDocMgrFocus->Release();
    }

    if (!_InitKeyEventSink())
    {
        goto ExitError;
    }

    if (!_InitActiveLanguageProfileNotifySink())
    {
        goto ExitError;
    }

    if (!_InitThreadFocusSink())
    {
        goto ExitError;
    }

    if (!_InitDisplayAttributeGuidAtom())
    {
        goto ExitError;
    }

    if (!_InitFunctionProviderSink())
    {
        goto ExitError;
    }

    if (!_AddTextProcessorEngine())
    {
        goto ExitError;
    }

    // 鼠标点击候选：创建 message-only 窗口 + 注册 IPC 回调
    _InitCandidateSelectWindow();

    return S_OK;

ExitError:
    Deactivate();
    return E_FAIL;
}

//+---------------------------------------------------------------------------
//
// ITfTextInputProcessorEx::Deactivate
//
//----------------------------------------------------------------------------

STDAPI CSampleIME::Deactivate()
{
    if (_pCompositionProcessorEngine)
    {
        delete _pCompositionProcessorEngine;
        _pCompositionProcessorEngine = nullptr;
    }

    ITfContext* pContext = _pContext;
    if (_pContext)
    {   
        pContext->AddRef();
        _EndComposition(_pContext);
    }

    if (_pCandidateListUIPresenter)
    {
        delete _pCandidateListUIPresenter;
        _pCandidateListUIPresenter = nullptr;

        if (pContext)
        {
            pContext->Release();
        }

        _candidateMode = CANDIDATE_NONE;
        _isCandidateWithWildcard = FALSE;
    }

    _UninitFunctionProviderSink();

    _UninitThreadFocusSink();

    // 鼠标点击候选：销毁 message-only 窗口，摘除 IPC 回调
    _UninitCandidateSelectWindow();

    _UninitActiveLanguageProfileNotifySink();

    _UninitKeyEventSink();

    _UninitThreadMgrEventSink();

    CCompartment CompartmentKeyboardOpen(_pThreadMgr, _tfClientId, GUID_COMPARTMENT_KEYBOARD_OPENCLOSE);
    CompartmentKeyboardOpen._ClearCompartment();

    CCompartment CompartmentDoubleSingleByte(_pThreadMgr, _tfClientId, Global::SampleIMEGuidCompartmentDoubleSingleByte);
    CompartmentDoubleSingleByte._ClearCompartment();

    CCompartment CompartmentPunctuation(_pThreadMgr, _tfClientId, Global::SampleIMEGuidCompartmentPunctuation);
    CompartmentDoubleSingleByte._ClearCompartment();

    if (_pThreadMgr != nullptr)
    {
        _pThreadMgr->Release();
    }

    _tfClientId = TF_CLIENTID_NULL;

    if (_pDocMgrLastFocused)
    {
        _pDocMgrLastFocused->Release();
		_pDocMgrLastFocused = nullptr;
    }

    return S_OK;
}

//+---------------------------------------------------------------------------
//
// ITfFunctionProvider::GetType
//
//----------------------------------------------------------------------------
HRESULT CSampleIME::GetType(__RPC__out GUID *pguid)
{
    HRESULT hr = E_INVALIDARG;
    if (pguid)
    {
        *pguid = Global::SampleIMECLSID;
        hr = S_OK;
    }
    return hr;
}

//+---------------------------------------------------------------------------
//
// ITfFunctionProvider::::GetDescription
//
//----------------------------------------------------------------------------
HRESULT CSampleIME::GetDescription(__RPC__deref_out_opt BSTR *pbstrDesc)
{
    HRESULT hr = E_INVALIDARG;
    if (pbstrDesc != nullptr)
    {
        *pbstrDesc = nullptr;
        hr = E_NOTIMPL;
    }
    return hr;
}

//+---------------------------------------------------------------------------
//
// ITfFunctionProvider::::GetFunction
//
//----------------------------------------------------------------------------
HRESULT CSampleIME::GetFunction(__RPC__in REFGUID rguid, __RPC__in REFIID riid, __RPC__deref_out_opt IUnknown **ppunk)
{
    HRESULT hr = E_NOINTERFACE;

    if ((IsEqualGUID(rguid, GUID_NULL)) 
        && (IsEqualGUID(riid, __uuidof(ITfFnSearchCandidateProvider))))
    {
        hr = _pITfFnSearchCandidateProvider->QueryInterface(riid, (void**)ppunk);
    }
    else if (IsEqualGUID(rguid, GUID_NULL))
    {
        hr = QueryInterface(riid, (void **)ppunk);
    }

    return hr;
}

//+---------------------------------------------------------------------------
//
// ITfFunction::GetDisplayName
//
//----------------------------------------------------------------------------
HRESULT CSampleIME::GetDisplayName(_Out_ BSTR *pbstrDisplayName)
{
    HRESULT hr = E_INVALIDARG;
    if (pbstrDisplayName != nullptr)
    {
        *pbstrDisplayName = nullptr;
        hr = E_NOTIMPL;
    }
    return hr;
}

//+---------------------------------------------------------------------------
//
// ITfFnGetPreferredTouchKeyboardLayout::GetLayout
// The tkblayout will be Optimized layout.
//----------------------------------------------------------------------------
HRESULT CSampleIME::GetLayout(_Out_ TKBLayoutType *ptkblayoutType, _Out_ WORD *pwPreferredLayoutId)
{
    HRESULT hr = E_INVALIDARG;
    if ((ptkblayoutType != nullptr) && (pwPreferredLayoutId != nullptr))
    {
        *ptkblayoutType = TKBLT_OPTIMIZED;
        *pwPreferredLayoutId = TKBL_OPT_SIMPLIFIED_CHINESE_PINYIN;
        hr = S_OK;
    }
    return hr;
}