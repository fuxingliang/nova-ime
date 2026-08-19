// THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
// PARTICULAR PURPOSE.
//
// Copyright (c) Microsoft Corporation. All rights reserved

#include "Private.h"
#include "EditSession.h"
#include "GetTextExtentEditSession.h"
#include "TfTextLayoutSink.h"

//+---------------------------------------------------------------------------
//
// ctor
//
//----------------------------------------------------------------------------

CGetTextExtentEditSession::CGetTextExtentEditSession(_In_ CSampleIME *pTextService, _In_ ITfContext *pContext, _In_ ITfContextView *pContextView, _In_ ITfRange *pRangeComposition, _In_ CTfTextLayoutSink *pTfTextLayoutSink) : CEditSessionBase(pTextService, pContext)
{
    _pContextView = pContextView;
    _pRangeComposition = pRangeComposition;
    _pTfTextLayoutSink = pTfTextLayoutSink;
}

//+---------------------------------------------------------------------------
//
// ITfEditSession::DoEditSession
//
//----------------------------------------------------------------------------

STDAPI CGetTextExtentEditSession::DoEditSession(TfEditCookie ec)
{
    // 统一走 _GetTextExt：组合 GetTextExt 失败/零矩形（拼音隐藏模式，组合不写入
    // 编辑区）时回退到插入符(selection)位置，并做 DPI 坐标归一化。
    // 此前这里直接调 GetTextExt(组合范围)，与 _StartCandidateList/_MoveWindowToTextExt
    // 两条链路取的坐标不一致——UE 等应用组合范围 GetTextExt 返回异常坐标时，
    // 候选窗被这条链路的错误值覆盖而错位。
    (void)ec;

    RECT rc = {0, 0, 0, 0};
    if (SUCCEEDED(_pTfTextLayoutSink->_GetTextExt(&rc)))
    {
        _pTfTextLayoutSink->_LayoutChangeNotification(&rc);
    }

    return S_OK;
}
