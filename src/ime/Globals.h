// THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
// PARTICULAR PURPOSE.
//
// Copyright (c) Microsoft Corporation. All rights reserved

#pragma once

#include "private.h"
#include "define.h"
#include "SampleIMEBaseStructure.h"
#include <string>

void DllAddRef();
void DllRelease();


namespace Global {
//---------------------------------------------------------------------
// inline
//---------------------------------------------------------------------

inline void SafeRelease(_In_ IUnknown *punk)
{
    if (punk != nullptr)
    {
        punk->Release();
    }
}

inline void QuickVariantInit(_Inout_ VARIANT *pvar)
{
    pvar->vt = VT_EMPTY;
}

inline void QuickVariantClear(_Inout_ VARIANT *pvar)
{
    switch (pvar->vt) 
    {
    // some ovbious VTs that don't need to call VariantClear.
    case VT_EMPTY:
    case VT_NULL:
    case VT_I2:
    case VT_I4:
    case VT_R4:
    case VT_R8:
    case VT_CY:
    case VT_DATE:
    case VT_I1:
    case VT_UI1:
    case VT_UI2:
    case VT_UI4:
    case VT_I8:
    case VT_UI8:
    case VT_INT:
    case VT_UINT:
    case VT_BOOL:
        break;

        // Call release for VT_UNKNOWN.
    case VT_UNKNOWN:
        SafeRelease(pvar->punkVal);
        break;

    default:
        // we call OleAut32 for other VTs.
        VariantClear(pvar);
        break;
    }
    pvar->vt = VT_EMPTY;
}

//+---------------------------------------------------------------------------
//
// IsTooSimilar
//
//  Return TRUE if the colors cr1 and cr2 are so similar that they
//  are hard to distinguish. Used for deciding to use reverse video
//  selection instead of system selection colors.
//
//----------------------------------------------------------------------------

inline BOOL IsTooSimilar(COLORREF cr1, COLORREF cr2)
{
    if ((cr1 | cr2) & 0xFF000000)        // One color and/or the other isn't RGB, so algorithm doesn't apply
    {
        return FALSE;
    }

    LONG DeltaR = abs(GetRValue(cr1) - GetRValue(cr2));
    LONG DeltaG = abs(GetGValue(cr1) - GetGValue(cr2));
    LONG DeltaB = abs(GetBValue(cr1) - GetBValue(cr2));

    return DeltaR + DeltaG + DeltaB < 80;
}

//---------------------------------------------------------------------
// extern
//---------------------------------------------------------------------
extern HINSTANCE dllInstanceHandle;

extern ATOM AtomCandidateWindow;
extern ATOM AtomShadowWindow;
extern ATOM AtomScrollBarWindow;

BOOL RegisterWindowClass();

extern LONG dllRefCount;

extern CRITICAL_SECTION CS;
extern HFONT defaultlFontHandle;  // Global font object we use everywhere

extern const CLSID SampleIMECLSID;
extern const CLSID SampleIMEGuidProfile;
extern const CLSID SampleIMEGuidImeModePreserveKey;
extern const CLSID SampleIMEGuidDoubleSingleBytePreserveKey;
extern const CLSID SampleIMEGuidPunctuationPreserveKey;

LRESULT CALLBACK ThreadKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);
BOOL CheckModifiers(UINT uModCurrent, UINT uMod);
BOOL UpdateModifiers(WPARAM wParam, LPARAM lParam);

extern USHORT ModifiersValue;
extern BOOL IsShiftKeyDownOnly;
extern BOOL IsControlKeyDownOnly;
extern BOOL IsAltKeyDownOnly;

extern const GUID SampleIMEGuidCompartmentDoubleSingleByte;
extern const GUID SampleIMEGuidCompartmentPunctuation;

extern const WCHAR FullWidthCharTable[];
extern const struct _PUNCTUATION PunctuationTable[14];

// ---- engine.conf 配置开关（候选窗状态按钮/设置面板写入，DLL 实时读取） ----
// 全角模式（width=1）：上屏文本/标点对 ASCII 全角化（0xFF01-0xFF5E）
BOOL IsFullWidthModeEnabled();
// 中文标点模式（punct=1，默认开）：标点键映射中文标点；关闭则直出半角 ASCII
BOOL IsChinesePunctuationEnabled();
// 搜狗式拆字（charsel=1，默认开）：候选态选中多字词时 `;`=第1字、`'`=第2字，
// 只上屏该单字（剩余字丢弃，搜狗式）；单字候选/开关关时回落普通标点行为
BOOL IsCharSplitEnabled();

extern const GUID SampleIMEGuidLangBarIMEMode;
extern const GUID SampleIMEGuidLangBarDoubleSingleByte;
extern const GUID SampleIMEGuidLangBarPunctuation;

extern const GUID SampleIMEGuidDisplayAttributeInput;
extern const GUID SampleIMEGuidDisplayAttributeConverted;

extern const GUID SampleIMEGuidCandUIElement;

extern const WCHAR UnicodeByteOrderMark;
extern const WCHAR KeywordDelimiter;
extern const WCHAR StringDelimiter;

extern const WCHAR ImeModeDescription[];
extern const int ImeModeOnIcoIndex;
extern const int ImeModeOffIcoIndex;

extern const WCHAR DoubleSingleByteDescription[];
extern const int DoubleSingleByteOnIcoIndex;
extern const int DoubleSingleByteOffIcoIndex;

extern const WCHAR PunctuationDescription[];
extern const int PunctuationOnIcoIndex;
extern const int PunctuationOffIcoIndex;

extern const WCHAR LangbarImeModeDescription[];
extern const WCHAR LangbarDoubleSingleByteDescription[];
extern const WCHAR LangbarPunctuationDescription[];
}

// ---- 运行时路径（安装包化：程序目录 + 数据目录分离，全局命名空间） ----
//  安装目录 = 本 DLL 所在目录（只读：Engine.exe / 词库 pinyin-plus*.txt / OpenCC 表）
//  数据目录 = %AppData%\NovaInput（可写：engine.conf / userdict.txt / 日志）
//  InitializePaths 在 DLL_PROCESS_ATTACH 时调用一次（纯 Win32，无 CRT 依赖）。
//  注意：崩溃观察器等异常路径只读 g_dataDir/g_installDir（避免堆分配），
//  不用 GetDataPath/GetInstallPath（内部构造 std::wstring）。
extern std::wstring g_installDir;
extern std::wstring g_dataDir;
void InitializePaths();
std::wstring GetInstallPath(const WCHAR* fileName);   // 安装目录 + 文件名
std::wstring GetDataPath(const WCHAR* fileName);      // 数据目录 + 文件名