// ImeDictTest 专用 DebugLog 桩：写临时日志文件（沙箱允许 g: 盘）。
#include "PinyinIpc.h"
#include <cstdio>
#include <cstdarg>

void CPinyinIpc::DebugLog(_In_ const WCHAR* fmt, ...)
{
    FILE* f = nullptr;
    _wfopen_s(&f, L"g:\\pinyin-plus\\dist\\dict_debug.log", L"a, ccs=UTF-8");
    if (!f) return;
    WCHAR buf[1024] = {};
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, args);
    va_end(args);
    fwprintf(f, L"%s\n", buf);
    fclose(f);
}
