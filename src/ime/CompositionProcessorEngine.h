// THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
// PARTICULAR PURPOSE.
//
// Copyright (c) Microsoft Corporation. All rights reserved


#pragma once

#include "sal.h"
#include "KeyHandlerEditSession.h"
#include "SampleIMEBaseStructure.h"
#include "Compartment.h"
#include "define.h"
#include "EngineClient.h"
#include <deque>
#include <string>

class CCompositionProcessorEngine
{
public:
    CCompositionProcessorEngine(void);
    ~CCompositionProcessorEngine(void);

    BOOL SetupLanguageProfile(LANGID langid, REFGUID guidLanguageProfile, _In_ ITfThreadMgr *pThreadMgr, TfClientId tfClientId, BOOL isSecureMode, BOOL isComLessMode);

    // Get language profile.
    GUID GetLanguageProfile(LANGID *plangid)
    {
        *plangid = _langid;
        return _guidProfile;
    }
    // Get locale
    LCID GetLocale()
    {
        return MAKELCID(_langid, SORT_DEFAULT);
    }

    BOOL IsVirtualKeyNeed(UINT uCode, _In_reads_(1) WCHAR *pwch, BOOL fComposing, CANDIDATE_MODE candidateMode, BOOL hasCandidateWithWildcard, _Out_opt_ _KEYSTROKE_STATE *pKeyState);

    BOOL AddVirtualKey(WCHAR wch);
    void RemoveVirtualKey(DWORD_PTR dwIndex);
    void PurgeVirtualKey();

    DWORD_PTR GetVirtualKeyLength() { return _keystrokeBuffer.GetLength(); }
    WCHAR GetVirtualKey(DWORD_PTR dwIndex);

    void GetReadingStrings(_Inout_ CSampleImeArray<CStringRange> *pReadingStrings, _Out_ BOOL *pIsWildcardIncluded);
    void GetCandidateList(_Inout_ CSampleImeArray<CCandidateListItem> *pCandidateList, BOOL isIncrementalWordSearch, BOOL isWildcardSearch);
    void GetCandidateStringInConverted(CStringRange &searchString, _In_ CSampleImeArray<CCandidateListItem> *pCandidateList);

    // Preserved key handler
    void OnPreservedKey(REFGUID rguid, _Out_ BOOL *pIsEaten, _In_ ITfThreadMgr *pThreadMgr, TfClientId tfClientId);

    // Punctuation
    BOOL IsPunctuation(WCHAR wch);
    // 返回标点字符串（支持多字符：……、——）。全角模式下全角化，英文标点模式直出原键。
    std::wstring GetPunctuation(WCHAR wch);

    BOOL IsDoubleSingleByte(WCHAR wch);
    BOOL IsWildcard() { return _isWildcard; }
    BOOL IsDisableWildcardAtFirst() { return _isDisableWildcardAtFirst; }
    BOOL IsWildcardChar(WCHAR wch) { return ((IsWildcardOneChar(wch) || IsWildcardAllChar(wch)) ? TRUE : FALSE); }
    BOOL IsWildcardOneChar(WCHAR wch) { return (wch==L'?' ? TRUE : FALSE); }
    BOOL IsWildcardAllChar(WCHAR wch) { return (wch==L'*' ? TRUE : FALSE); }
    BOOL IsMakePhraseFromText() { return _hasMakePhraseFromText; }
    BOOL IsKeystrokeSort() { return _isKeystrokeSort; }

    // Dictionary engine
    // 词库在独立引擎进程(PinyinPlus.Engine.exe)内，由 CEngineClient 自动拉起/重连。
    BOOL IsDictionaryAvailable() { return TRUE; }

    // 用户选词：提升 (pinyin, word) 词频并持久化（自学习词库）→ 引擎进程
    void BoostWord(_In_ const WCHAR *pinyin, DWORD_PTR pinyinLen, _In_ const WCHAR *word, DWORD_PTR wordLen)
    {
        CEngineClient::BoostWord(std::wstring(pinyin, static_cast<size_t>(pinyinLen)),
            std::wstring(word, static_cast<size_t>(wordLen)));
    }

    // 精确查询 (pinyin, word) 是否在词库中 → 引擎进程
    BOOL IsEntry(_In_ const std::wstring &pinyin, _In_ const std::wstring &word) const
    {
        return FALSE;   // 引擎进程持有词库；本接口用于查重，现由引擎侧去重，返回 FALSE 不影响
    }

    // 用户造词（分段选字完成的新词）入库：词库无则新增（含简拼），下次全拼/简拼直达 → 引擎进程
    void AddUserWord(_In_ const WCHAR *pinyin, DWORD_PTR pinyinLen, _In_ const WCHAR *word, DWORD_PTR wordLen)
    {
        CEngineClient::AddUserWord(std::wstring(pinyin, static_cast<size_t>(pinyinLen)),
            std::wstring(word, static_cast<size_t>(wordLen)));
    }

