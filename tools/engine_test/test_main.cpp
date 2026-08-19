// 引擎性能测试：加载 bin/pinyin-plus.txt（86.9 万条），实测各查询路径单次耗时
#include "Private.h"
#include "PinyinEngine.h"
#include "PinyinIpc.h"
#include <cstdio>
#include <chrono>
#include <string>
#include <cstdarg>

static FILE* g_out = nullptr;

// 桩实现：CPinyinIpc::DebugLog（引擎进程版在 src/engine/engine_debug_stub.cpp）
void CPinyinIpc::DebugLog(_In_ const WCHAR* fmt, ...)
{
    WCHAR buf[512] = { 0 };
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, args);
    va_end(args);
    FILE* f = nullptr;
    _wfopen_s(&f, L"g:\\pinyin-plus\\tools\\engine_test\\debug.log", L"a, ccs=UTF-8");
    if (f)
    {
        fwprintf(f, L"%lu\t%s\n", GetTickCount(), buf);
        fclose(f);
    }
}

// 复刻 EnginePipe::QueryCandidates 的完整查询链路（整句预测已禁用，跳过）
static int QueryChain(CPinyinEngine& engine, const wchar_t* pinyin)
{
    CSampleImeArray<CCandidateListItem> list;
    CStringRange range;
    range.Set((WCHAR*)pinyin, wcslen(pinyin));

    // 2. 前缀词（全拼增量）
    std::wstring wild = std::wstring(pinyin) + L"*";
    CStringRange wrange;
    wrange.Set((WCHAR*)wild.c_str(), (DWORD_PTR)wild.size());
    engine.CollectWordForWildcard(&wrange, &list);
    if (list.Count() > 0)
    {
        return list.Count();
    }

    // 3. 简拼回退
    engine.CollectWordByInitial(&range, &list);
    if (list.Count() > 0)
    {
        return list.Count();
    }

    // 4. 混合简拼/全拼
    engine.CollectWordByMixed(&range, &list);
    if (list.Count() > 0)
    {
        return list.Count();
    }

    // 5. 模糊音
    if (wcslen(pinyin) >= 3)
    {
        engine.CollectWordFuzzy(&range, &list);
    }

    // 6. 长输入按音节拼单字
    if (wcslen(pinyin) >= 6)
    {
        engine.CollectSingleCharsByKey(&range, &list);
    }
    return list.Count();
}

