// THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
// PARTICULAR PURPOSE.
//
// Copyright (c) Microsoft Corporation. All rights reserved

#include "Private.h"
#include "SampleIME.h"
#include "CandidateWindow.h"
#include "CandidateListUIPresenter.h"
#include "CompositionProcessorEngine.h"
#include "SampleIMEBaseStructure.h"
#include "PinyinIpc.h"

//////////////////////////////////////////////////////////////////////
//
// CSampleIME candidate key handler methods
//
//////////////////////////////////////////////////////////////////////

const int MOVEUP_ONE = -1;
const int MOVEDOWN_ONE = 1;
const int MOVETO_TOP = 0;
const int MOVETO_BOTTOM = -1;
//+---------------------------------------------------------------------------
//
// _HandleCandidateFinalize
//
//----------------------------------------------------------------------------

HRESULT CSampleIME::_HandleCandidateFinalize(TfEditCookie ec, _In_ ITfContext *pContext)
{
    // 造词模式：空格 = 锁定当前段选中字并推进段
    if (_pCompositionProcessorEngine->IsMakeWordMode())
    {
        return _HandleMakeWordAdvance(ec, pContext);
    }

    HRESULT hr = S_OK;
    DWORD_PTR candidateLen = 0;
    const WCHAR* pCandidateString = nullptr;
    CStringRange candidateString;

    if (nullptr == _pCandidateListUIPresenter)
    {
        goto NoPresenter;
    }

    candidateLen = _pCandidateListUIPresenter->_GetSelectedCandidateString(&pCandidateString);

    candidateString.Set(pCandidateString, candidateLen);

    if (candidateLen)
    {
        hr = _AddComposingAndChar(ec, pContext, &candidateString);

        // 自学习词库：选词上屏后提升该词词频（拼音为空时 _BoostUserWord 内部回退到用户输入）
        const WCHAR* pKeyCode = nullptr;
        DWORD_PTR keyLen = _pCandidateListUIPresenter->_GetSelectedCandidateKeyCode(&pKeyCode);
        _BoostUserWord(pKeyCode, keyLen, pCandidateString, candidateLen);

        if (FAILED(hr))
        {
            return hr;
        }
    }

NoPresenter:

    _HandleComplete(ec, pContext);

    return hr;
}

//+---------------------------------------------------------------------------
//
// _BoostUserWord — 自学习词库
//
// 用户选词上屏后，把 (pinyin, word) 写入用户词库（词频+1）。
// pinyin 可能来自：
//   - 全拼/增量搜索：候选 _FindKeyCode 可能是完整拼音或剩余拼音
//   - 简拼回退：候选 _FindKeyCode 是完整全拼（与用户输入的简拼串不同）
// 处理策略：
//   1. 候选 key 以用户已输入拼音为前缀 → 候选 key 即完整拼音，直接使用
//   2. 否则拼接 用户输入+候选 key，若该组合存在于词库 → 使用拼接
//   3. 否则候选 key 本身是完整拼音（简拼回退场景）→ 直接使用候选 key
//
//----------------------------------------------------------------------------

void CSampleIME::_BoostUserWord(_In_ const WCHAR* pwszPinyin, DWORD_PTR pinyinLen, _In_ const WCHAR* pwszWord, DWORD_PTR wordLen)
{
    if (!pwszWord || wordLen == 0)
    {
        return;
    }

    std::wstring word(pwszWord, wordLen);

    // 用户已输入拼音缓冲（增量搜索时候选 key 被截断为空，需回退到它）
    std::wstring typed;
    {
        DWORD_PTR bufLen = _pCompositionProcessorEngine->GetVirtualKeyLength();
        typed.reserve(bufLen);
        for (DWORD_PTR i = 0; i < bufLen; i++)
        {
            typed.push_back(_pCompositionProcessorEngine->GetVirtualKey(i));
        }
    }

    std::wstring fullPinyin;

    if (pwszPinyin && pinyinLen > 0)
    {
        std::wstring candidateKey(pwszPinyin, pinyinLen);

        if (!typed.empty() && candidateKey.compare(0, typed.size(), typed) == 0)
        {
            // 候选 key 以用户输入开头 → 候选 key 即完整拼音
            fullPinyin = candidateKey;
        }
        else if (!typed.empty())
        {
            // 尝试 用户输入+候选剩余（增量搜索截断场景）
            std::wstring joined = typed + candidateKey;
            if (_pCompositionProcessorEngine->IsEntry(joined, word))
            {
                fullPinyin = joined;
            }
            else
            {
                // 简拼回退场景：候选 key 是完整全拼
                fullPinyin = candidateKey;
            }
        }
        else
        {
            fullPinyin = candidateKey;
        }
    }

    // 候选 key 缺失/为空（增量搜索截断后为空串）→ 直接用用户输入拼音
    if (fullPinyin.empty())
    {
        fullPinyin = typed;
    }

    if (fullPinyin.empty())
    {
        return;
    }

    _pCompositionProcessorEngine->BoostWord(fullPinyin.c_str(), fullPinyin.size(), word.c_str(), word.size());
}

//+---------------------------------------------------------------------------
//
// _HandleCandidateConvert
//
//----------------------------------------------------------------------------

HRESULT CSampleIME::_HandleCandidateConvert(TfEditCookie ec, _In_ ITfContext *pContext)
{
    return _HandleCandidateWorker(ec, pContext);
}

//+---------------------------------------------------------------------------
//
// _HandleCandidateWorker
//
//----------------------------------------------------------------------------

