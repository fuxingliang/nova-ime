// THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
// PARTICULAR PURPOSE.
//
// Copyright (c) Microsoft Corporation. All rights reserved

#include "Private.h"
#include "Globals.h"
#include "SampleIME.h"
#include "CompositionProcessorEngine.h"
#include "PinyinIpc.h"

//+---------------------------------------------------------------------------
//
// HostNeedsCompositionText — 宿主应用是否需要在编辑区显示拼音组合
//
// 搜狗式"隐藏拼音"（组合保持空，拼音只在候选窗）在大多数应用工作良好，
// 但 QQ(NT/Chromium) 对空组合不兼容：Chromium 在 TSF 空组合回调后可能
// 产生空指针导致崩溃（搜狗在 QQ 里是写拼音组合的，所以完全兼容）。
// 因此对 QQ 等已知应用回退为"写拼音组合"（与搜狗一致）。
//
//----------------------------------------------------------------------------

bool HostNeedsCompositionText()
{
    static int s_result = -1;   // -1 未判定；0 不写（隐藏拼音）；1 写（兼容模式）
    if (s_result < 0)
    {
        s_result = 0;
        // Electron/Chromium 系宿主（Qoder/Trae 等 Electron IDE、QQ NT 等）会加载
        // chrome_elf.dll。Chromium 对"空组合"（隐藏拼音）不兼容：前几个字侥幸
        // 上屏后，选字无法提交（候选窗有字但不入编辑区），之后只剩英文。
        // 回退为"写拼音组合"（与搜狗一致），保证 Chromium 宿主正常上屏。
        if (GetModuleHandleW(L"chrome_elf.dll") != nullptr)
        {
            s_result = 1;
        }
        else
        {
            // 兜底：按 exe 名识别已知 Chromium 内核应用（转小写后子串匹配，
            // 覆盖 Chrome/Edge/QQ 及 Qoder/Trae 等未加载 chrome_elf 的构建）
            WCHAR exe[MAX_PATH] = {0};
            if (GetModuleFileNameW(nullptr, exe, MAX_PATH) > 0)
            {
                WCHAR lower[MAX_PATH] = {0};
                wcscpy_s(lower, _countof(lower), exe);
                _wcslwr_s(lower, _countof(lower));
                if (wcsstr(lower, L"qq.exe") != nullptr ||
                    wcsstr(lower, L"chrome.exe") != nullptr ||
                    wcsstr(lower, L"msedge.exe") != nullptr ||
                    wcsstr(lower, L"qoder") != nullptr ||
                    wcsstr(lower, L"trae") != nullptr)
                {
                    s_result = 1;
                }
            }
        }
    }
    return s_result == 1;
}

//+---------------------------------------------------------------------------
//
// ITfCompositionSink::OnCompositionTerminated
//
// Callback for ITfCompositionSink.  The system calls this method whenever
// someone other than this service ends a composition.
//----------------------------------------------------------------------------

STDAPI CSampleIME::OnCompositionTerminated(TfEditCookie ecWrite, _In_ ITfComposition *pComposition)
{
    // Clear dummy composition
    _RemoveDummyCompositionForComposing(ecWrite, pComposition);

    // Clear display attribute and end composition, _EndComposition will release composition for us
    ITfContext* pContext = _pContext;
    if (pContext)
    {
        pContext->AddRef();
    }

    _EndComposition(_pContext);

    _DeleteCandidateList(FALSE, pContext);

    if (pContext)
    {
        pContext->Release();
        pContext = nullptr;
    }

    return S_OK;
}

//+---------------------------------------------------------------------------
//
// _IsComposing
//
//----------------------------------------------------------------------------

BOOL CSampleIME::_IsComposing()
{
    return _pComposition != nullptr;
}

//+---------------------------------------------------------------------------
//
// _SetComposition
//
//----------------------------------------------------------------------------

void CSampleIME::_SetComposition(_In_ ITfComposition *pComposition)
{
    _pComposition = pComposition;
}