int wmain()
{
    g_out = _wfopen(L"g:\\pinyin-plus\\tools\\engine_test\\result.txt", L"w, ccs=UTF-8");
    if (!g_out)
    {
        wprintf(L"cannot open result.txt\n");
        return 2;
    }

    CPinyinEngine engine;
    auto t0 = std::chrono::steady_clock::now();
    if (!engine.Initialize(L"g:\\pinyin-plus\\bin\\pinyin-plus.txt"))
    {
        fwprintf(g_out, L"Initialize FAILED\n");
        fclose(g_out);
        return 1;
    }
    auto t1 = std::chrono::steady_clock::now();
    double loadMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    fwprintf(g_out, L"== 词库加载: %.0f ms ==\n", loadMs);

    // ---- 加载后立即测傅兴亮简拼/混合（此时未做任何 AddUserWord/BoostWord 修改）----
    fwprintf(g_out, L"\n== 加载后立即查询（对照：无任何修改）==\n");
    {
        const wchar_t* queries[] = { L"fxl", L"fuxl" };
        for (const wchar_t* q : queries)
        {
            CSampleImeArray<CCandidateListItem> qlist;
            CStringRange qrange;
            qrange.Set((WCHAR*)q, wcslen(q));   // 必须初始化！否则简拼/混合收到空 key
            std::wstring wild = std::wstring(q) + L"*";
            CStringRange wrange;
            wrange.Set((WCHAR*)wild.c_str(), (DWORD_PTR)wild.size());
            engine.CollectWordForWildcard(&wrange, &qlist);
            if (qlist.Count() == 0)
            {
                engine.CollectWordByInitial(&qrange, &qlist);
                if (qlist.Count() == 0)
                {
                    engine.CollectWordByMixed(&qrange, &qlist);
                }
            }
            std::wstring cands;
            bool foundFXL = false;
            for (UINT i = 0; i < qlist.Count() && i < 12; i++)
            {
                CStringRange* p = &qlist.GetAt(i)->_ItemString;
                std::wstring w(p->Get(), p->GetLength());
                if (i > 0) cands += L" / ";
                cands += w;
                if (w == L"傅兴亮") foundFXL = true;
            }
            fwprintf(g_out, L"%-12ls 傅兴亮可达=%s (共%d候选): %s\n",
                q, foundFXL ? L"是" : L"否", qlist.Count(), cands.c_str());
        }
    }

    // 用例：模拟真实击键序列（每行一个输入串）
    const wchar_t* cases[] = {
        L"z", L"zh", L"zhe", L"zheg", L"zhege",          // 增量输入"这个"
        L"c", L"ch", L"che",                              // 增量输入"车"
        L"w", L"wo", L"wox", L"woxi", L"woxia", L"woxiang", L"woxiangn", L"woxiangni", // 增量"我想你"
        L"n", L"ni", L"nih", L"niha", L"nihao",           // 增量"你好"
        L"zhurongji",                                     // 朱镕基
        L"fuxl",                                          // 混合简拼（傅兴亮）
        L"shurufa", L"diannao", L"xinwenlianbo",          // 多音节
        L"sjx", L"qdy", L"rmb", L"wh",                    // 符号简拼直达（△/≌/¥/？）
        L"sanjiao", L"quandengyu",                        // 符号全拼
    };
    const int REPS = 50;  // 每用例重复次数取平均，降低噪声
    double maxMs = 0, totalMs = 0;
    int totalQueries = 0;

    for (const wchar_t* c : cases)
    {
        double best = 1e9;
        double worst = 0;
        int count = 0;
        for (int r = 0; r < REPS; r++)
        {
            auto s = std::chrono::steady_clock::now();
            count = QueryChain(engine, c);
            auto e = std::chrono::steady_clock::now();
            double ms = std::chrono::duration<double, std::milli>(e - s).count();
            if (ms < best) best = ms;
            if (ms > worst) worst = ms;
        }
        fwprintf(g_out, L"%-14ls 命中%-3d  最坏 %.2f ms  最好 %.3f ms\n", c, count, worst, best);
        if (worst > maxMs) maxMs = worst;
        totalMs += worst;
        totalQueries++;
    }
    fwprintf(g_out, L"--- 最坏单次: %.2f ms | 平均(最坏): %.2f ms ---\n", maxMs, totalMs / totalQueries);

    // ---- 造词性能与正确性（AddUserWord 局部重排优化验证）----
    fwprintf(g_out, L"\n== 造词（AddUserWord）测试 ==\n");

    // 新词：敢干敢拼（用户目标用例；切分任务未做，这里验证造词入库与局部维护）
    {
        auto s = std::chrono::steady_clock::now();
        engine.AddUserWord(L"ganganganpin", 12, L"敢干敢拼", 4);
        auto e = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(e - s).count();
        fwprintf(g_out, L"造新词 敢干敢拼: %.2f ms\n", ms);
    }
    // 已有词 freq+1（如 你好 nihao）
    {
        auto s = std::chrono::steady_clock::now();
        engine.AddUserWord(L"nihao", 5, L"你好", 2);
        auto e = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(e - s).count();
        fwprintf(g_out, L"已有词 你好 +1: %.2f ms\n", ms);
    }
    // BoostWord 已有词（简拼索引 freq 序维护）
    {
        auto s = std::chrono::steady_clock::now();
        engine.BoostWord(L"shijie", 6, L"世界", 2);
        auto e = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(e - s).count();
        fwprintf(g_out, L"BoostWord 世界: %.2f ms\n", ms);
    }
    // 造词后再造同词（应走 freq+1 分支）
    {
        auto s = std::chrono::steady_clock::now();
        engine.AddUserWord(L"ganganganpin", 12, L"敢干敢拼", 4);
        auto e = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(e - s).count();
        fwprintf(g_out, L"再造 敢干敢拼(已有): %.2f ms\n", ms);
    }

    // 正确性：全拼 / 简拼 / 混合是否可达新词
    {
        CSampleImeArray<CCandidateListItem> list;
        CStringRange range;
        // 全拼前缀
        std::wstring w1 = L"ganganganpin*";
        range.Set((WCHAR*)w1.c_str(), (DWORD_PTR)w1.size());
        engine.CollectWordForWildcard(&range, &list);
        bool foundFull = false;
        for (UINT i = 0; i < list.Count(); i++)
        {
            CStringRange* p = &list.GetAt(i)->_ItemString;
            if (std::wstring(p->Get(), p->GetLength()) == L"敢干敢拼") foundFull = true;
        }
        fwprintf(g_out, L"全拼可达 敢干敢拼: %s\n", foundFull ? L"是" : L"否(失败)");

        // 简拼可达性：当前贪心切分 [gang,ang,an,pin] → initial=gaap（任务2切分优化后变 gggp）
        const wchar_t* initCases[] = { L"gaap", L"gggp" };
        for (const wchar_t* ic : initCases)
        {
            list.Clear();
            std::wstring w2 = ic;
            range.Set((WCHAR*)w2.c_str(), (DWORD_PTR)w2.size());
            engine.CollectWordByInitial(&range, &list);
            bool foundInit = false;
            for (UINT i = 0; i < list.Count(); i++)
            {
                CStringRange* p = &list.GetAt(i)->_ItemString;
                if (std::wstring(p->Get(), p->GetLength()) == L"敢干敢拼") foundInit = true;
            }
            fwprintf(g_out, L"简拼 %s 可达: %s\n", ic, foundInit ? L"是" : L"否");
        }

        // 符号简拼优先：sjx 候选首位应为 △（不被"三角形/数据线"等高频词淹没）
        {
            list.Clear();
            std::wstring w2 = L"sjx";
            range.Set((WCHAR*)w2.c_str(), (DWORD_PTR)w2.size());
            engine.CollectWordByInitial(&range, &list);
            std::wstring first;
            if (list.Count() > 0)
            {
                CStringRange* p = &list.GetAt(0)->_ItemString;
                first.assign(p->Get(), p->GetLength());
            }
            fwprintf(g_out, L"符号简拼 sjx 首位: %s (共%d候选, 期望△)\n",
                first.c_str(), list.Count());
        }

        // 混合 ganganganpin → [gan,gan,gan,pin] 声母串 gggp（注意：当前贪心切分 [gang,ang,an,pin] 声母串 gaap，此项预期可达性取决于切分，仅打印）
        list.Clear();
        std::wstring w3 = L"ganganganpin";
        range.Set((WCHAR*)w3.c_str(), (DWORD_PTR)w3.size());
        engine.CollectWordByMixed(&range, &list);
        bool foundMixed = false;
        for (UINT i = 0; i < list.Count(); i++)
        {
            CStringRange* p = &list.GetAt(i)->_ItemString;
            if (std::wstring(p->Get(), p->GetLength()) == L"敢干敢拼") foundMixed = true;
        }
        fwprintf(g_out, L"混合可达 敢干敢拼(依赖切分): %s\n", foundMixed ? L"是" : L"否");
    }

    // ---- 傅兴亮：全拼 / 简拼 / 混合 可达性（用户实测 fxl 出不来）----
    fwprintf(g_out, L"\n== 傅兴亮三种输入可达性 ==\n");
    {
        const wchar_t* queries[] = { L"fuxingliang", L"fxl", L"fuxl", L"fuxingl", L"fxliang" };
        for (const wchar_t* q : queries)
        {
            CSampleImeArray<CCandidateListItem> qlist;
            CStringRange qrange;
            qrange.Set((WCHAR*)q, wcslen(q));   // 必须初始化！否则简拼/混合收到空 key
            // 复刻 QueryCandidates：整句禁用(跳过) → 前缀 → 简拼 → 混合 → 模糊 → 组词
            std::wstring wild = std::wstring(q) + L"*";
            CStringRange wrange;
            wrange.Set((WCHAR*)wild.c_str(), (DWORD_PTR)wild.size());
            engine.CollectWordForWildcard(&wrange, &qlist);
            if (qlist.Count() == 0)
            {
                engine.CollectWordByInitial(&qrange, &qlist);
                if (qlist.Count() == 0)
                {
                    engine.CollectWordByMixed(&qrange, &qlist);
                }
            }
            // 打印候选（前 12 个）
            std::wstring cands;
            bool foundFXL = false;
            for (UINT i = 0; i < qlist.Count() && i < 12; i++)
            {
                CStringRange* p = &qlist.GetAt(i)->_ItemString;
                std::wstring w(p->Get(), p->GetLength());
                if (i > 0) cands += L" / ";
                cands += w;
                if (w == L"傅兴亮") foundFXL = true;
            }
            fwprintf(g_out, L"%-12ls 傅兴亮可达=%s (共%d候选): %s\n",
                q, foundFXL ? L"是" : L"否", qlist.Count(), cands.c_str());
        }
    }

    // ---- 造词后立即查（模拟用户场景：insert 中间插入破坏索引的回归验证）----
    fwprintf(g_out, L"\n== 造词后立即查（insert 下标平移修复验证）==\n");
    {
        // 1. 再造一次傅兴亮（found 分支，freq+1）
        engine.AddUserWord(L"fuxingliang", 11, L"傅兴亮", 3);
        // 2. 真正的新词：fuxiyang（插入 f 区中间，触发 insert 平移）
        engine.AddUserWord(L"fuxiyang", 8, L"傅西扬", 3);

        const wchar_t* queries[] = { L"fuxingliang", L"fxl", L"fuxl", L"fuxiyang", L"fuxy" };
        for (const wchar_t* q : queries)
        {
            CSampleImeArray<CCandidateListItem> qlist;
            CStringRange qrange;
            qrange.Set((WCHAR*)q, wcslen(q));
            std::wstring wild = std::wstring(q) + L"*";
            CStringRange wrange;
            wrange.Set((WCHAR*)wild.c_str(), (DWORD_PTR)wild.size());
            engine.CollectWordForWildcard(&wrange, &qlist);
            if (qlist.Count() == 0)
            {
                engine.CollectWordByInitial(&qrange, &qlist);
                if (qlist.Count() == 0)
                {
                    engine.CollectWordByMixed(&qrange, &qlist);
                }
            }
            std::wstring cands;
            bool foundFXL = false, foundFXY = false;
            for (UINT i = 0; i < qlist.Count() && i < 12; i++)
            {
                CStringRange* p = &qlist.GetAt(i)->_ItemString;
                std::wstring w(p->Get(), p->GetLength());
                if (i > 0) cands += L" / ";
                cands += w;
                if (w == L"傅兴亮") foundFXL = true;
                if (w == L"傅西扬") foundFXY = true;
            }
            fwprintf(g_out, L"%-12ls 傅兴亮=%s 傅西扬=%s (共%d): %s\n",
                q, foundFXL ? L"是" : L"否", foundFXY ? L"是" : L"否", qlist.Count(), cands.c_str());
        }
    }

    // ---- 任务2：切分优化验证（完整音节全切分 + 打分）----
    fwprintf(g_out, L"\n== 切分（SegmentToSyllablesBest）测试 ==\n");
    {
        const wchar_t* segCases[] = { L"ganganganpin", L"zhongguo", L"xian", L"xianjing", L"nihao", L"fuxingliang", L"diannao", L"zhurongji" };
        for (const wchar_t* sc : segCases)
        {
            auto s = std::chrono::steady_clock::now();
            std::vector<std::wstring> syls = engine.SegmentToSyllablesBest(sc);
            auto e = std::chrono::steady_clock::now();
            double ms = std::chrono::duration<double, std::milli>(e - s).count();
            std::wstring joined;
            for (const std::wstring &sy : syls)
            {
                if (!joined.empty()) joined += L"-";
                joined += sy;
            }
            fwprintf(g_out, L"%-16ls -> %-28ls  %.3f ms\n", sc, joined.c_str(), ms);
        }
    }

    // ---- 引擎管道探针：连接真实引擎服务（与 DLL 同款 API）----
    fwprintf(g_out, L"\n== 引擎管道探针 ==\n");
    {
        HANDLE hPipe = CreateFileW(L"\\\\.\\pipe\\PinyinPlus.Engine", GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (hPipe == INVALID_HANDLE_VALUE)
        {
            fwprintf(g_out, L"连接引擎管道失败 err=%lu\n", GetLastError());
        }
        else
        {
            fwprintf(g_out, L"已连接引擎管道\n");
            std::vector<BYTE> frame;
            auto u32 = [&](unsigned v) { for (int i = 0; i < 4; i++) frame.push_back((BYTE)(v >> (8 * i))); };
            u32(0x5050494D); u32(1); u32(1); u32(5);
            const char* py = "nihao";
            for (int i = 0; i < 5; i++) frame.push_back((BYTE)py[i]);
            DWORD wr = 0;
            BOOL ok = WriteFile(hPipe, frame.data(), (DWORD)frame.size(), &wr, nullptr);
            fwprintf(g_out, L"WriteFile ok=%d wr=%lu err=%lu\n", ok, wr, GetLastError());
            // 带超时读响应头（PeekNamedPipe 轮询，避免永久阻塞）
            BYTE hdr[16]; DWORD rd = 0;
            DWORD t0 = GetTickCount();
            while (GetTickCount() - t0 < 10000)
            {
                DWORD avail = 0;
                if (PeekNamedPipe(hPipe, nullptr, 0, nullptr, &avail, nullptr) && avail >= 16)
                {
                    ok = ReadFile(hPipe, hdr, 16, &rd, nullptr);
                    break;
                }
                Sleep(100);
            }
            fwprintf(g_out, L"ReadFile ok=%d rd=%lu err=%lu 耗时=%lums\n", ok, rd, GetLastError(), GetTickCount() - t0);
            if (ok && rd == 16)
            {
                unsigned type = hdr[8] | (hdr[9] << 8) | (hdr[10] << 16) | (hdr[11] << 24);
                unsigned plen = hdr[12] | (hdr[13] << 8) | (hdr[14] << 16) | (hdr[15] << 24);
                fwprintf(g_out, L"响应 type=%u plen=%u\n", type, plen);
            }
            CloseHandle(hPipe);
        }
    }

    fclose(g_out);
    return 0;
}