HRESULT CSampleIME::_HandleCandidateWorker(TfEditCookie ec, _In_ ITfContext *pContext)
{
    // 造词模式：数字键选字 = 锁定当前段选中字并推进段
    if (_pCompositionProcessorEngine->IsMakeWordMode())
    {
        return _HandleMakeWordAdvance(ec, pContext);
    }

    HRESULT hrReturn = E_FAIL;
    DWORD_PTR candidateLen = 0;
    const WCHAR* pCandidateString = nullptr;
    BSTR pbstr = nullptr;
    CStringRange candidateString;
    CSampleImeArray<CCandidateListItem> candidatePhraseList;

    if (nullptr == _pCandidateListUIPresenter)
    {
        hrReturn = S_OK;
        goto Exit;
    }

    candidateLen = _pCandidateListUIPresenter->_GetSelectedCandidateString(&pCandidateString);
    if (0 == candidateLen)
    {
        hrReturn = S_FALSE;
        goto Exit;
    }

    candidateString.Set(pCandidateString, candidateLen);

    BOOL fMakePhraseFromText = _pCompositionProcessorEngine->IsMakePhraseFromText();
    if (fMakePhraseFromText)
    {
        _pCompositionProcessorEngine->GetCandidateStringInConverted(candidateString, &candidatePhraseList);
        LCID locale = _pCompositionProcessorEngine->GetLocale();

        _pCandidateListUIPresenter->RemoveSpecificCandidateFromList(locale, candidatePhraseList, candidateString);
    }

    // We have a candidate list if candidatePhraseList.Cnt is not 0
    // If we are showing reverse conversion, use CCandidateListUIPresenter
    CANDIDATE_MODE tempCandMode = CANDIDATE_NONE;
    CCandidateListUIPresenter* pTempCandListUIPresenter = nullptr;
    if (candidatePhraseList.Count())
    {
        tempCandMode = CANDIDATE_WITH_NEXT_COMPOSITION;

        pTempCandListUIPresenter = new (std::nothrow) CCandidateListUIPresenter(this, Global::AtomCandidateWindow,
            CATEGORY_CANDIDATE,
            _pCompositionProcessorEngine->GetCandidateListIndexRange(),
            FALSE);
        if (nullptr == pTempCandListUIPresenter)
        {
            hrReturn = E_OUTOFMEMORY;
            goto Exit;
        }
    }

    // call _Start*Line for CCandidateListUIPresenter or CReadingLine
    // we don't cache the document manager object so get it from pContext.
    ITfDocumentMgr* pDocumentMgr = nullptr;
    HRESULT hrStartCandidateList = E_FAIL;
    if (pContext->GetDocumentMgr(&pDocumentMgr) == S_OK)
    {
        ITfRange* pRange = nullptr;
        if (_pComposition->GetRange(&pRange) == S_OK)
        {
            if (pTempCandListUIPresenter)
            {
                hrStartCandidateList = pTempCandListUIPresenter->_StartCandidateList(_tfClientId, pDocumentMgr, pContext, ec, pRange, _pCompositionProcessorEngine->GetCandidateWindowWidth());
            } 

            pRange->Release();
        }
        pDocumentMgr->Release();
    }

    // set up candidate list if it is being shown
    if (SUCCEEDED(hrStartCandidateList))
    {
        pTempCandListUIPresenter->_SetTextColor(RGB(0, 0x80, 0), GetSysColor(COLOR_WINDOW));    // Text color is green
        pTempCandListUIPresenter->_SetFillColor((HBRUSH)(COLOR_WINDOW+1));    // Background color is window

        // 自学习词库：先捕获选中候选的拼音（旧 presenter 稍后会被删除替换）
        const WCHAR* pSelectedKeyCode = nullptr;
        DWORD_PTR selectedKeyLen = 0;
        if (_pCandidateListUIPresenter)
        {
            selectedKeyLen = _pCandidateListUIPresenter->_GetSelectedCandidateKeyCode(&pSelectedKeyCode);
        }

        // Add composing character
        hrReturn = _AddComposingAndChar(ec, pContext, &candidateString);

        // 选词上屏后提升该词词频（数字键选词路径；拼音为空时 _BoostUserWord 内部回退到用户输入）
        if (hrReturn == S_OK)
        {
            _BoostUserWord(pSelectedKeyCode, selectedKeyLen, candidateString.Get(), candidateString.GetLength());
        }

        // close candidate list —— 必须先结束旧的（发 Hide），再让临时列表显示（发 Show），
        // 否则临时列表的 Show 会被旧的 Hide 覆盖，导致候选窗不可见
        if (_pCandidateListUIPresenter)
        {
            _pCandidateListUIPresenter->_EndCandidateList();
            delete _pCandidateListUIPresenter;
            _pCandidateListUIPresenter = nullptr;

            _candidateMode = CANDIDATE_NONE;
            _isCandidateWithWildcard = FALSE;
        }

        pTempCandListUIPresenter->_SetText(&candidatePhraseList, FALSE);

        if (hrReturn == S_OK)
        {
            // copy temp candidate
            _pCandidateListUIPresenter = pTempCandListUIPresenter;

            _candidateMode = tempCandMode;
            _isCandidateWithWildcard = FALSE;
        }
    }
    else
    {
        hrReturn = _HandleCandidateFinalize(ec, pContext);
    }

    if (pbstr)
    {
        SysFreeString(pbstr);
    }

Exit:
    return hrReturn;
}

//+---------------------------------------------------------------------------
//
// _HandleMakeWordAdvance — 造词模式：锁定当前段选中字并推进
//
// 空格/数字选字：把当前段选中的单字追加到 _makeWordChars，段索引前进。
// 还有后续段 → 刷新候选为下一段单字；全部段完成 → 组合词上屏 + 入库
// （AddUserWord，下次全拼/简拼直达）+ 结束本轮。
//
//----------------------------------------------------------------------------

HRESULT CSampleIME::_HandleMakeWordAdvance(TfEditCookie ec, _In_ ITfContext *pContext)
{
    CCompositionProcessorEngine* pEngine = _pCompositionProcessorEngine;
    if (!pEngine || !pEngine->IsMakeWordMode())
    {
        return S_FALSE;
    }

    // 取当前段选中单字
    DWORD_PTR candLen = 0;
    const WCHAR* pCand = nullptr;
    if (_pCandidateListUIPresenter)
    {
        candLen = _pCandidateListUIPresenter->_GetSelectedCandidateString(&pCand);
    }
    if (candLen == 0 || !pCand)
    {
        return S_FALSE;
    }

    // 锁定该字
    pEngine->AppendMakeWordChar(pCand, candLen);
    UINT nextSeg = pEngine->GetMakeWordSegIndex() + 1;

    if (nextSeg >= pEngine->GetMakeWordSylCount())
    {
        // 全部段完成：组合词上屏 + 入库 + 结束本轮
        std::wstring word = pEngine->GetMakeWordChars();
        std::wstring key = pEngine->GetMakeWordFullKey();
        pEngine->AddUserWord(key.c_str(), key.size(), word.c_str(), word.size());

        pEngine->ExitMakeWordMode();
        _DeleteCandidateList(FALSE, pContext);

        CStringRange finalWord;
        finalWord.Set(word.c_str(), static_cast<DWORD_PTR>(word.size()));
        HRESULT hr = _AddComposingAndChar(ec, pContext, &finalWord);   // 替换组合中的拼音串为组合词
        _HandleComplete(ec, pContext);
        return hr;
    }

    // 还有后续段：推进并刷新候选
    pEngine->SetMakeWordSegIndex(nextSeg);
    CSampleImeArray<CCandidateListItem> candidateList;
    pEngine->GetMakeWordCandidates(&candidateList);
    if (_pCandidateListUIPresenter)
    {
        _pCandidateListUIPresenter->_ClearList();
        _pCandidateListUIPresenter->_SetText(&candidateList, TRUE);
    }
    return S_OK;
}

