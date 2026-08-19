//+---------------------------------------------------------------------------
//
//  engine_debug_stub.cpp
//
//  PinyinEngine.cpp 依赖 CPinyinIpc::DebugLog（DLL 侧实现）。
//  引擎进程为独立 exe，此处提供桩实现：日志写入 engine_debug.log。
//
//----------------------------------------------------------------------------

#include "PinyinIpc.h"
#include "PathUtil.h"
#include <cstdio>
#include <cstdarg>

void CPinyinIpc::DebugLog(_In_ const WCHAR* fmt, ...)
{
    WCHAR buf[512] = {0};
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, args);
    va_end(args);

    const std::wstring logPath = EnginePaths::DataFile(L"engine_debug.log");

    // 日志轮转：超过 64MB 改名为 .old 重新写，防止异常循环把日志无限膨胀
    //（2026-08-15 安装事故中 engine_debug.log 曾膨胀到 230MB）。
    const ULONGLONG kLogMax = 64ULL * 1024 * 1024;
    WIN32_FILE_ATTRIBUTE_DATA la{};
    if (GetFileAttributesExW(logPath.c_str(), GetFileExInfoStandard, &la))
    {
        const ULONGLONG logSize =
            ((ULONGLONG)la.nFileSizeHigh << 32) | la.nFileSizeLow;
        if (logSize > kLogMax)
        {
            MoveFileExW(logPath.c_str(), (logPath + L".old").c_str(),
                MOVEFILE_REPLACE_EXISTING);
        }
    }

    FILE* f = nullptr;
    _wfopen_s(&f, logPath.c_str(), L"a, ccs=UTF-8");
    if (f)
    {
        fwprintf(f, L"%lu\t%s\n", GetTickCount(), buf);
        fclose(f);
    }
}
