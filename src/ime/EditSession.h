// THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
// PARTICULAR PURPOSE.
//
// Copyright (c) Microsoft Corporation. All rights reserved


#pragma once

#include <string>

class CSampleIME;

class CEditSessionBase : public ITfEditSession
{
public:
    CEditSessionBase(_In_ CSampleIME *pTextService, _In_ ITfContext *pContext);
    virtual ~CEditSessionBase();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, _Outptr_ void **ppvObj);
    STDMETHODIMP_(ULONG) AddRef(void);
    STDMETHODIMP_(ULONG) Release(void);

    // ITfEditSession
    virtual STDMETHODIMP DoEditSession(TfEditCookie ec) = 0;

protected:
    ITfContext *_pContext;
    CSampleIME *_pTextService;

private:
    LONG _refCount;     // COM ref count
};

//+---------------------------------------------------------------------------
//
// CCandidateSelectEditSession — 鼠标点击候选 → 按全局索引选字
//
// 由 IPC 读线程触发（经 message-only 窗口投递到 TSF 线程），
// 在焦点上下文的编辑会话里执行 _HandleCandidateSelectByGlobalIndex。
//
//----------------------------------------------------------------------------

class CCandidateSelectEditSession : public CEditSessionBase
{
public:
    CCandidateSelectEditSession(_In_ CSampleIME *pTextService, _In_ ITfContext *pContext, _In_ UINT globalIndex);

    // ITfEditSession
    STDMETHODIMP DoEditSession(TfEditCookie ec) override;

private:
    UINT _globalIndex;
};

//+---------------------------------------------------------------------------
//
// CCandidateTextCommandEditSession — 服务进程文本命令（右键删词 / 符号上屏）
//
// 由 IPC 读线程触发（经 message-only 窗口投递到 TSF 线程），在焦点上下文的
// 编辑会话里执行：删除用户词（并刷新候选）或向光标处插入文本。
//
//----------------------------------------------------------------------------

enum class TextCommandKind
{
    DeleteUserWord,
    DemoteWord,
    InsertText,
};

class CCandidateTextCommandEditSession : public CEditSessionBase
{
public:
    CCandidateTextCommandEditSession(_In_ CSampleIME *pTextService, _In_ ITfContext *pContext,
                                     _In_ TextCommandKind kind, _In_ const std::wstring& text);

    // ITfEditSession
    STDMETHODIMP DoEditSession(TfEditCookie ec) override;

private:
    TextCommandKind _kind;
    std::wstring _text;
};