//+---------------------------------------------------------------------------
//
// _HandleCandidateDeleteUserWord — 候选模式 Ctrl+Delete：删除当前选中用户词
//
// 取候选窗高亮词 → 引擎删除（仅删用户造的词，词库自带词静默忽略）→
// 重新生成候选并刷新候选窗（删除的词即时消失）。
//
//----------------------------------------------------------------------------

HRESULT CSampleIME::_HandleCandidateDeleteUserWord(TfEditCookie ec, _In_ ITfContext *pContext)
{
    DWORD_PTR candLen = 0;
    const WCHAR* pCand = nullptr;
    if (_pCandidateListUIPresenter)
    {
        candLen = _pCandidateListUIPresenter->_GetSelectedCandidateString(&pCand);
    }
    if (candLen == 0 || !pCand)
    {
        return S_FALSE;   // 无高亮候选（候选窗未打开/空）
    }

    std::wstring word(pCand, candLen);
    if (!CEngineClient::DeleteUserWord(word))
    {
        return S_FALSE;   // 引擎不可达 / 非用户词（删除失败）
    }

    // 重新查询当前拼音并刷新候选窗（删除后该词条从候选消失）
    return _HandleCompositionConvert(ec, pContext, FALSE);
}

//+---------------------------------------------------------------------------
//
// _HandleCandidateDemoteWord — 候选模式 Ctrl+PageDown / 右键"降低排位"：
// 把当前高亮词降权沉底（引擎持久化到 downweight.txt）→ 刷新候选窗。
//
// 词库自带词/用户词一视同仁（用户明确"不想要它靠前"）；重复执行幂等。
//
//----------------------------------------------------------------------------

HRESULT CSampleIME::_HandleCandidateDemoteWord(TfEditCookie ec, _In_ ITfContext *pContext)
{
    DWORD_PTR candLen = 0;
    const WCHAR* pCand = nullptr;
    if (_pCandidateListUIPresenter)
    {
        candLen = _pCandidateListUIPresenter->_GetSelectedCandidateString(&pCand);
    }
    if (candLen == 0 || !pCand)
    {
        return S_FALSE;   // 无高亮候选（候选窗未打开/空）
    }

    std::wstring word(pCand, candLen);
    if (!CEngineClient::DemoteWord(word))
    {
        return S_FALSE;   // 引擎不可达（降权失败）
    }

    // 重新查询当前拼音并刷新候选窗（降权词立即沉底）
    return _HandleCompositionConvert(ec, pContext, FALSE);
}

//+---------------------------------------------------------------------------
//
// _HandleEnterMakeWord — 候选模式按 0 键：从当前拼音进入造词模式
//
// 从引擎当前输入缓冲切音节进入造词（EnterMakeWordModeFromCurrentInput），
// 然后把候选窗切换为第一段单字候选；拼音缓冲由 _SetText 在造词模式下
// 自动显示"已选字+当前段拼音"。
//
//----------------------------------------------------------------------------

HRESULT CSampleIME::_HandleEnterMakeWord(TfEditCookie ec, _In_ ITfContext *pContext)
{
    ec;
    pContext;

    CCompositionProcessorEngine* pEngine = _pCompositionProcessorEngine;
    if (!pEngine || !pEngine->EnterMakeWordModeFromCurrentInput())
    {
        return S_FALSE;
    }

    CSampleImeArray<CCandidateListItem> candidateList;
    pEngine->GetMakeWordCandidates(&candidateList);
    if (_pCandidateListUIPresenter)
    {
        _pCandidateListUIPresenter->_ClearList();
        _pCandidateListUIPresenter->_SetText(&candidateList, TRUE);
    }
    return S_OK;
}

//+---------------------------------------------------------------------------
//
// _HandleCandidateArrowKey
//
//----------------------------------------------------------------------------

HRESULT CSampleIME::_HandleCandidateArrowKey(TfEditCookie ec, _In_ ITfContext *pContext, _In_ KEYSTROKE_FUNCTION keyFunction)
{
    ec;
    pContext;

    _pCandidateListUIPresenter->AdviseUIChangedByArrowKey(keyFunction);

    return S_OK;
}

//+---------------------------------------------------------------------------
//
// _HandleCandidateSelectByNumber
//
//----------------------------------------------------------------------------

HRESULT CSampleIME::_HandleCandidateSelectByNumber(TfEditCookie ec, _In_ ITfContext *pContext, _In_ UINT uCode)
{
    int iSelectAsNumber = _pCompositionProcessorEngine->GetCandidateListIndexRange()->GetIndex(uCode);
    if (iSelectAsNumber == -1)
    {
        return S_FALSE;
    }

    if (_pCandidateListUIPresenter)
    {
        if (_pCandidateListUIPresenter->_SetSelectionInPage(iSelectAsNumber))
        {
            return _HandleCandidateConvert(ec, pContext);
        }
    }

    return S_FALSE;
}

//+---------------------------------------------------------------------------
//
// _HandleCandidateSelectByGlobalIndex — 鼠标点击候选（按全局索引选字）
//
// WPF 候选窗点击后经 IPC 发来全局索引（页起始+页内序号）。这里先把 TSF
// 侧选中项切到目标项（_SetSelectionInPage），再走常规选字链路：
//   普通模式 → _HandleCandidateConvert 上屏；
//   造词模式 → _HandleMakeWordAdvance 锁定当前段单字。
//
//----------------------------------------------------------------------------

HRESULT CSampleIME::_HandleCandidateSelectByGlobalIndex(TfEditCookie ec, _In_ ITfContext *pContext, _In_ UINT globalIndex)
{
    if (_pCandidateListUIPresenter == nullptr || _pCompositionProcessorEngine == nullptr)
    {
        return S_FALSE;
    }

    UINT selected = 0;
    if (FAILED(_pCandidateListUIPresenter->GetSelection(&selected)))
    {
        return S_FALSE;
    }

    // 当前页起始 = 选中索引所在页的起点；点击项须落在当前页内
    UINT pageSize = _pCompositionProcessorEngine->GetCandidateListIndexRange()->Count();
    UINT pageStart = (selected / pageSize) * pageSize;
    if (globalIndex < pageStart)
    {
        return S_FALSE;
    }
    int inPage = static_cast<int>(globalIndex - pageStart);

    if (!_pCandidateListUIPresenter->_SetSelectionInPage(inPage))
    {
        return S_FALSE;
    }

    if (_pCompositionProcessorEngine->IsMakeWordMode())
    {
        return _HandleMakeWordAdvance(ec, pContext);
    }
    return _HandleCandidateConvert(ec, pContext);
}

//+---------------------------------------------------------------------------
//
// _HandlePhraseFinalize
//
//----------------------------------------------------------------------------

