// THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
// PARTICULAR PURPOSE.
//
// Copyright (c) Microsoft Corporation. All rights reserved

#include "Private.h"
#include "EditSession.h"
#include "SampleIME.h"
#include "EngineClient.h"

//+---------------------------------------------------------------------------
//
// ctor
//
//----------------------------------------------------------------------------

CEditSessionBase::CEditSessionBase(_In_ CSampleIME *pTextService, _In_ ITfContext *pContext)
{
    _refCount = 1;
    _pContext = pContext;
    _pContext->AddRef();

    _pTextService = pTextService;
    _pTextService->AddRef();
}

//+---------------------------------------------------------------------------
//
// dtor
//
//----------------------------------------------------------------------------

CEditSessionBase::~CEditSessionBase()
{
    _pContext->Release();
    _pTextService->Release();
}

//+---------------------------------------------------------------------------
//
// QueryInterface
//
//----------------------------------------------------------------------------

STDAPI CEditSessionBase::QueryInterface(REFIID riid, _Outptr_ void **ppvObj)
{
    if (ppvObj == nullptr)
    {
        return E_INVALIDARG;
    }

    *ppvObj = nullptr;

    if (IsEqualIID(riid, IID_IUnknown) ||
        IsEqualIID(riid, IID_ITfEditSession))
    {
        *ppvObj = (ITfLangBarItemButton *)this;
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

STDAPI_(ULONG) CEditSessionBase::AddRef(void)
{
    return ++_refCount;
}

//+---------------------------------------------------------------------------
//
// Release
//
//----------------------------------------------------------------------------

STDAPI_(ULONG) CEditSessionBase::Release(void)
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
// CCandidateSelectEditSession
//
//----------------------------------------------------------------------------

CCandidateSelectEditSession::CCandidateSelectEditSession(_In_ CSampleIME *pTextService, _In_ ITfContext *pContext, _In_ UINT globalIndex)
    : CEditSessionBase(pTextService, pContext)
{
    _globalIndex = globalIndex;
}

STDAPI CCandidateSelectEditSession::DoEditSession(TfEditCookie ec)
{
    return _pTextService->_HandleCandidateSelectByGlobalIndex(ec, _pContext, _globalIndex);
}

//+---------------------------------------------------------------------------
//
// CCandidateTextCommandEditSession
//
//----------------------------------------------------------------------------

CCandidateTextCommandEditSession::CCandidateTextCommandEditSession(_In_ CSampleIME *pTextService, _In_ ITfContext *pContext,
                                                                   _In_ TextCommandKind kind, _In_ const std::wstring& text)
    : CEditSessionBase(pTextService, pContext)
{
    _kind = kind;
    _text = text;
}

STDAPI CCandidateTextCommandEditSession::DoEditSession(TfEditCookie ec)
{
    if (_kind == TextCommandKind::DeleteUserWord)
    {
        // 右键删除用户词（仅删 isUser=true 词条，词库词静默忽略）→ 刷新候选（删除词即时消失）
        if (!_text.empty() && CEngineClient::DeleteUserWord(_text))
        {
            return _pTextService->_HandleCompositionConvert(ec, _pContext, FALSE);
        }
        return S_FALSE;
    }

    if (_kind == TextCommandKind::DemoteWord)
    {
        // 右键"降低排位"：把词降权沉底（引擎持久化）→ 刷新候选（该词立即让位）
        if (!_text.empty() && CEngineClient::DemoteWord(_text))
        {
            return _pTextService->_HandleCompositionConvert(ec, _pContext, FALSE);
        }
        return S_FALSE;
    }

    // InsertText：在光标处插入符号/文本（ITfInsertAtSelection 标准接口，不依赖组合状态）
    if (_text.empty())
    {
        return S_FALSE;
    }
    ITfInsertAtSelection* pInsert = nullptr;
    HRESULT hr = _pContext->QueryInterface(IID_ITfInsertAtSelection, reinterpret_cast<void**>(&pInsert));
    if (SUCCEEDED(hr) && pInsert != nullptr)
    {
        ITfRange* pRange = nullptr;
        hr = pInsert->InsertTextAtSelection(ec, 0, _text.c_str(), static_cast<LONG>(_text.size()), &pRange);
        if (SUCCEEDED(hr) && pRange != nullptr)
        {
            // 光标折叠到插入文本末尾（style 全 0 = TF_AE_NONE + 非 interim）
            if (SUCCEEDED(pRange->Collapse(ec, TF_ANCHOR_END)))
            {
                TF_SELECTION sel = {};
                sel.range = pRange;
                _pContext->SetSelection(ec, 1, &sel);
            }
            pRange->Release();
        }
        pInsert->Release();
    }
    return hr;
}