//+---------------------------------------------------------------------------
//
// ToFullWidthIfNeeded — 全角模式上屏转换
//
// 全角模式（engine.conf width=1，候选窗状态按钮切换）：
// ASCII 可打印字符（0x21-0x7E）转全角（0xFF01-0xFF5E，复用 FullWidthCharTable）。
// 汉字 / 全角 / 符号 / emoji（非 ASCII）原样保留。返回是否发生了转换。
//
//----------------------------------------------------------------------------

static bool ToFullWidthIfNeeded(_In_ CStringRange* pIn, std::wstring& out)
{
    if (!Global::IsFullWidthModeEnabled())
    {
        return false;   // 默认半角：零开销
    }
    const WCHAR* p = pIn->Get();
    DWORD_PTR len = pIn->GetLength();
    out.reserve(len);
    bool changed = false;
    for (DWORD_PTR i = 0; i < len; i++)
    {
        WCHAR c = p[i];
        if (c >= 0x21 && c <= 0x7E)
        {
            out += Global::FullWidthCharTable[c - 0x20];
            changed = true;
        }
        else
        {
            out += c;
        }
    }
    return changed;
}

//+---------------------------------------------------------------------------
//
// _AddComposingAndChar
//
//----------------------------------------------------------------------------

HRESULT CSampleIME::_AddComposingAndChar(TfEditCookie ec, _In_ ITfContext *pContext, _In_ CStringRange *pstrAddString, _In_ BOOL writeToComposition)
{
    HRESULT hr = S_OK;

    // 全角模式：上屏文本 ASCII 全角化（汉字候选不受影响；隐藏拼音模式组合为空）
    std::wstring fwText;
    CStringRange fwRange;
    CStringRange* pStr = pstrAddString;
    if (ToFullWidthIfNeeded(pstrAddString, fwText))
    {
        fwRange.Set(const_cast<WCHAR*>(fwText.c_str()), static_cast<DWORD_PTR>(fwText.size()));
        pStr = &fwRange;
    }

    ULONG fetched = 0;
    TF_SELECTION tfSelection;

    if (pContext->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &tfSelection, &fetched) != S_OK || fetched == 0)
        return S_FALSE;

    //
    // make range start to selection
    //
    ITfRange* pAheadSelection = nullptr;
    hr = pContext->GetStart(ec, &pAheadSelection);
    if (SUCCEEDED(hr))
    {
        hr = pAheadSelection->ShiftEndToRange(ec, tfSelection.range, TF_ANCHOR_START);
        if (SUCCEEDED(hr))
        {
            ITfRange* pRange = nullptr;
            BOOL exist_composing = _FindComposingRange(ec, pContext, pAheadSelection, &pRange);

            _SetInputString(ec, pContext, pRange, pStr, exist_composing, writeToComposition);

            if (pRange)
            {
                pRange->Release();
            }
        }
    }

    tfSelection.range->Release();

    if (pAheadSelection)
    {
        pAheadSelection->Release();
    }

    return S_OK;
}

//+---------------------------------------------------------------------------
//
// _AddCharAndFinalize
//
//----------------------------------------------------------------------------

HRESULT CSampleIME::_AddCharAndFinalize(TfEditCookie ec, _In_ ITfContext *pContext, _In_ CStringRange *pstrAddString)
{
    HRESULT hr = E_FAIL;

    // 全角模式：上屏文本 ASCII 全角化（标点已由 GetPunctuation 全角化，此处兜底字母/数字/符号）
    std::wstring fwText;
    const WCHAR* pText = pstrAddString->Get();
    LONG textLen = static_cast<LONG>(pstrAddString->GetLength());
    if (ToFullWidthIfNeeded(pstrAddString, fwText))
    {
        pText = fwText.c_str();
        textLen = static_cast<LONG>(fwText.size());
    }

    ULONG fetched = 0;
    TF_SELECTION tfSelection;

    if ((hr = pContext->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &tfSelection, &fetched)) != S_OK || fetched != 1)
        return hr;

    // we use SetText here instead of InsertTextAtSelection because we've already started a composition
    // we don't want to the app to adjust the insertion point inside our composition
    hr = tfSelection.range->SetText(ec, 0, pText, textLen);
    if (hr == S_OK)
    {
        // update the selection, we'll make it an insertion point just past
        // the inserted text.
        tfSelection.range->Collapse(ec, TF_ANCHOR_END);
        pContext->SetSelection(ec, 1, &tfSelection);
    }

    tfSelection.range->Release();

    return hr;
}