HRESULT CSampleIME::_HandlePhraseFinalize(TfEditCookie ec, _In_ ITfContext *pContext)
{
    HRESULT hr = S_OK;

    DWORD phraseLen = 0;
    const WCHAR* pPhraseString = nullptr;

    phraseLen = (DWORD)_pCandidateListUIPresenter->_GetSelectedCandidateString(&pPhraseString);

    CStringRange phraseString;
    phraseString.Set(pPhraseString, phraseLen);

    if (phraseLen)
    {
        if ((hr = _AddCharAndFinalize(ec, pContext, &phraseString)) != S_OK)
        {
            return hr;
        }
    }

    _HandleComplete(ec, pContext);

    return S_OK;
}

//+---------------------------------------------------------------------------
//
// _HandlePhraseArrowKey
//
//----------------------------------------------------------------------------

HRESULT CSampleIME::_HandlePhraseArrowKey(TfEditCookie ec, _In_ ITfContext *pContext, _In_ KEYSTROKE_FUNCTION keyFunction)
{
    ec;
    pContext;

    _pCandidateListUIPresenter->AdviseUIChangedByArrowKey(keyFunction);

    return S_OK;
}

//+---------------------------------------------------------------------------
//
// _HandlePhraseSelectByNumber
//
//----------------------------------------------------------------------------

HRESULT CSampleIME::_HandlePhraseSelectByNumber(TfEditCookie ec, _In_ ITfContext *pContext, _In_ UINT uCode)
{
    int iSelectAsNumber = _pCompositionProcessorEngine->GetCandidateListIndexRange()->GetIndex(uCode);
    if (iSelectAsNumber == -1)
    {
        return S_FALSE;
    }

    if (_pCandidateListUIPresenter)
    {
        if (_pCandidateListUIPresenter->_SetSelectionInPage(iSelectAsNumber))
        {
            return _HandlePhraseFinalize(ec, pContext);
        }
    }

    return S_FALSE;
}

//////////////////////////////////////////////////////////////////////
//
// CCandidateListUIPresenter class
//
//////////////////////////////////////////////////////////////////////

//+---------------------------------------------------------------------------
//
// ctor
//
//----------------------------------------------------------------------------

CCandidateListUIPresenter::CCandidateListUIPresenter(_In_ CSampleIME *pTextService, ATOM atom, KEYSTROKE_CATEGORY Category, _In_ CCandidateRange *pIndexRange, BOOL hideWindow) : CTfTextLayoutSink(pTextService)
{
    _atom = atom;

    _pIndexRange = pIndexRange;

    _parentWndHandle = nullptr;
    _pCandidateWnd = nullptr;

    _Category = Category;

    _updatedFlags = 0;

    _uiElementId = (DWORD)-1;
    _isShowMode = TRUE;   // store return value from BeginUIElement
    _hideWindow = hideWindow;     // Hide window flag from [Configuration] CandidateList.Phrase.HideWindow

    _pTextService = pTextService;
    _pTextService->AddRef();

    _refCount = 1;
}

//+---------------------------------------------------------------------------
//
// dtor
//
//----------------------------------------------------------------------------

CCandidateListUIPresenter::~CCandidateListUIPresenter()
{
    _EndCandidateList();
    _pTextService->Release();
}

//+---------------------------------------------------------------------------
//
// ITfCandidateListUIElement::IUnknown::QueryInterface
//
//----------------------------------------------------------------------------