    // ---- 造词模式（分段选字）状态 ----
    BOOL IsMakeWordMode() const { return _isMakeWordMode; }

    // 进入造词：记录音节序列与完整拼音，段索引置 0
    void EnterMakeWordMode(_In_ std::vector<std::wstring> syls, _In_ const std::wstring &fullKey)
    {
        _isMakeWordMode = TRUE;
        _makeWordSyls = std::move(syls);
        _makeWordFullKey = fullKey;
        _makeWordChars.clear();
        _makeWordSegIndex = 0;
    }

    // 退出造词（Esc / 普通输入接管）
    void ExitMakeWordMode()
    {
        _isMakeWordMode = FALSE;
        _makeWordSyls.clear();
        _makeWordFullKey.clear();
        _makeWordChars.clear();
        _makeWordSegIndex = 0;
    }

    // 从当前输入缓冲进入造词模式（候选模式按 0 键触发）。
    // 输入需能切出 ≥2 个音节，且每段音节都有单字候选（引擎查询），否则放弃。
    BOOL EnterMakeWordModeFromCurrentInput()
    {
        if (_isMakeWordMode || _keystrokeBuffer.GetLength() == 0)
        {
            return FALSE;
        }
        std::wstring fullKey(_keystrokeBuffer.Get(), _keystrokeBuffer.GetLength());
        std::vector<std::wstring> syls;
        if (!CEngineClient::SegmentToSyllables(fullKey, syls) || syls.size() < 2)
        {
            return FALSE;
        }
        // 每段都须有单字候选，否则后续段无法完成选字
        for (const std::wstring &s : syls)
        {
            std::vector<std::wstring> chars;
            if (!CEngineClient::QuerySyllableChars(s, chars) || chars.empty())
            {
                return FALSE;
            }
        }
        EnterMakeWordMode(std::move(syls), fullKey);
        return TRUE;
    }

    UINT GetMakeWordSegIndex() const { return _makeWordSegIndex; }
    void SetMakeWordSegIndex(UINT i) { _makeWordSegIndex = i; }
    UINT GetMakeWordSylCount() const { return static_cast<UINT>(_makeWordSyls.size()); }
    const std::wstring &GetMakeWordChars() const { return _makeWordChars; }
    void AppendMakeWordChar(_In_ const WCHAR *ch, size_t len) { if (ch && len) { _makeWordChars.append(ch, len); } }
    // 回退一段：删除最后选的字，段索引回退
    void BackspaceMakeWord()
    {
        if (!_makeWordChars.empty())
        {
            _makeWordChars.pop_back();   // 最后一段是一个汉字
        }
        if (_makeWordSegIndex > 0)
        {
            _makeWordSegIndex--;
        }
    }
    const std::wstring &GetMakeWordFullKey() const { return _makeWordFullKey; }

    // 已选字 + 当前段拼音（候选窗拼音缓冲显示，如 "傅xing"）
    std::wstring GetMakeWordDisplayBuffer() const;

    // 当前段音节的单字候选（追加到列表）
    void GetMakeWordCandidates(_Inout_ CSampleImeArray<CCandidateListItem> *pItemList);

    // Language bar control
    void SetLanguageBarStatus(DWORD status, BOOL isSet);

    void ConversionModeCompartmentUpdated(_In_ ITfThreadMgr *pThreadMgr);

    void ShowAllLanguageBarIcons();
    void HideAllLanguageBarIcons();

    inline CCandidateRange *GetCandidateListIndexRange() { return &_candidateListIndexRange; }
    inline UINT GetCandidateListPhraseModifier() { return _candidateListPhraseModifier; }
    inline UINT GetCandidateWindowWidth() { return _candidateWndWidth; }

private:
    void InitKeyStrokeTable();
    BOOL InitLanguageBar(_In_ CLangBarItemButton *pLanguageBar, _In_ ITfThreadMgr *pThreadMgr, TfClientId tfClientId, REFGUID guidCompartment);

    struct _KEYSTROKE;
    BOOL IsVirtualKeyKeystrokeComposition(UINT uCode, _Out_opt_ _KEYSTROKE_STATE *pKeyState, KEYSTROKE_FUNCTION function);
    BOOL IsVirtualKeyKeystrokeCandidate(UINT uCode, _In_ _KEYSTROKE_STATE *pKeyState, CANDIDATE_MODE candidateMode, _Out_ BOOL *pfRetCode, _In_ CSampleImeArray<_KEYSTROKE> *pKeystrokeMetric);
    BOOL IsKeystrokeRange(UINT uCode, _Out_ _KEYSTROKE_STATE *pKeyState, CANDIDATE_MODE candidateMode);