//+---------------------------------------------------------------------------
//
// _FindComposingRange
//
//----------------------------------------------------------------------------

BOOL CSampleIME::_FindComposingRange(TfEditCookie ec, _In_ ITfContext *pContext, _In_ ITfRange *pSelection, _Outptr_result_maybenull_ ITfRange **ppRange)
{
    if (ppRange == nullptr)
    {
        return FALSE;
    }

    *ppRange = nullptr;

    // find GUID_PROP_COMPOSING
    ITfProperty* pPropComp = nullptr;
    IEnumTfRanges* enumComp = nullptr;

    HRESULT hr = pContext->GetProperty(GUID_PROP_COMPOSING, &pPropComp);
    if (FAILED(hr) || pPropComp == nullptr)
    {
        return FALSE;
    }

    hr = pPropComp->EnumRanges(ec, &enumComp, pSelection);
    if (FAILED(hr) || enumComp == nullptr)
    {
        pPropComp->Release();
        return FALSE;
    }

    BOOL isCompExist = FALSE;
    VARIANT var;
    ULONG  fetched = 0;

    while (enumComp->Next(1, ppRange, &fetched) == S_OK && fetched == 1)
    {
        hr = pPropComp->GetValue(ec, *ppRange, &var);
        if (hr == S_OK)
        {
            if (var.vt == VT_I4 && var.lVal != 0)
            {
                isCompExist = TRUE;
                break;
            }
        }
        (*ppRange)->Release();
        *ppRange = nullptr;
    }

    pPropComp->Release();
    enumComp->Release();

    return isCompExist;
}

//+---------------------------------------------------------------------------
//
// _SetInputString
//
//----------------------------------------------------------------------------

HRESULT CSampleIME::_SetInputString(TfEditCookie ec, _In_ ITfContext *pContext, _Out_opt_ ITfRange *pRange, _In_ CStringRange *pstrAddString, BOOL exist_composing, _In_ BOOL writeToComposition)
{
    ITfRange* pRangeInsert = nullptr;
    if (!exist_composing)
    {
        _InsertAtSelection(ec, pContext, pstrAddString, &pRangeInsert);
        if (pRangeInsert == nullptr)
        {
            CPinyinIpc::DebugLog(L"_SetInputString: InsertAtSelection failed");
            return S_OK;
        }
        pRange = pRangeInsert;
    }
    if (pRange != nullptr)
    {
        if (writeToComposition)
        {
            // 选字/造词/标点等确认内容：写入组合 range，由 Finalize 提交到应用。
            HRESULT hrSetText = pRange->SetText(ec, 0, pstrAddString->Get(), (LONG)pstrAddString->GetLength());
            CPinyinIpc::DebugLog(L"_SetInputString: SetText hr=0x%08x len=%d exist=%d", hrSetText, pstrAddString->GetLength(), exist_composing);
        }
        else
        {
            // 拼音输入：不写入编辑区（搜狗式隐藏拼音，只在候选窗显示）。
            // 组合 range 保持 0 长度（光标处）；GetTextExt 对空 range 返回
            // 插入符位置，候选窗定位不受影响。
            CPinyinIpc::DebugLog(L"_SetInputString: pinyin hidden, len=%d exist=%d", pstrAddString->GetLength(), exist_composing);
        }
    }

    _SetCompositionLanguage(ec, pContext);

    BOOL bAttr = _SetCompositionDisplayAttributes(ec, pContext, _gaDisplayAttributeInput);
    CPinyinIpc::DebugLog(L"_SetInputString: DisplayAttr=%d pComposition=%p", bAttr ? 1 : 0, _pComposition);

    // update the selection, we'll make it an insertion point just past
    // the inserted text.
    ITfRange* pSelection = nullptr;
    TF_SELECTION sel;

    if ((pRange != nullptr) && (pRange->Clone(&pSelection) == S_OK))
    {
        pSelection->Collapse(ec, TF_ANCHOR_END);

        sel.range = pSelection;
        sel.style.ase = TF_AE_NONE;
        sel.style.fInterimChar = FALSE;
        pContext->SetSelection(ec, 1, &sel);
        pSelection->Release();
    }

    if (pRangeInsert)
    {
        pRangeInsert->Release();
    }


    return S_OK;
}

