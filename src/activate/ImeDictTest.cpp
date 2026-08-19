// ============================================================
//  ImeDictTest - 字典排序诊断（开发用，独立测试）
//  直接链接 PinyinEngine.cpp，实测各输入在三条候选路径下的输出。
// ============================================================
#include <windows.h>
#include <cstdio>
#include "PinyinEngine.h"
#include "PathUtil.h"

// 输出到文件（终端被 trae-sandbox 包装，WriteConsoleW 不可见）
static FILE *g_f = nullptr;

static void Out(const wchar_t *s)
{
    if (g_f) fwprintf(g_f, L"%s", s);
}

static void OutLine(const wchar_t *fmt, ...)
{
    wchar_t buf[512] = {};
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    Out(buf);
    Out(L"\n");
    if (g_f) fflush(g_f);
}

static void Dump(CPinyinEngine &eng, const wchar_t *key, int kind)
{
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);

    CStringRange r;
    r.Set(const_cast<WCHAR *>(key), wcslen(key));
    CSampleImeArray<CCandidateListItem> list;
    QueryPerformanceCounter(&t0);
    if (kind == 0) eng.CollectWordForWildcard(&r, &list);
    else if (kind == 1) eng.CollectWordByInitial(&r, &list);
    else if (kind == 2) eng.CollectWord(&r, &list);
    QueryPerformanceCounter(&t1);
    double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;

    const char *kn = kind == 0 ? "wild" : kind == 1 ? "init" : "full";
    OutLine(L"== %s (%hs) -> %u items  [%.2f ms]", key, kn, list.Count(), ms);
    for (UINT i = 0; i < list.Count() && i < 15; i++)
    {
        const WCHAR *s = list.GetAt(i)->_ItemString.Get();
        OutLine(L"   [%2u] %s", i + 1, s);
    }
    OutLine(L"");
}

int wmain()
{
    _wfopen_s(&g_f, L"g:\\pinyin-plus\\dist\\dict_test_out.txt", L"w, ccs=UTF-8");

    CPinyinEngine eng;
    BOOL ok = eng.Initialize(L"g:\\pinyin-plus\\bin\\pinyin-plus-big.txt");
    OutLine(L"dict init: %d", ok);
    if (!ok) return 1;

    Dump(eng, L"d", 0);     // 输入 d 走的前缀路径
    Dump(eng, L"d", 1);     // 简拼回退
    Dump(eng, L"da", 0);
    Dump(eng, L"dan", 0);
    Dump(eng, L"zhong", 0);
    Dump(eng, L"xian", 0);
    Dump(eng, L"de", 0);    // 完整音节 de
    Dump(eng, L"l", 0);
    Dump(eng, L"le", 0);
    Dump(eng, L"sh", 0);
    Dump(eng, L"sjx", 1);   // 简拼符号直达
    return 0;
}