    void SetupKeystroke();
    void SetupPreserved(_In_ ITfThreadMgr *pThreadMgr, TfClientId tfClientId);
    void SetupConfiguration();
    void SetupLanguageBar(_In_ ITfThreadMgr *pThreadMgr, TfClientId tfClientId, BOOL isSecureMode);
    void SetKeystrokeTable(_Inout_ CSampleImeArray<_KEYSTROKE> *pKeystroke);
    void SetupPunctuationPair();
    void CreateLanguageBarButton(DWORD dwEnable, GUID guidLangBar, _In_z_ LPCWSTR pwszDescriptionValue, _In_z_ LPCWSTR pwszTooltipValue, DWORD dwOnIconIndex, DWORD dwOffIconIndex, _Outptr_result_maybenull_ CLangBarItemButton **ppLangBarItemButton, BOOL isSecureMode);
    void SetInitialCandidateListRange();
    void SetDefaultCandidateTextFont();
	void InitializeSampleIMECompartment(_In_ ITfThreadMgr *pThreadMgr, TfClientId tfClientId);

    class XPreservedKey;
    void SetPreservedKey(const CLSID clsid, TF_PRESERVEDKEY & tfPreservedKey, _In_z_ LPCWSTR pwszDescription, _Out_ XPreservedKey *pXPreservedKey);
    BOOL InitPreservedKey(_In_ XPreservedKey *pXPreservedKey, _In_ ITfThreadMgr *pThreadMgr, TfClientId tfClientId);
    BOOL CheckShiftKeyOnly(_In_ CSampleImeArray<TF_PRESERVEDKEY> *pTSFPreservedKeyTable);

    static HRESULT CompartmentCallback(_In_ void *pv, REFGUID guidCompartment);
    void PrivateCompartmentsUpdated(_In_ ITfThreadMgr *pThreadMgr);
    void KeyboardOpenCompartmentUpdated(_In_ ITfThreadMgr *pThreadMgr);

    
    BOOL SetupDictionaryFile();

private:
    // 造词模式（分段选字）状态
    BOOL _isMakeWordMode;
    std::vector<std::wstring> _makeWordSyls;   // 音节序列 [fu, xing, liang]
    std::wstring _makeWordChars;               // 已选字 "傅兴"
    std::wstring _makeWordFullKey;             // 完整拼音 "fuxingliang"
    UINT _makeWordSegIndex;                    // 当前段索引

    // 引擎返回候选的稳定存储池（deque 保证地址稳定，候选 _ItemString 引用池中内存）
    std::deque<std::wstring> _candidatePool;

    struct _KEYSTROKE
    {
        UINT VirtualKey;
        UINT Modifiers;
        KEYSTROKE_FUNCTION Function;

        _KEYSTROKE()
        {
            VirtualKey = 0;
            Modifiers = 0;
            Function = FUNCTION_NONE;
        }
    };
    _KEYSTROKE _keystrokeTable[26];

    CStringRange _keystrokeBuffer;

    BOOL _hasWildcardIncludedInKeystrokeBuffer;

    LANGID _langid;
    GUID _guidProfile;
    TfClientId  _tfClientId;

    CSampleImeArray<_KEYSTROKE> _KeystrokeComposition;
    CSampleImeArray<_KEYSTROKE> _KeystrokeCandidate;
    CSampleImeArray<_KEYSTROKE> _KeystrokeCandidateWildcard;
    CSampleImeArray<_KEYSTROKE> _KeystrokeCandidateSymbol;
    CSampleImeArray<_KEYSTROKE> _KeystrokeSymbol;

    // Preserved key data
    class XPreservedKey
    {
    public:
        XPreservedKey();
        ~XPreservedKey();
        BOOL UninitPreservedKey(_In_ ITfThreadMgr *pThreadMgr);

    public:
        CSampleImeArray<TF_PRESERVEDKEY> TSFPreservedKeyTable;
        GUID Guid;
        LPCWSTR Description;
    };

    XPreservedKey _PreservedKey_IMEMode;
    XPreservedKey _PreservedKey_DoubleSingleByte;
    XPreservedKey _PreservedKey_Punctuation;

    // Punctuation data
    CSampleImeArray<CPunctuationPair> _PunctuationPair;
    CSampleImeArray<CPunctuationNestPair> _PunctuationNestPair;

    // Language bar data
    CLangBarItemButton* _pLanguageBar_IMEMode;
    CLangBarItemButton* _pLanguageBar_DoubleSingleByte;
    CLangBarItemButton* _pLanguageBar_Punctuation;

    // Compartment
    CCompartment* _pCompartmentConversion;
    CCompartmentEventSink* _pCompartmentConversionEventSink;
    CCompartmentEventSink* _pCompartmentKeyboardOpenEventSink;
    CCompartmentEventSink* _pCompartmentDoubleSingleByteEventSink;
    CCompartmentEventSink* _pCompartmentPunctuationEventSink;

    // Configuration data
    BOOL _isWildcard : 1;
    BOOL _isDisableWildcardAtFirst : 1;
    BOOL _hasMakePhraseFromText : 1;
    BOOL _isKeystrokeSort : 1;
    BOOL _isComLessMode : 1;
    CCandidateRange _candidateListIndexRange;
    UINT _candidateListPhraseModifier;
    UINT _candidateWndWidth;

    static const int OUT_OF_FILE_INDEX = -1;
};