//+---------------------------------------------------------------------------
//
// _InsertAtSelection
//
//----------------------------------------------------------------------------

HRESULT CSampleIME::_InsertAtSelection(TfEditCookie ec, _In_ ITfContext *pContext, _In_ CStringRange *pstrAddString, _Outptr_ ITfRange **ppCompRange)
{
    ITfRange* rangeInsert = nullptr;
    ITfInsertAtSelection* pias = nullptr;
    HRESULT hr = S_OK;

    if (ppCompRange == nullptr)
    {
        hr = E_INVALIDARG;
        goto Exit;
    }

    *ppCompRange = nullptr;

    hr = pContext->QueryInterface(IID_ITfInsertAtSelection, (void **)&pias);
    if (FAILED(hr))
    {
        goto Exit;
    }

    hr = pias->InsertTextAtSelection(ec, TF_IAS_QUERYONLY, pstrAddString->Get(), (LONG)pstrAddString->GetLength(), &rangeInsert);

    if ( FAILED(hr) || rangeInsert == nullptr)
    {
        rangeInsert = nullptr;
        pias->Release();
        goto Exit;
    }

    *ppCompRange = rangeInsert;
    pias->Release();
    hr = S_OK;

Exit:
    return hr;
}

//+---------------------------------------------------------------------------
//
// _RemoveDummyCompositionForComposing
//
//----------------------------------------------------------------------------

HRESULT CSampleIME::_RemoveDummyCompositionForComposing(TfEditCookie ec, _In_ ITfComposition *pComposition)
{
    HRESULT hr = S_OK;

    ITfRange* pRange = nullptr;
    
    if (pComposition)
    {
        hr = pComposition->GetRange(&pRange);
        if (SUCCEEDED(hr))
        {
            pRange->SetText(ec, 0, nullptr, 0);
            pRange->Release();
        }
    }

    return hr;
}

//+---------------------------------------------------------------------------
//
// _SetCompositionLanguage
//
//----------------------------------------------------------------------------

BOOL CSampleIME::_SetCompositionLanguage(TfEditCookie ec, _In_ ITfContext *pContext)
{
    HRESULT hr = S_OK;
    BOOL ret = TRUE;

    // 组合可能已被宿主终止（部署/注册表变化时 Chromium 重置 TSF），_pComposition
    // 已置空，直接 GetRange 会空指针崩溃（0xC0000005 宿主即退出）。
    if (_pComposition == nullptr)
    {
        return TRUE;
    }

    CCompositionProcessorEngine* pCompositionProcessorEngine = nullptr;
    pCompositionProcessorEngine = _pCompositionProcessorEngine;

    LANGID langidProfile = 0;
    pCompositionProcessorEngine->GetLanguageProfile(&langidProfile);

    ITfRange* pRangeComposition = nullptr;
    ITfProperty* pLanguageProperty = nullptr;

    // we need a range and the context it lives in
    hr = _pComposition->GetRange(&pRangeComposition);
    if (FAILED(hr))
    {
        ret = FALSE;
        goto Exit;
    }

    // get our the language property
    hr = pContext->GetProperty(GUID_PROP_LANGID, &pLanguageProperty);
    if (FAILED(hr))
    {
        ret = FALSE;
        goto Exit;
    }

    VARIANT var;
    var.vt = VT_I4;   // we're going to set DWORD
    var.lVal = langidProfile; 

    hr = pLanguageProperty->SetValue(ec, pRangeComposition, &var);
    if (FAILED(hr))
    {
        ret = FALSE;
        goto Exit;
    }

    pLanguageProperty->Release();
    pRangeComposition->Release();

Exit:
    return ret;
}