STDAPI CCandidateListUIPresenter::QueryInterface(REFIID riid, _Outptr_ void **ppvObj)
{
    if (CTfTextLayoutSink::QueryInterface(riid, ppvObj) == S_OK)
    {
        return S_OK;
    }

    if (ppvObj == nullptr)
    {
        return E_INVALIDARG;
    }

    *ppvObj = nullptr;

    if (IsEqualIID(riid, IID_ITfUIElement) ||
        IsEqualIID(riid, IID_ITfCandidateListUIElement))
    {
        *ppvObj = (ITfCandidateListUIElement*)this;
    }
    else if (IsEqualIID(riid, IID_IUnknown) || 
        IsEqualIID(riid, IID_ITfCandidateListUIElementBehavior)) 
    {
        *ppvObj = (ITfCandidateListUIElementBehavior*)this;
    }
    else if (IsEqualIID(riid, __uuidof(ITfIntegratableCandidateListUIElement))) 
    {
        *ppvObj = (ITfIntegratableCandidateListUIElement*)this;
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
// ITfCandidateListUIElement::IUnknown::AddRef
//
//----------------------------------------------------------------------------

STDAPI_(ULONG) CCandidateListUIPresenter::AddRef()
{
    CTfTextLayoutSink::AddRef();
    return ++_refCount;
}

//+---------------------------------------------------------------------------
//
// ITfCandidateListUIElement::IUnknown::Release
//
//----------------------------------------------------------------------------

STDAPI_(ULONG) CCandidateListUIPresenter::Release()
{
    CTfTextLayoutSink::Release();

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
// ITfCandidateListUIElement::ITfUIElement::GetDescription
//
//----------------------------------------------------------------------------

STDAPI CCandidateListUIPresenter::GetDescription(BSTR *pbstr)
{
    if (pbstr)
    {
        *pbstr = SysAllocString(L"Cand");
    }
    return S_OK;
}

//+---------------------------------------------------------------------------
//
// ITfCandidateListUIElement::ITfUIElement::GetGUID
//
//----------------------------------------------------------------------------

STDAPI CCandidateListUIPresenter::GetGUID(GUID *pguid)
{
    *pguid = Global::SampleIMEGuidCandUIElement;
    return S_OK;
}

//+---------------------------------------------------------------------------
//
// ITfCandidateListUIElement::ITfUIElement::Show
//
//----------------------------------------------------------------------------

STDAPI CCandidateListUIPresenter::Show(BOOL showCandidateWindow)
{
    if (showCandidateWindow)
    {
        ToShowCandidateWindow();
    }
    else
    {
        ToHideCandidateWindow();
    }
    return S_OK;
}

HRESULT CCandidateListUIPresenter::ToShowCandidateWindow()
{
    // 原生候选窗一律不显示，候选由 WPF 服务进程统一渲染
    if (_pCandidateWnd)
    {
        _MoveWindowToTextExt();

        _pCandidateWnd->_Show(FALSE);
    }

    return S_OK;
}

HRESULT CCandidateListUIPresenter::ToHideCandidateWindow()
{
	if (_pCandidateWnd)
	{
		_pCandidateWnd->_Show(FALSE);
	}

    // 不向 WPF 服务进程发送 Hide：WPF 候选窗的隐藏只在 _EndCandidateList()
    // （组合结束/取消）时统一发送。此前在 TSF 调用 Show(FALSE) 时（如线程失焦、
    // UILess 模式切换）也发 Hide，但对应的 Show(TRUE) 并不发 Show，导致
    // WPF 候选窗被隐藏后无法恢复 → 用户看到"打字但不出候选窗"。

    _updatedFlags = TF_CLUIE_SELECTION | TF_CLUIE_CURRENTPAGE;
    _UpdateUIElement();

    return S_OK;
}

//+---------------------------------------------------------------------------
//
// ITfCandidateListUIElement::ITfUIElement::IsShown
//
//----------------------------------------------------------------------------

STDAPI CCandidateListUIPresenter::IsShown(BOOL *pIsShow)
{
    *pIsShow = _pCandidateWnd->_IsWindowVisible();
    return S_OK;
}

//+---------------------------------------------------------------------------
//
// ITfCandidateListUIElement::GetUpdatedFlags
//
//----------------------------------------------------------------------------

STDAPI CCandidateListUIPresenter::GetUpdatedFlags(DWORD *pdwFlags)
{
    *pdwFlags = _updatedFlags;
    return S_OK;
}

//+---------------------------------------------------------------------------
//
// ITfCandidateListUIElement::GetDocumentMgr
//
//----------------------------------------------------------------------------

STDAPI CCandidateListUIPresenter::GetDocumentMgr(ITfDocumentMgr **ppdim)
{
    *ppdim = nullptr;

    return E_NOTIMPL;
}

//+---------------------------------------------------------------------------
//
// ITfCandidateListUIElement::GetCount
//
//----------------------------------------------------------------------------

STDAPI CCandidateListUIPresenter::GetCount(UINT *pCandidateCount)
{
    if (_pCandidateWnd)
    {
        *pCandidateCount = _pCandidateWnd->_GetCount();
    }
    else
    {
        *pCandidateCount = 0;
    }
    return S_OK;
}

//+---------------------------------------------------------------------------
//
// ITfCandidateListUIElement::GetSelection
//
//----------------------------------------------------------------------------

STDAPI CCandidateListUIPresenter::GetSelection(UINT *pSelectedCandidateIndex)
{
    if (_pCandidateWnd)
    {
        *pSelectedCandidateIndex = _pCandidateWnd->_GetSelection();
    }
    else
    {
        *pSelectedCandidateIndex = 0;
    }
    return S_OK;
}

//+---------------------------------------------------------------------------
//
// ITfCandidateListUIElement::GetString
//
//----------------------------------------------------------------------------

STDAPI CCandidateListUIPresenter::GetString(UINT uIndex, BSTR *pbstr)
{
    if (!_pCandidateWnd || (uIndex > _pCandidateWnd->_GetCount()))
    {
        return E_FAIL;
    }

    DWORD candidateLen = 0;
    const WCHAR* pCandidateString = nullptr;

    candidateLen = _pCandidateWnd->_GetCandidateString(uIndex, &pCandidateString);

    *pbstr = (candidateLen == 0) ? nullptr : SysAllocStringLen(pCandidateString, candidateLen);

    return S_OK;
}

//+---------------------------------------------------------------------------
//
// ITfCandidateListUIElement::GetPageIndex
//
//----------------------------------------------------------------------------

STDAPI CCandidateListUIPresenter::GetPageIndex(UINT *pIndex, UINT uSize, UINT *puPageCnt)
{
    if (!_pCandidateWnd)
    {
        if (pIndex)
        {
            *pIndex = 0;
        }
        *puPageCnt = 0;
        return S_OK;
    }
    return _pCandidateWnd->_GetPageIndex(pIndex, uSize, puPageCnt);
}

//+---------------------------------------------------------------------------
//
// ITfCandidateListUIElement::SetPageIndex
//
//----------------------------------------------------------------------------

STDAPI CCandidateListUIPresenter::SetPageIndex(UINT *pIndex, UINT uPageCnt)
{
    if (!_pCandidateWnd)
    {
        return E_FAIL;
    }
    return _pCandidateWnd->_SetPageIndex(pIndex, uPageCnt);
}

//+---------------------------------------------------------------------------
//
// ITfCandidateListUIElement::GetCurrentPage
//
//----------------------------------------------------------------------------

STDAPI CCandidateListUIPresenter::GetCurrentPage(UINT *puPage)
{
    if (!_pCandidateWnd)
    {
        *puPage = 0;
        return S_OK;
    }
    return _pCandidateWnd->_GetCurrentPage(puPage);
}

//+---------------------------------------------------------------------------
//
// ITfCandidateListUIElementBehavior::SetSelection
// It is related of the mouse clicking behavior upon the suggestion window
//----------------------------------------------------------------------------

STDAPI CCandidateListUIPresenter::SetSelection(UINT nIndex)
{
    if (_pCandidateWnd)
    {
        _pCandidateWnd->_SetSelection(nIndex);
    }

    return S_OK;
}

//+---------------------------------------------------------------------------
//
// ITfCandidateListUIElementBehavior::Finalize
// It is related of the mouse clicking behavior upon the suggestion window
//----------------------------------------------------------------------------

STDAPI CCandidateListUIPresenter::Finalize(void)
{
    _CandidateChangeNotification(CAND_ITEM_SELECT);
    return S_OK;
}

//+---------------------------------------------------------------------------
//
// ITfCandidateListUIElementBehavior::Abort
//
//----------------------------------------------------------------------------

STDAPI CCandidateListUIPresenter::Abort(void)
{
    return E_NOTIMPL;
}

//+---------------------------------------------------------------------------
//
// ITfIntegratableCandidateListUIElement::SetIntegrationStyle
// To show candidateNumbers on the suggestion window
//----------------------------------------------------------------------------

STDAPI CCandidateListUIPresenter::SetIntegrationStyle(GUID guidIntegrationStyle)
{
    return (guidIntegrationStyle == GUID_INTEGRATIONSTYLE_SEARCHBOX) ? S_OK : E_NOTIMPL;
}

//+---------------------------------------------------------------------------
//
// ITfIntegratableCandidateListUIElement::GetSelectionStyle
//
//----------------------------------------------------------------------------

STDAPI CCandidateListUIPresenter::GetSelectionStyle(_Out_ TfIntegratableCandidateListSelectionStyle *ptfSelectionStyle)
{
    *ptfSelectionStyle = STYLE_ACTIVE_SELECTION;
    return S_OK;
}

//+---------------------------------------------------------------------------
//
// ITfIntegratableCandidateListUIElement::OnKeyDown
//
//----------------------------------------------------------------------------

STDAPI CCandidateListUIPresenter::OnKeyDown(_In_ WPARAM wParam, _In_ LPARAM lParam, _Out_ BOOL *pIsEaten)
{
    wParam;
    lParam;

    *pIsEaten = TRUE;
    return S_OK;
}

//+---------------------------------------------------------------------------
//
// ITfIntegratableCandidateListUIElement::ShowCandidateNumbers
//
//----------------------------------------------------------------------------

STDAPI CCandidateListUIPresenter::ShowCandidateNumbers(_Out_ BOOL *pIsShow)
{
    *pIsShow = TRUE;
    return S_OK;
}

//+---------------------------------------------------------------------------
//
// ITfIntegratableCandidateListUIElement::FinalizeExactCompositionString
//
//----------------------------------------------------------------------------

STDAPI CCandidateListUIPresenter::FinalizeExactCompositionString()
{
    return E_NOTIMPL;
}


//+---------------------------------------------------------------------------
//
// _StartCandidateList
//
//----------------------------------------------------------------------------

HRESULT CCandidateListUIPresenter::_StartCandidateList(TfClientId tfClientId, _In_ ITfDocumentMgr *pDocumentMgr, _In_ ITfContext *pContextDocument, TfEditCookie ec, _In_ ITfRange *pRangeComposition, UINT wndWidth)
{
	pDocumentMgr;tfClientId;

    HRESULT hr = E_FAIL;

    if (FAILED(_StartLayout(pContextDocument, ec, pRangeComposition)))
    {
        goto Exit;
    }

    BeginUIElement();

    hr = MakeCandidateWindow(pContextDocument, wndWidth);
    if (FAILED(hr))
    {
        goto Exit;
    }

    Show(_isShowMode);

    RECT rcTextExt;
    if (SUCCEEDED(_GetTextExt(&rcTextExt)))
    {
        _LayoutChangeNotification(&rcTextExt);
    }

Exit:
    if (FAILED(hr))
    {
        _EndCandidateList();
    }
    return hr;
}

//+---------------------------------------------------------------------------
//
// _EndCandidateList
//
//----------------------------------------------------------------------------

void CCandidateListUIPresenter::_EndCandidateList()
{
    EndUIElement();

    DisposeCandidateWindow();

    _EndLayout();

    CPinyinIpc::SendHide();
}

//+---------------------------------------------------------------------------
//
// _SetText
//
//----------------------------------------------------------------------------

void CCandidateListUIPresenter::_SetText(_In_ CSampleImeArray<CCandidateListItem> *pCandidateList, BOOL isAddFindKeyCode)
{
    AddCandidateToCandidateListUI(pCandidateList, isAddFindKeyCode);

    SetPageIndexWithScrollInfo(pCandidateList);

    // 将候选列表与拼音缓冲发送到 WPF 服务进程
    CPinyinIpc::DebugLog(L"_SetText count=%u isShow=%d pWnd=%p", pCandidateList->Count(), _isShowMode, _pCandidateWnd);
    if (_pCandidateWnd)
    {
        std::vector<std::wstring> candidates;
        candidates.reserve(pCandidateList->Count());
        for (UINT index = 0; index < pCandidateList->Count(); index++)
        {
            CCandidateListItem* pLI = pCandidateList->GetAt(index);
            DWORD_PTR itemLen = pLI->_ItemString.GetLength();
            if (itemLen > 0)
            {
                candidates.emplace_back(pLI->_ItemString.Get(), itemLen);
            }
            else
            {
                candidates.emplace_back();
            }
        }

        // 组合区拼音缓冲：普通模式逐字符取出；造词模式显示"已选字+当前段拼音"
        std::wstring buffer;
        CCompositionProcessorEngine* pEngine = _pTextService->GetCompositionProcessorEngine();
        if (pEngine)
        {
            if (pEngine->IsMakeWordMode())
            {
                buffer = pEngine->GetMakeWordDisplayBuffer();
            }
            else
            {
                DWORD_PTR bufLen = pEngine->GetVirtualKeyLength();
                buffer.reserve(bufLen);
                for (DWORD_PTR i = 0; i < bufLen; i++)
                {
                    buffer.push_back(pEngine->GetVirtualKey(i));
                }
            }
        }

        CPinyinIpc::SendShow(candidates,
            (int)_pCandidateWnd->_GetSelection(),
            buffer.empty() ? nullptr : buffer.c_str(),
            (int)buffer.size());
    }

    if (_isShowMode)
    {
        _pCandidateWnd->_InvalidateRect();
    }
    else
    {
        _updatedFlags = TF_CLUIE_COUNT       |
            TF_CLUIE_SELECTION   |
            TF_CLUIE_STRING      |
            TF_CLUIE_PAGEINDEX   |
            TF_CLUIE_CURRENTPAGE;
        _UpdateUIElement();
    }
}

void CCandidateListUIPresenter::AddCandidateToCandidateListUI(_In_ CSampleImeArray<CCandidateListItem> *pCandidateList, BOOL isAddFindKeyCode)
{
    for (UINT index = 0; index < pCandidateList->Count(); index++)
    {
        _pCandidateWnd->_AddString(pCandidateList->GetAt(index), isAddFindKeyCode);
    }
}

void CCandidateListUIPresenter::SetPageIndexWithScrollInfo(_In_ CSampleImeArray<CCandidateListItem> *pCandidateList)
{
    UINT candCntInPage = _pIndexRange->Count();
    UINT bufferSize = pCandidateList->Count() / candCntInPage + 1;
    UINT* puPageIndex = new (std::nothrow) UINT[ bufferSize ];
    if (puPageIndex != nullptr)
    {
        for (UINT i = 0; i < bufferSize; i++)
        {
            puPageIndex[i] = i * candCntInPage;
        }

        _pCandidateWnd->_SetPageIndex(puPageIndex, bufferSize);
        delete [] puPageIndex;
    }
    _pCandidateWnd->_SetScrollInfo(pCandidateList->Count(), candCntInPage);  // nMax:range of max, nPage:number of items in page

}
//+---------------------------------------------------------------------------
//
// _ClearList
//
//----------------------------------------------------------------------------

void CCandidateListUIPresenter::_ClearList()
{
    // 候选窗可能已被 DisposeCandidateWindow 释放（_pCandidateWnd = nullptr），
    // 例如上一轮无候选已销毁、本轮又走到清空分支。空指针解引用会导致
    // 0xC0000005 崩溃（崩溃点即 _ClearList 首条指令 mov rdi,[rcx+58h]）。
    if (_pCandidateWnd)
    {
        _pCandidateWnd->_ClearList();
        _pCandidateWnd->_InvalidateRect();
    }
}

//+---------------------------------------------------------------------------
//
// _SetTextColor
// _SetFillColor
//
//----------------------------------------------------------------------------

void CCandidateListUIPresenter::_SetTextColor(COLORREF crColor, COLORREF crBkColor)
{
    if (_pCandidateWnd)
    {
        _pCandidateWnd->_SetTextColor(crColor, crBkColor);
    }
}

void CCandidateListUIPresenter::_SetFillColor(HBRUSH hBrush)
{
    if (_pCandidateWnd)
    {
        _pCandidateWnd->_SetFillColor(hBrush);
    }
}

//+---------------------------------------------------------------------------
//
// _GetSelectedCandidateString
//
//----------------------------------------------------------------------------

DWORD_PTR CCandidateListUIPresenter::_GetSelectedCandidateString(_Outptr_result_maybenull_ const WCHAR **ppwchCandidateString)
{
    if (ppwchCandidateString)
    {
        *ppwchCandidateString = nullptr;
    }
    if (!_pCandidateWnd)
    {
        return 0;
    }
    return _pCandidateWnd->_GetSelectedCandidateString(ppwchCandidateString);
}

DWORD_PTR CCandidateListUIPresenter::_GetSelectedCandidateKeyCode(_Outptr_result_maybenull_ const WCHAR **ppwchKeyCode)
{
    if (ppwchKeyCode)
    {
        *ppwchKeyCode = nullptr;
    }
    if (!_pCandidateWnd)
    {
        return 0;
    }
    return _pCandidateWnd->_GetSelectedCandidateKeyCode(ppwchKeyCode);
}

//+---------------------------------------------------------------------------
//
// _MoveSelection
//
//----------------------------------------------------------------------------

BOOL CCandidateListUIPresenter::_MoveSelection(_In_ int offSet)
{
    if (!_pCandidateWnd)
    {
        return FALSE;
    }
    BOOL ret = _pCandidateWnd->_MoveSelection(offSet, TRUE);
    if (ret)
    {
        CPinyinIpc::SendSetSelection((int)_pCandidateWnd->_GetSelection());
        if (_isShowMode)
        {
            _pCandidateWnd->_InvalidateRect();
        }
        else
        {
            _updatedFlags = TF_CLUIE_SELECTION;
            _UpdateUIElement();
        }
    }
    return ret;
}

//+---------------------------------------------------------------------------
//
// _SetSelection
//
//----------------------------------------------------------------------------

BOOL CCandidateListUIPresenter::_SetSelection(_In_ int selectedIndex)
{
    if (!_pCandidateWnd)
    {
        return FALSE;
    }
    BOOL ret = _pCandidateWnd->_SetSelection(selectedIndex, TRUE);
    if (ret)
    {
        CPinyinIpc::SendSetSelection((int)_pCandidateWnd->_GetSelection());
        if (_isShowMode)
        {
            _pCandidateWnd->_InvalidateRect();
        }
        else
        {
            _updatedFlags = TF_CLUIE_SELECTION |
                TF_CLUIE_CURRENTPAGE;
            _UpdateUIElement();
        }
    }
    return ret;
}

//+---------------------------------------------------------------------------
//
// _MovePage
//
//----------------------------------------------------------------------------

BOOL CCandidateListUIPresenter::_MovePage(_In_ int offSet)
{
    if (!_pCandidateWnd)
    {
        return FALSE;
    }
    BOOL ret = _pCandidateWnd->_MovePage(offSet, TRUE);
    if (ret)
    {
        CPinyinIpc::SendSetSelection((int)_pCandidateWnd->_GetSelection());
        if (_isShowMode)
        {
            _pCandidateWnd->_InvalidateRect();
        }
        else
        {
            _updatedFlags = TF_CLUIE_SELECTION |
                TF_CLUIE_CURRENTPAGE;
            _UpdateUIElement();
        }
    }
    return ret;
}

//+---------------------------------------------------------------------------
//
// _MoveWindowToTextExt
//
//----------------------------------------------------------------------------

void CCandidateListUIPresenter::_MoveWindowToTextExt()
{
    RECT rc;

    if (FAILED(_GetTextExt(&rc)))
    {
        return;
    }

    if (_pCandidateWnd)
    {
        _pCandidateWnd->_Move(rc.left, rc.bottom);
    }

    CPinyinIpc::DebugLog(L"SendPos[MoveToTextExt] x=%d y=%d", rc.left, rc.bottom);
    CPinyinIpc::SendSetPosition(rc.left, rc.bottom);
}
//+---------------------------------------------------------------------------
//
// _LayoutChangeNotification
//
//----------------------------------------------------------------------------

VOID CCandidateListUIPresenter::_LayoutChangeNotification(_In_ RECT *lpRect)
{
    // 候选窗由 WPF 服务进程渲染：DLL 只上报"光标物理坐标"（插入符左下角），
    // 贴边/翻转/DPI 换算统一由 Server 端 CandidatePlacement 完成。
    // 原来经 _GetWindowExtent 用虚拟窗口矩形计算：虚拟窗口记录历史 _Move 位置，
    // 与文本坐标叠加产生漂移（942→3620）；候选数多时窗口宽度巨大又触发
    // OVER_RIGHT 误贴边到屏幕右缘——两条错误源头，全部移除。
    if (_pCandidateWnd)
    {
        _pCandidateWnd->_Move(lpRect->left, lpRect->bottom);
    }

    CPinyinIpc::DebugLog(L"SendPos[LayoutChange] x=%d y=%d", lpRect->left, lpRect->bottom);
    CPinyinIpc::SendSetPosition(lpRect->left, lpRect->bottom);
}

//+---------------------------------------------------------------------------
//
// _LayoutDestroyNotification
//
//----------------------------------------------------------------------------

VOID CCandidateListUIPresenter::_LayoutDestroyNotification()
{
    _EndCandidateList();
}

//+---------------------------------------------------------------------------
//
// _CandidateChangeNotifiction
//
//----------------------------------------------------------------------------

HRESULT CCandidateListUIPresenter::_CandidateChangeNotification(_In_ enum CANDWND_ACTION action)
{
    HRESULT hr = E_FAIL;

    TfClientId tfClientId = _pTextService->_GetClientId();
    ITfThreadMgr* pThreadMgr = nullptr;
    ITfDocumentMgr* pDocumentMgr = nullptr;
    ITfContext* pContext = nullptr;

    _KEYSTROKE_STATE KeyState;
    KeyState.Category = _Category;
    KeyState.Function = FUNCTION_FINALIZE_CANDIDATELIST;

    if (CAND_ITEM_SELECT != action)
    {
        goto Exit;
    }

    pThreadMgr = _pTextService->_GetThreadMgr();
    if (nullptr == pThreadMgr)
    {
        goto Exit;
    }

    hr = pThreadMgr->GetFocus(&pDocumentMgr);
    if (FAILED(hr))
    {
        goto Exit;
    }

    hr = pDocumentMgr->GetTop(&pContext);
    if (FAILED(hr))
    {
        pDocumentMgr->Release();
        goto Exit;
    }

    CKeyHandlerEditSession *pEditSession = new (std::nothrow) CKeyHandlerEditSession(_pTextService, pContext, 0, 0, KeyState);
    if (nullptr != pEditSession)
    {
        HRESULT hrSession = S_OK;
        hr = pContext->RequestEditSession(tfClientId, pEditSession, TF_ES_SYNC | TF_ES_READWRITE, &hrSession);
        if (hrSession == TF_E_SYNCHRONOUS || hrSession == TS_E_READONLY)
        {
            hr = pContext->RequestEditSession(tfClientId, pEditSession, TF_ES_ASYNC | TF_ES_READWRITE, &hrSession);
        }
        pEditSession->Release();
    }

    pContext->Release();
    pDocumentMgr->Release();

Exit:
    return hr;
}

//+---------------------------------------------------------------------------
//
// _CandWndCallback
//
//----------------------------------------------------------------------------

// static
HRESULT CCandidateListUIPresenter::_CandWndCallback(_In_ void *pv, _In_ enum CANDWND_ACTION action)
{
    CCandidateListUIPresenter* fakeThis = (CCandidateListUIPresenter*)pv;

    return fakeThis->_CandidateChangeNotification(action);
}

//+---------------------------------------------------------------------------
//
// _UpdateUIElement
//
//----------------------------------------------------------------------------

HRESULT CCandidateListUIPresenter::_UpdateUIElement()
{
    HRESULT hr = S_OK;

    ITfThreadMgr* pThreadMgr = _pTextService->_GetThreadMgr();
    if (nullptr == pThreadMgr)
    {
        return S_OK;
    }

    ITfUIElementMgr* pUIElementMgr = nullptr;

    hr = pThreadMgr->QueryInterface(IID_ITfUIElementMgr, (void **)&pUIElementMgr);
    if (hr == S_OK)
    {
        pUIElementMgr->UpdateUIElement(_uiElementId);
        pUIElementMgr->Release();
    }

    return S_OK;
}

//+---------------------------------------------------------------------------
//
// OnSetThreadFocus
//
//----------------------------------------------------------------------------

HRESULT CCandidateListUIPresenter::OnSetThreadFocus()
{
    if (_isShowMode)
    {
        Show(TRUE);
    }
    return S_OK;
}

//+---------------------------------------------------------------------------
//
// OnKillThreadFocus
//
//----------------------------------------------------------------------------

HRESULT CCandidateListUIPresenter::OnKillThreadFocus()
{
    if (_isShowMode)
    {
        Show(FALSE);
    }
    return S_OK;
}

void CCandidateListUIPresenter::RemoveSpecificCandidateFromList(_In_ LCID Locale, _Inout_ CSampleImeArray<CCandidateListItem> &candidateList, _In_ CStringRange &candidateString)
{
    for (UINT index = 0; index < candidateList.Count();)
    {
        CCandidateListItem* pLI = candidateList.GetAt(index);

        if (CStringRange::Compare(Locale, &candidateString, &pLI->_ItemString) == CSTR_EQUAL)
        {
            candidateList.RemoveAt(index);
            continue;
        }

        index++;
    }
}

void CCandidateListUIPresenter::AdviseUIChangedByArrowKey(_In_ KEYSTROKE_FUNCTION arrowKey)
{
    switch (arrowKey)
    {
    case FUNCTION_MOVE_UP:
        {
            _MoveSelection(MOVEUP_ONE);
            break;
        }
    case FUNCTION_MOVE_DOWN:
        {
            _MoveSelection(MOVEDOWN_ONE);
            break;
        }
    case FUNCTION_MOVE_PAGE_UP:
        {
            _MovePage(MOVEUP_ONE);
            break;
        }
    case FUNCTION_MOVE_PAGE_DOWN:
        {
            _MovePage(MOVEDOWN_ONE);
            break;
        }
    case FUNCTION_MOVE_PAGE_TOP:
        {
            _SetSelection(MOVETO_TOP);
            break;
        }
    case FUNCTION_MOVE_PAGE_BOTTOM:
        {
            _SetSelection(MOVETO_BOTTOM);
            break;
        }
    default:
        break;
    }
}

HRESULT CCandidateListUIPresenter::BeginUIElement()
{
    HRESULT hr = S_OK;

    ITfThreadMgr* pThreadMgr = _pTextService->_GetThreadMgr();
    if (nullptr ==pThreadMgr)
    {
        hr = E_FAIL;
        goto Exit;
    }

    ITfUIElementMgr* pUIElementMgr = nullptr;
    hr = pThreadMgr->QueryInterface(IID_ITfUIElementMgr, (void **)&pUIElementMgr);
    if (hr == S_OK)
    {
        pUIElementMgr->BeginUIElement(this, &_isShowMode, &_uiElementId);
        pUIElementMgr->Release();
    }

Exit:
    return hr;
}

HRESULT CCandidateListUIPresenter::EndUIElement()
{
    HRESULT hr = S_OK;

    ITfThreadMgr* pThreadMgr = _pTextService->_GetThreadMgr();
    if ((nullptr == pThreadMgr) || (-1 == _uiElementId))
    {
        hr = E_FAIL;
        goto Exit;
    }

    ITfUIElementMgr* pUIElementMgr = nullptr;
    hr = pThreadMgr->QueryInterface(IID_ITfUIElementMgr, (void **)&pUIElementMgr);
    if (hr == S_OK)
    {
        pUIElementMgr->EndUIElement(_uiElementId);
        pUIElementMgr->Release();
    }

Exit:
    return hr;
}

HRESULT CCandidateListUIPresenter::MakeCandidateWindow(_In_ ITfContext *pContextDocument, _In_ UINT wndWidth)
{
    HRESULT hr = S_OK;

    if (nullptr != _pCandidateWnd)
    {
        return hr;
    }

    _pCandidateWnd = new (std::nothrow) CCandidateWindow(_CandWndCallback, this, _pIndexRange, _pTextService->_IsStoreAppMode());
    if (_pCandidateWnd == nullptr)
    {
        hr = E_OUTOFMEMORY;
        goto Exit;
    }

    HWND parentWndHandle = nullptr;
    ITfContextView* pView = nullptr;
    if (SUCCEEDED(pContextDocument->GetActiveView(&pView)))
    {
        pView->GetWnd(&parentWndHandle);
    }

    if (!_pCandidateWnd->_Create(_atom, wndWidth, parentWndHandle))
    {
        hr = E_OUTOFMEMORY;
        goto Exit;
    }

Exit:
    return hr;
}

void CCandidateListUIPresenter::DisposeCandidateWindow()
{
    if (nullptr == _pCandidateWnd)
    {
        return;
    }

    _pCandidateWnd->_Destroy();

    delete _pCandidateWnd;
    _pCandidateWnd = nullptr;
}