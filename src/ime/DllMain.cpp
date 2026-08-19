// THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
// PARTICULAR PURPOSE.
//
// Copyright (c) Microsoft Corporation. All rights reserved

#include "Private.h"
#include "Globals.h"
#include "PinyinIpc.h"
#include "EngineClient.h"

//+---------------------------------------------------------------------------
//
//  IME 崩溃观察器（向量异常处理）
//
//  背景：TSF DLL 是进程内注入，DLL 内任何未处理异常都会直接砸在宿主
//  （Trae/QQ 等）的进程里。此前出现"部署后打字宿主即退出"但零崩溃记录
//  的疑难：WER/crashpad 都没有 dump，无法判断是不是 DLL 抛了异常。
//
//  本观察器通过 AddVectoredExceptionHandler 观察进程内所有致命异常并写
//  日志（纯 Win32 文件 IO，不依赖 CRT——QQ 崩溃的教训）。只记录不处理
//  （EXCEPTION_CONTINUE_SEARCH），不干扰宿主自身的异常链，也不影响
//  调试器。若宿主退出但本日志无记录 → 可判定宿主主动退出，与 DLL 无关。
//
//----------------------------------------------------------------------------

static volatile LONG s_crashLogCount = 0;

static void LogImeCrash(DWORD code, DWORD_PTR addr)
{
    // 限频：同一次异常风暴最多记 20 条（防止宿主反复抛异常刷爆日志）
    if (InterlockedIncrement(&s_crashLogCount) > 20)
    {
        return;
    }

    // 定位异常地址所在模块（完整路径 + 基址 + 模块内偏移 RVA）
    WCHAR modPath[MAX_PATH] = L"<unknown>";
    DWORD_PTR moduleBase = 0;
    HMODULE hMod = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCWSTR)addr, &hMod) && hMod)
    {
        GetModuleFileNameW(hMod, modPath, MAX_PATH);
        moduleBase = (DWORD_PTR)hMod;
    }

    WCHAR buf[900] = { 0 };
    wsprintfW(buf, L"CRASH code=0x%08X addr=0x%p module=%s base=0x%p rva=0x%p",
        code, (void*)addr, modPath, (void*)moduleBase,
        (void*)(addr - moduleBase));

    // 崩溃日志 → 数据目录（%AppData%\NovaInput\ime_crash.log）
    // 注意：异常路径禁止堆分配（GetDataPath 内部构造 std::wstring），
    // 直接只读 g_dataDir 全局字符串 + 栈缓冲拼接。
    WCHAR logPath[MAX_PATH] = { 0 };
    if (g_dataDir.empty())
    {
        swprintf_s(logPath, MAX_PATH, L"ime_crash.log");
    }
    else
    {
        swprintf_s(logPath, MAX_PATH, L"%s\\ime_crash.log", g_dataDir.c_str());
    }

    HANDLE h = CreateFileW(logPath,
        FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE)
    {
        char utf8[1024] = { 0 };
        int ulen = WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8,
            (int)sizeof(utf8) - 1, nullptr, nullptr);
        if (ulen > 1)
        {
            DWORD written = 0;
            WriteFile(h, utf8, (DWORD)(ulen - 1), &written, nullptr);
            static const char crlf[] = "\r\n";
            WriteFile(h, crlf, 2, &written, nullptr);
        }
        CloseHandle(h);
    }
}

static LONG CALLBACK ImeVectoredHandler(PEXCEPTION_POINTERS pExc)
{
    if (!pExc || !pExc->ExceptionRecord)
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    DWORD code = pExc->ExceptionRecord->ExceptionCode;
    // 只观察致命异常：访问违规 / 非法指令 / 除零 / 栈溢出 / 特权指令 / .NET 异常
    if (code == 0xC0000005 || code == 0xC000001D || code == 0xC0000094 ||
        code == 0xC0000096 || code == 0xC00000FD || code == 0xE0434352)
    {
        LogImeCrash(code, (DWORD_PTR)pExc->ExceptionRecord->ExceptionAddress);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

//+---------------------------------------------------------------------------
//
//  ImeSehFilter —— SEH 兜底过滤器（供各 TSF 回调的 __except 使用）
//
//  TSF 回调（OnKeyDown/OnEndEdit/OnSetThreadFocus 等）被宿主直接调用，且宿主
//  （Chromium/Trae）会在输入法重置时释放我们的引擎对象（Deactivate），导致
//  悬垂指针崩溃连带宿主退出。用 __try/__except 包裹回调后，任何异常在这里
//  被吞掉（记录日志便于定位），回调返回安全值，宿主不再退出。
//
//----------------------------------------------------------------------------

int ImeSehFilter(PEXCEPTION_POINTERS pExc)
{
    if (pExc && pExc->ExceptionRecord)
    {
        LogImeCrash(pExc->ExceptionRecord->ExceptionCode,
            (DWORD_PTR)pExc->ExceptionRecord->ExceptionAddress);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

//+---------------------------------------------------------------------------
//
// DllMain
//
//----------------------------------------------------------------------------

BOOL WINAPI DllMain(HINSTANCE hInstance, DWORD dwReason, LPVOID pvReserved)
{
	pvReserved;

    switch (dwReason)
    {
    case DLL_PROCESS_ATTACH:

        Global::dllInstanceHandle = hInstance;

        // 初始化运行时路径（安装目录/数据目录）——必须在崩溃观察器注册前，
        // 保证任何后续异常日志都能写出
        InitializePaths();

        if (!InitializeCriticalSectionAndSpinCount(&Global::CS, 0))
        {
            return FALSE;
        }

        if (!Global::RegisterWindowClass()) {
            return FALSE;
        }

        // 安装崩溃观察器：记录 DLL/宿主进程内的致命异常（取证用，不干预）
        AddVectoredExceptionHandler(1, ImeVectoredHandler);

        CPinyinIpc::Initialize();
        CEngineClient::Initialize();

        break;

    case DLL_PROCESS_DETACH:

        RemoveVectoredExceptionHandler(ImeVectoredHandler);

        CEngineClient::Uninitialize();
        CPinyinIpc::Uninitialize();

        DeleteCriticalSection(&Global::CS);

        break;

    case DLL_THREAD_ATTACH:

        break;

    case DLL_THREAD_DETACH:

        break;
    }

    return TRUE;
}
