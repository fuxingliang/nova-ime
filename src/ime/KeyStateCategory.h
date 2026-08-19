// THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
// PARTICULAR PURPOSE.
//
// Copyright (c) Microsoft Corporation. All rights reserved

#pragma once
#include "Globals.h"
#include "Private.h"
#include "SampleIME.h"

class CKeyStateCategory;

class CKeyStateCategoryFactory
{
public:
    static CKeyStateCategoryFactory* Instance();
    CKeyStateCategory* MakeKeyStateCategory(KEYSTROKE_CATEGORY keyCategory, _In_ CSampleIME *pTextService);
    void Release();

protected:
    CKeyStateCategoryFactory();

private:
    static CKeyStateCategoryFactory* _instance;

};

typedef struct KeyHandlerEditSessionDTO
{
    KeyHandlerEditSessionDTO::KeyHandlerEditSessionDTO(TfEditCookie tFEC, _In_ ITfContext *pTfContext, UINT virualCode, WCHAR inputChar, KEYSTROKE_FUNCTION arrowKeyFunction)
    {
        ec = tFEC;
        pContext = pTfContext;
        code = virualCode;
        wch = inputChar;
        arrowKey = arrowKeyFunction;
    }

    TfEditCookie ec;
    ITfContext* pContext;
    UINT code;
    WCHAR wch;
    KEYSTROKE_FUNCTION arrowKey;
}KeyHandlerEditSessionDTO;

class CKeyStateCategory
{
public:
    CKeyStateCategory(_In_ CSampleIME *pTextService);

protected:
    ~CKeyStateCategory(void);

public:
    HRESULT KeyStateHandler(KEYSTROKE_FUNCTION function, KeyHandlerEditSessionDTO dto);
    void Release(void);

protected:
    // HandleKeyInput
    virtual HRESULT HandleKeyInput(KeyHandlerEditSessionDTO dto);

    // HandleKeyFinalizeTextStoreAndInput
    virtual HRESULT HandleKeyFinalizeTextStoreAndInput(KeyHandlerEditSessionDTO dto);

    // HandleKeyFinalizeTextStore
    virtual HRESULT HandleKeyFinalizeTextStore(KeyHandlerEditSessionDTO dto);

    // HandleKeyFinalizeCandidatelistAndInput
    virtual HRESULT HandleKeyFinalizeCandidatelistAndInput(KeyHandlerEditSessionDTO dto);

    // HandleKeyFinalizeCandidatelist
    virtual HRESULT HandleKeyFinalizeCandidatelist(KeyHandlerEditSessionDTO dto);

    // HandleKeyConvert
    virtual HRESULT HandleKeyConvert(KeyHandlerEditSessionDTO dto);

    // HandleKeyConvertWild
    virtual HRESULT HandleKeyConvertWildCard(KeyHandlerEditSessionDTO dto);

    // HandleKeyCancel
    virtual HRESULT HandleKeyCancel(KeyHandlerEditSessionDTO dto);

    // HandleKeyBackspace
    virtual HRESULT HandleKeyBackspace(KeyHandlerEditSessionDTO dto);

    // HandleKeyArrow
    virtual HRESULT HandleKeyArrow(KeyHandlerEditSessionDTO dto);

    // HandleKeyDoubleSingleByte
    virtual HRESULT HandleKeyDoubleSingleByte(KeyHandlerEditSessionDTO dto);

    // HandleKeyPunctuation
    virtual HRESULT HandleKeyPunctuation(KeyHandlerEditSessionDTO dto);

    // HandleKeySelectByNumber
    virtual HRESULT HandleKeySelectByNumber(KeyHandlerEditSessionDTO dto);

    // HandleKeyEnterMakeWord（候选模式按 0 键 → 进入造词模式）
    virtual HRESULT HandleKeyEnterMakeWord(KeyHandlerEditSessionDTO dto);

    // HandleKeyDeleteUserWord（候选模式 Ctrl+Delete → 删除选中用户词）
    virtual HRESULT HandleKeyDeleteUserWord(KeyHandlerEditSessionDTO dto);

    // HandleKeyDemoteWord（候选模式 Ctrl+PageDown → 降权沉底当前词）
    virtual HRESULT HandleKeyDemoteWord(KeyHandlerEditSessionDTO dto);

protected:
    CSampleIME* _pTextService;
};

class CKeyStateComposing : public CKeyStateCategory
{
public:
    CKeyStateComposing(_In_ CSampleIME *pTextService);

protected:
    // _HandleCompositionInput
    HRESULT HandleKeyInput(KeyHandlerEditSessionDTO dto);

    // HandleKeyCompositionFinalizeTextStoreAndInput
    HRESULT HandleKeyFinalizeTextStoreAndInput(KeyHandlerEditSessionDTO dto);

    // HandleKeyFinalizeTextStore
    HRESULT HandleKeyFinalizeTextStore(KeyHandlerEditSessionDTO dto);

    // HandleKeyCompositionFinalizeCandidatelistAndInput
    HRESULT HandleKeyFinalizeCandidatelistAndInput(KeyHandlerEditSessionDTO dto);

    // HandleKeyCompositionFinalizeCandidatelist
    HRESULT HandleKeyFinalizeCandidatelist(KeyHandlerEditSessionDTO dto);

    // HandleCompositionConvert
    HRESULT HandleKeyConvert(KeyHandlerEditSessionDTO dto);

    // HandleKeyCompositionConvertWildCard
    HRESULT HandleKeyConvertWildCard(KeyHandlerEditSessionDTO dto);

    // HandleCancel
    HRESULT HandleKeyCancel(KeyHandlerEditSessionDTO dto);

    // HandleCompositionBackspace
    HRESULT HandleKeyBackspace(KeyHandlerEditSessionDTO dto);

    // HandleArrowKey
    HRESULT HandleKeyArrow(KeyHandlerEditSessionDTO dto);

    // HandleKeyDoubleSingleByte
    HRESULT HandleKeyDoubleSingleByte(KeyHandlerEditSessionDTO dto);

    // HandleKeyCompositionPunctuation
    HRESULT HandleKeyPunctuation(KeyHandlerEditSessionDTO dto);
};

class CKeyStateCandidate : public CKeyStateCategory
{
public:
    CKeyStateCandidate(_In_ CSampleIME *pTextService);

protected:
    // HandleKeyFinalizeTextStore（回车：直接上屏拼音原文，不转汉字，便于输入英文）
    HRESULT HandleKeyFinalizeTextStore(KeyHandlerEditSessionDTO dto);

    // HandleKeyFinalizeCandidatelist
    HRESULT HandleKeyFinalizeCandidatelist(KeyHandlerEditSessionDTO dto);

    // HandleKeyFinalizeCandidatelistAndInput
    HRESULT HandleKeyFinalizeCandidatelistAndInput(KeyHandlerEditSessionDTO dto);

    //_HandleCandidateConvert
    HRESULT HandleKeyConvert(KeyHandlerEditSessionDTO dto);

    //_HandleCancel
    HRESULT HandleKeyCancel(KeyHandlerEditSessionDTO dto);

    //_HandleCandidateArrowKey
    HRESULT HandleKeyArrow(KeyHandlerEditSessionDTO dto);

    //_HandleCandidateSelectByNumber
    HRESULT HandleKeySelectByNumber(KeyHandlerEditSessionDTO dto);

    // 按 0 键 → 进入造词模式（分段选字）
    HRESULT HandleKeyEnterMakeWord(KeyHandlerEditSessionDTO dto);

    // Ctrl+Delete → 删除当前选中的用户词
    HRESULT HandleKeyDeleteUserWord(KeyHandlerEditSessionDTO dto);

    // Ctrl+PageDown → 降权沉底当前选中的词
    HRESULT HandleKeyDemoteWord(KeyHandlerEditSessionDTO dto);
};

class CKeyStatePhrase : public CKeyStateCategory
{
public:
    CKeyStatePhrase(_In_ CSampleIME *pTextService);

protected:
    // HandleKeyFinalizeTextStore（回车：直接上屏拼音原文，不转汉字，便于输入英文）
    HRESULT HandleKeyFinalizeTextStore(KeyHandlerEditSessionDTO dto);

    //_HandleCancel
    HRESULT HandleKeyFinalizeCandidatelist(KeyHandlerEditSessionDTO dto);

    //_HandleCancel
    HRESULT HandleKeyCancel(KeyHandlerEditSessionDTO dto);

    //_HandlePhraseArrowKey
    HRESULT HandleKeyArrow(KeyHandlerEditSessionDTO dto);

    //_HandlePhraseSelectByNumber
    HRESULT HandleKeySelectByNumber(KeyHandlerEditSessionDTO dto);
};

//degeneration class
class CKeyStateNull : public CKeyStateCategory
{
public:
    CKeyStateNull(_In_ CSampleIME *pTextService) : CKeyStateCategory(pTextService) {};

protected:
    // _HandleNullInput
    HRESULT HandleKeyInput(KeyHandlerEditSessionDTO dto) { return __super::HandleKeyInput(dto); };

    // HandleKeyNullFinalizeTextStoreAndInput
    HRESULT HandleKeyFinalizeTextStoreAndInput(KeyHandlerEditSessionDTO dto) { return __super::HandleKeyFinalizeTextStoreAndInput(dto); };

    // HandleKeyFinalizeTextStore
    HRESULT HandleKeyFinalizeTextStore(KeyHandlerEditSessionDTO dto) { return __super::HandleKeyFinalizeTextStore(dto); };

    // HandleKeyNullFinalizeCandidatelistAndInput
    HRESULT HandleKeyFinalizeCandidatelistAndInput(KeyHandlerEditSessionDTO dto) { return __super::HandleKeyFinalizeCandidatelistAndInput(dto); };

    // HandleKeyNullFinalizeCandidatelist
    HRESULT HandleKeyFinalizeCandidatelist(KeyHandlerEditSessionDTO dto) { return __super::HandleKeyFinalizeCandidatelist(dto); };

    //_HandleNullConvert
    HRESULT HandleKeyConvert(KeyHandlerEditSessionDTO dto) { return __super::HandleKeyConvert(dto); };

    //_HandleNullCancel
    HRESULT HandleKeyCancel(KeyHandlerEditSessionDTO dto) { return __super::HandleKeyCancel(dto); };

    // HandleKeyNullConvertWild
    HRESULT HandleKeyConvertWildCard(KeyHandlerEditSessionDTO dto) { return __super::HandleKeyConvertWildCard(dto); };

    //_HandleNullBackspace
    HRESULT HandleKeyBackspace(KeyHandlerEditSessionDTO dto) { return __super::HandleKeyBackspace(dto); };

    //_HandleNullArrowKey
    HRESULT HandleKeyArrow(KeyHandlerEditSessionDTO dto) { return __super::HandleKeyArrow(dto); };

    // HandleKeyDoubleSingleByte
    HRESULT HandleKeyDoubleSingleByte(KeyHandlerEditSessionDTO dto) { return __super::HandleKeyDoubleSingleByte(dto); };

    // HandleKeyPunctuation
    HRESULT HandleKeyPunctuation(KeyHandlerEditSessionDTO dto) { return __super::HandleKeyPunctuation(dto); };

    //_HandleNullCandidateSelectByNumber
    HRESULT HandleKeySelectByNumber(KeyHandlerEditSessionDTO dto) { return __super::HandleKeySelectByNumber(dto); };
};