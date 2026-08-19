//+---------------------------------------------------------------------------
//
//  EnginePipe.cpp
//
//  引擎进程管道服务实现。
//
//----------------------------------------------------------------------------

#include "EnginePipe.h"
#include "TraditionalConvert.h"
#include "PathUtil.h"
#include <cstdio>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <sddl.h>

// ---- 双缓冲热重载（2026-08-13）----
// 查询/写操作始终访问 g_engine（引擎锁内取指针）；type 11 触发后台线程用
// g_reloadEngine 重建（Initialize 全量加载），完成后加锁原子切换并释放旧实例。
// 收益：导入词库时查询零阻塞（后台线程不持有引擎锁，仅切换瞬间抢锁微秒级）。
static CPinyinEngine* g_engine = nullptr;        // 当前服务实例
static CPinyinEngine* g_reloadEngine = nullptr;  // 后台重建实例（非空 = 重建中）

static void TriggerReload();   // 双缓冲热重载入口（定义见下方，case 11 调用）

static void EngineLog(const WCHAR* fmt, ...)
{
    // 日志锁初始化（2026-08-13 修复竞态）：与 g_engineLock 同款隐患 —— 连接风暴下
    // 多连接线程首次并发进入会同时 InitializeCriticalSection，损坏锁对象导致
    // EnterCriticalSection 永久挂起（日志停止、连接线程全卡死、进程假死）。
    static CRITICAL_SECTION s_cs;
    static std::once_flag s_csOnce;
    std::call_once(s_csOnce, []() { InitializeCriticalSection(&s_cs); });

    WCHAR buf[512] = {0};
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, args);
    va_end(args);

    EnterCriticalSection(&s_cs);
    FILE* f = nullptr;
    _wfopen_s(&f, EnginePaths::DataFile(L"engine_debug.log").c_str(), L"a, ccs=UTF-8");
    if (f)
    {
        fwprintf(f, L"%lu\t%s\n", GetTickCount(), buf);
        fclose(f);
    }
    LeaveCriticalSection(&s_cs);
}

// ---- 帧编解码 ----

static void AppendU32(std::vector<BYTE>& v, unsigned int value)
{
    v.push_back(static_cast<BYTE>(value & 0xFF));
    v.push_back(static_cast<BYTE>((value >> 8) & 0xFF));
    v.push_back(static_cast<BYTE>((value >> 16) & 0xFF));
    v.push_back(static_cast<BYTE>((value >> 24) & 0xFF));
}

static void AppendUtf8(std::vector<BYTE>& v, const std::wstring& s)
{
    int len = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    AppendU32(v, static_cast<unsigned int>(len > 0 ? len : 0));
    if (len > 0)
    {
        std::vector<BYTE> utf8(len);
        WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
            reinterpret_cast<LPSTR>(utf8.data()), len, nullptr, nullptr);
        v.insert(v.end(), utf8.begin(), utf8.end());
    }
}

// 完整读一帧（16 字节头 + payload）。返回 true 表示成功。
static bool ReadFrame(HANDLE hPipe, unsigned int& type, std::vector<BYTE>& payload)
{
    BYTE header[16];
    DWORD read = 0;
    if (!ReadFile(hPipe, header, 16, &read, nullptr) || read != 16)
    {
        EngineLog(L"ReadFrame header fail read=%lu err=%lu", read, GetLastError());
        return false;
    }
    unsigned int magic = header[0] | (header[1] << 8) | (header[2] << 16) | (header[3] << 24);
    if (magic != 0x5050494D)
    {
        EngineLog(L"ReadFrame bad magic=0x%08X (got %02X %02X %02X %02X)", magic, header[0], header[1], header[2], header[3]);
        return false;
    }
    type = header[8] | (header[9] << 8) | (header[10] << 16) | (header[11] << 24);
    unsigned int plen = header[12] | (header[13] << 8) | (header[14] << 16) | (header[15] << 24);
    if (plen > 64 * 1024)
    {
        return false;
    }
    payload.resize(plen);
    DWORD off = 0;
    while (off < plen)
    {
        DWORD rd = 0;
        if (!ReadFile(hPipe, payload.data() + off, plen - off, &rd, nullptr) || rd == 0)
        {
            return false;
        }
        off += rd;
    }
    return true;
}

static bool WriteFrame(HANDLE hPipe, unsigned int type, const std::vector<BYTE>& payload)
{
    std::vector<BYTE> frame;
    frame.reserve(16 + payload.size());
    AppendU32(frame, 0x5050494D);
    AppendU32(frame, 1);
    AppendU32(frame, type);
    AppendU32(frame, static_cast<unsigned int>(payload.size()));
    frame.insert(frame.end(), payload.begin(), payload.end());

    DWORD written = 0;
    BOOL ok = WriteFile(hPipe, frame.data(), static_cast<DWORD>(frame.size()), &written, nullptr);
    if (!ok || written != frame.size())
    {
        EngineLog(L"WriteFrame FAILED type=%u err=%lu written=%lu size=%u",
            type, GetLastError(), written, (unsigned)frame.size());
    }
    return ok && written == frame.size();
}

// 从 payload 读 u32 + utf8 字符串
static bool ReadU32(const std::vector<BYTE>& p, size_t& pos, unsigned int& out)
{
    if (p.size() - pos < 4) return false;
    out = p[pos] | (p[pos+1] << 8) | (p[pos+2] << 16) | (p[pos+3] << 24);
    pos += 4;
    return true;
}

static bool ReadUtf8(const std::vector<BYTE>& p, size_t& pos, std::wstring& out)
{
    unsigned int len = 0;
    if (!ReadU32(p, pos, len) || len > 65536 || p.size() - pos < len)
    {
        return false;
    }
    if (len > 0)
    {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(p.data() + pos), static_cast<int>(len), nullptr, 0);
        if (wlen <= 0)
        {
            pos += len;
            out.clear();
            return true;
        }
        out.resize(wlen);
        MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(p.data() + pos), static_cast<int>(len), &out[0], wlen);
        pos += len;
    }
    else
    {
        pos += len;
        out.clear();
    }
    return true;
}

// ---- 复合候选查询（复刻 DLL 侧 GetCandidateList 的生成逻辑） ----

static void QueryCandidates(CPinyinEngine& engine, const std::wstring& pinyin, std::vector<std::wstring>& out)
{
    if (pinyin.empty())
    {
        return;
    }

    CStringRange range;
    range.Set(const_cast<WCHAR*>(pinyin.c_str()), static_cast<DWORD_PTR>(pinyin.size()));
    CSampleImeArray<CCandidateListItem> list;

    // 1. 整句预测（Viterbi）置于最前
    engine.CollectSentence(&range, &list);

    // 2. 前缀词（增量搜索：拼音 + '*'）
    if (pinyin.size() > 0)
    {
        std::wstring wild = pinyin + L"*";
        CStringRange wrange;
        wrange.Set(const_cast<WCHAR*>(wild.c_str()), static_cast<DWORD_PTR>(wild.size()));
        engine.CollectWordForWildcard(&wrange, &list);
    }

    // 3. 全拼无结果 → 简拼回退 → 混合简拼/全拼回退
    if (list.Count() == 0)
    {
        engine.CollectWordByInitial(&range, &list);
        if (list.Count() == 0)
        {
            engine.CollectWordByMixed(&range, &list);
        }
    }

    // 3.5 仍无结果且输入够长 → 模糊音回退（z/zh、in/ing、an/ang 等常见错误发音）
    if (list.Count() == 0 && pinyin.size() >= 3)
    {
        engine.CollectWordFuzzy(&range, &list);
    }

    // 4. 长输入（≥3 音节）补按音节拼单字的组词候选（人名/生僻词兜底）
    if (pinyin.size() >= 6)
    {
        engine.CollectSingleCharsByKey(&range, &list);
    }

    // 转字符串并去重
    for (UINT i = 0; i < list.Count(); i++)
    {
        CStringRange* p = &list.GetAt(i)->_ItemString;
        std::wstring s(p->Get(), p->GetLength());
        bool dup = false;
        for (const auto& e : out)
        {
            if (e == s) { dup = true; break; }
        }
        if (!dup)
        {
            out.push_back(std::move(s));
        }
    }

    // 5. 繁体输出（tradition=1）：候选显示与上屏文本统一转繁体。
    //    词库恒为简体，转换只在输出层做；关闭时零开销。
    if (CConverter::IsTraditionEnabled())
    {
        for (auto& w : out)
        {
            w = CConverter::ToTraditional(w);
        }
    }
}

static void QuerySyllableChars(CPinyinEngine& engine, const std::wstring& syl, std::vector<std::wstring>& out)
{
    CSampleImeArray<CCandidateListItem> list;
    engine.CollectSyllableChars(syl, &list);
    for (UINT i = 0; i < list.Count(); i++)
    {
        CStringRange* p = &list.GetAt(i)->_ItemString;
        std::wstring w(p->Get(), p->GetLength());
        if (CConverter::IsTraditionEnabled())
        {
            w = CConverter::ToTraditional(w);
        }
        out.emplace_back(std::move(w));
    }
}

// ---- 连接处理：逐请求-响应 ----
//
//  线程模型：每连接一个线程，但共享同一个 CPinyinEngine 实例。
//  QueryCandidates 读 _entries / _syllableTable，而 BoostWord / AddUserWord
//  会写并重排这些容器 → 并发读写是未定义行为，是引擎崩溃的根源。
//  因此所有请求处理串行化：一次只服务一个请求（查询 <10ms，可接受）。

static CRITICAL_SECTION g_engineLock;
static bool g_engineLockInit = false;
static std::once_flag g_engineLockOnce;

// 引擎锁初始化（2026-08-13 修复竞态）：原实现用非原子 bool 标记，连接风暴下
// 多连接线程并发首次进入会同时 InitializeCriticalSection，损坏锁对象导致
// EnterCriticalSection 永久挂起（引擎 accept 后卡死、客户端超时断开 err=109）。
// std::call_once 保证仅单线程初始化一次。
static void EnsureEngineLock()
{
    std::call_once(g_engineLockOnce, []()
    {
        InitializeCriticalSection(&g_engineLock);
        g_engineLockInit = true;
    });
}

static void HandleConnection(HANDLE hPipe)
{
    static volatile LONG s_connId = 0;
    LONG myId = InterlockedIncrement(&s_connId);
    EngineLog(L"HandleConnection conn=%d BEGIN", myId);

    for (;;)
    {
        unsigned int type = 0;
        std::vector<BYTE> payload;
        EngineLog(L"HandleConnection conn=%d waiting for ReadFrame...", myId);
        if (!ReadFrame(hPipe, type, payload))
        {
            EngineLog(L"HandleConnection conn=%d ReadFrame failed, exiting", myId);
            break;   // 客户端断开/帧错误
        }

        EngineLog(L"HandleConnection conn=%d got request type=%u payload=%u", myId, type, (unsigned)payload.size());

        // 整个请求处理期间持有引擎锁
        std::vector<BYTE> resp;
        EnsureEngineLock();
        EnterCriticalSection(&g_engineLock);
        CPinyinEngine* engine = g_engine;   // 锁内取当前实例（双缓冲热重载切换也在此锁内）
        DWORD t0 = GetTickCount();
        switch (type)
        {
        case 1:   // RequestCandidates
        {
            size_t pos = 0;
            std::wstring pinyin;
            if (ReadUtf8(payload, pos, pinyin))
            {
                std::vector<std::wstring> words;
                QueryCandidates(*engine, pinyin, words);
                AppendU32(resp, static_cast<unsigned int>(words.size()));
                for (const auto& w : words)
                {
                    AppendUtf8(resp, w);
                }
                WriteFrame(hPipe, 2, resp);
            }
            break;
        }
        case 3:   // RequestSyllableChars
        {
            size_t pos = 0;
            std::wstring syl;
            if (ReadUtf8(payload, pos, syl))
            {
                std::vector<std::wstring> words;
                QuerySyllableChars(*engine, syl, words);
                AppendU32(resp, static_cast<unsigned int>(words.size()));
                for (const auto& w : words)
                {
                    AppendUtf8(resp, w);
                }
                WriteFrame(hPipe, 4, resp);
            }
            break;
        }
        case 5:   // BoostWord
        {
            size_t pos = 0;
            std::wstring pinyin, word;
            if (ReadUtf8(payload, pos, pinyin) && ReadUtf8(payload, pos, word))
            {
                // 繁体模式选词：DLL 回传的候选是繁体，入库前转回简体
                //（词库恒为简体，切换简繁模式无繁体残留）。
                if (CConverter::IsTraditionEnabled())
                {
                    word = CConverter::ToSimplified(word);
                }
                engine->BoostWord(pinyin.c_str(), pinyin.size(), word.c_str(), word.size());
            }
            // 发送空响应（DLL 的 RequestResponse 需要收到回复才解除阻塞）
            WriteFrame(hPipe, 5, resp);
            break;
        }
        case 6:   // AddUserWord
        {
            size_t pos = 0;
            std::wstring pinyin, word;
            if (ReadUtf8(payload, pos, pinyin) && ReadUtf8(payload, pos, word))
            {
                if (CConverter::IsTraditionEnabled())
                {
                    word = CConverter::ToSimplified(word);
                }
                engine->AddUserWord(pinyin.c_str(), pinyin.size(), word.c_str(), word.size());
            }
            // 发送空响应
            WriteFrame(hPipe, 6, resp);
            break;
        }
        case 7:   // SegmentToSyllables
        {
            size_t pos = 0;
            std::wstring key;
            if (ReadUtf8(payload, pos, key))
            {
                std::vector<std::wstring> syls = engine->SegmentToSyllables(key);
                AppendU32(resp, static_cast<unsigned int>(syls.size()));
                for (const auto& s : syls)
                {
                    AppendUtf8(resp, s);
                }
                WriteFrame(hPipe, 8, resp);
            }
            break;
        }
        case 9:   // RequestConvertedWildcard（从文本造词：按已选字反查同音/前缀词）
        {
            size_t pos = 0;
            std::wstring pattern;
            if (ReadUtf8(payload, pos, pattern))
            {
                std::wstring wild = pattern + L"*";
                CStringRange wrange;
                wrange.Set(const_cast<WCHAR*>(wild.c_str()), static_cast<DWORD_PTR>(wild.size()));
                CSampleImeArray<CCandidateListItem> list;
                engine->CollectWordFromConvertedStringForWildcard(&wrange, &list);
                AppendU32(resp, static_cast<unsigned int>(list.Count()));
                for (UINT i = 0; i < list.Count(); i++)
                {
                    CStringRange* p = &list.GetAt(i)->_ItemString;
                    std::wstring w(p->Get(), p->GetLength());
                    if (CConverter::IsTraditionEnabled())
                    {
                        w = CConverter::ToTraditional(w);
                    }
                    AppendUtf8(resp, w);
                }
                WriteFrame(hPipe, 10, resp);
            }
            break;
        }
        case 11:   // RequestReloadUserDict（设置面板导入词库：双缓冲后台热重载）
        {
            // 触发后台线程重建新实例，完成后原子切换。本请求立即返回，不阻塞击键
            //（原实现锁内同步 ReloadAll，86.9 万条词库需 7.5s，期间打字全卡）。
            TriggerReload();
            WriteFrame(hPipe, 12, resp);   // 空响应，客户端解除阻塞
            break;
        }
        case 13:   // DeleteUserWord（候选模式 Ctrl+Delete：删除选中的用户词）
        {
            size_t pos = 0;
            std::wstring word;
            if (ReadUtf8(payload, pos, word))
            {
                // 繁体模式：候选词是繁体，转回简体与词库匹配（词库恒为简体）
                if (CConverter::IsTraditionEnabled())
                {
                    word = CConverter::ToSimplified(word);
                }
                engine->DeleteUserWordByWord(word.c_str(), word.size());
            }
            WriteFrame(hPipe, 14, resp);   // 空响应
            break;
        }
        case 15:   // SetTradition（设置面板简/繁开关，payload 首字节 '0'/'1'）
        {
            bool on = (!payload.empty() && payload[0] == '1');
            CConverter::SetTraditionEnabled(on);
            EngineLog(L"SetTradition=%d", on ? 1 : 0);
            WriteFrame(hPipe, 16, resp);   // 空响应
            break;
        }
        case 16:   // DemoteWord（候选模式 Ctrl+PageDown / 右键"降低排位"：词降权沉底）
        {
            size_t pos = 0;
            std::wstring word;
            if (ReadUtf8(payload, pos, word))
            {
                // 繁体模式：候选词是繁体，转回简体与词库匹配（词库恒为简体）
                if (CConverter::IsTraditionEnabled())
                {
                    word = CConverter::ToSimplified(word);
                }
                engine->DemoteWord(word.c_str(), word.size());
            }
            WriteFrame(hPipe, 17, resp);   // 空响应
            break;
        }
        default:
            break;
        }
        DWORD elapsed = GetTickCount() - t0;
        LeaveCriticalSection(&g_engineLock);
        EngineLog(L"HandleConnection conn=%d request type=%u done in %lums", myId, type, elapsed);
    }
    EngineLog(L"HandleConnection conn=%d END", myId);
}

// 每连接一个线程：避免一个客户端卡住拖累其他
static volatile LONG g_activeConnections = 0;   // 并发连接计数（风暴限流用）
static volatile LONG g_totalConnections = 0;    // 累计连接数（存活 tick 日志用）
static const LONG MAX_CONCURRENT_CONNECTIONS = 32;   // 并发连接上限（防线程爆炸）
static DWORD g_crashCode = 0;                   // 连接线程崩溃信息（SEH 过滤器记录）
static PVOID g_crashAddr = nullptr;

// SEH 过滤器：记录崩溃信息并吞掉异常（GetExceptionInformation 只能在过滤器表达式调用）
static int RecordCrash(PEXCEPTION_POINTERS p)
{
    if (p && p->ExceptionRecord)
    {
        g_crashCode = p->ExceptionRecord->ExceptionCode;
        g_crashAddr = p->ExceptionRecord->ExceptionAddress;
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

static void ConnectionThreadProc(HANDLE hPipe)
{
    // SEH 兜底：连接线程异常不得导致引擎进程崩溃（连接风暴/客户端异常数据场景）
    __try
    {
        HandleConnection(hPipe);
    }
    __except (RecordCrash(GetExceptionInformation()))
    {
        EngineLog(L"ConnectionThread crashed code=0x%08X addr=%p (swallowed)", g_crashCode, g_crashAddr);
    }
    InterlockedDecrement(&g_activeConnections);
    CloseHandle(hPipe);
}

// 双缓冲热重载（type 11）：后台线程用新实例加载最新词库，完成后原子切换。
// 重建期间查询/写操作照常走 g_engine（零阻塞）；切换瞬间抢锁微秒级。
static void TriggerReload()
{
    EnsureEngineLock();
    EnterCriticalSection(&g_engineLock);
    if (g_reloadEngine != nullptr)
    {
        LeaveCriticalSection(&g_engineLock);
        return;   // 已有重建进行中（幂等）
    }
    g_reloadEngine = new CPinyinEngine();
    // 每次重读 engine.conf：大字库模式开关（bigdict=0/1）切换后，热重载即按
    // 新配置换词库（默认 pinyin-plus.txt ↔ 大字库 pinyin-plus-big.txt）。
    std::wstring dictPath = ResolveDictPath();
    LeaveCriticalSection(&g_engineLock);

    try
    {
        std::thread th([dictPath]()
        {
            if (g_reloadEngine->Initialize(dictPath.c_str()))
            {
                // 重建成功：原子切换，旧实例在锁外释放（无查询再引用它）
                EnsureEngineLock();
                EnterCriticalSection(&g_engineLock);
                CPinyinEngine* old = g_engine;
                g_engine = g_reloadEngine;
                g_reloadEngine = nullptr;
                LeaveCriticalSection(&g_engineLock);
                delete old;
                EngineLog(L"ReloadAll swap done (dict=%s)", dictPath.c_str());
            }
            else
            {
                // 重建失败：保留旧实例继续服务
                EnsureEngineLock();
                EnterCriticalSection(&g_engineLock);
                delete g_reloadEngine;
                g_reloadEngine = nullptr;
                LeaveCriticalSection(&g_engineLock);
                EngineLog(L"ReloadAll Initialize FAILED, keep old");
            }
        });
        th.detach();
    }
    catch (...)
    {
        // 线程创建失败：清理重建实例，继续用旧实例
        EnsureEngineLock();
        EnterCriticalSection(&g_engineLock);
        delete g_reloadEngine;
        g_reloadEngine = nullptr;
        LeaveCriticalSection(&g_engineLock);
    }
}

// ---- 命名管道安全描述符（2026-08-18 修复"只能打英文"）----
// 根因：安装器 PrivilegesRequired=admin 会让 [Run] 段以提权方式启动 Server，
// Server 看门狗再提权拉起引擎 → 引擎创建的管道为 High 完整性级别；
// 普通应用（Medium 完整性）里的 TSF DLL 连不上（ERROR_ACCESS_DENIED）→ 无候选 → 只能打英文。
// 显式放宽 DACL（Everyone 全权）+ 把管道完整性标签降到 Low，即使引擎被提权启动，
// 普通进程也始终能连上管道。这是与安装器 runasoriginaluser 双保险的治本修复。
static SECURITY_ATTRIBUTES& GetPipeSecurity()
{
    static SECURITY_ATTRIBUTES sa = []() {
        SECURITY_ATTRIBUTES s = {};
        s.nLength = sizeof(s);
        s.bInheritHandle = FALSE;
        PSECURITY_DESCRIPTOR pSD = nullptr;
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(A;;GA;;;WD)S:(ML;;NW;;;LW)",
            SDDL_REVISION_1, &pSD, nullptr))
        {
            s.lpSecurityDescriptor = pSD;
        }
        return s;
    }();
    return sa;
}

void RunEngineServer(CPinyinEngine* engine)
{
    g_engine = engine;   // 初始当前实例（双缓冲热重载在此指针上切换）
    EnsureEngineLock();  // 单线程预初始化引擎锁（连接风暴下多线程首次并发初始化会损坏锁）
    EngineLog(L"Engine server started");
    DWORD lastTick = GetTickCount();

    for (;;)
    {
        // 存活 tick：确认引擎主循环活着（崩溃排查用）
        if (GetTickCount() - lastTick >= 10000)
        {
            EngineLog(L"Engine alive conn_total=%lu active=%lu", g_totalConnections, g_activeConnections);
            lastTick = GetTickCount();
        }

        HANDLE hPipe = CreateNamedPipeW(
            L"\\\\.\\pipe\\PinyinPlus.Engine",
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            65536, 65536,
            0, &GetPipeSecurity());

        if (hPipe == INVALID_HANDLE_VALUE)
        {
            EngineLog(L"CreateNamedPipe FAILED err=%lu", GetLastError());
            Sleep(1000);
            continue;
        }

        BOOL ok = ConnectNamedPipe(hPipe, nullptr);
        if (!ok && GetLastError() != ERROR_PIPE_CONNECTED)
        {
            EngineLog(L"ConnectNamedPipe FAILED err=%lu (closing client handle)", GetLastError());
            CloseHandle(hPipe);
            continue;
        }

        // 连接风暴限流：DLL 多进程保活疯狂重连时会堆积线程（线程爆炸 → 进程崩溃）。
        // 超过上限直接拒绝，不创建线程。
        if (InterlockedIncrement(&g_activeConnections) > MAX_CONCURRENT_CONNECTIONS)
        {
            InterlockedDecrement(&g_activeConnections);
            EngineLog(L"Connection throttled (active=%lu)", g_activeConnections);
            CloseHandle(hPipe);
            continue;
        }
        InterlockedIncrement(&g_totalConnections);

        // 每连接独立线程
        try
        {
            std::thread th(ConnectionThreadProc, hPipe);
            th.detach();
        }
        catch (const std::exception& e)
        {
            EngineLog(L"ConnectionThread CREATE FAILED: %S", e.what());
            CloseHandle(hPipe);
        }
        catch (...)
        {
            EngineLog(L"ConnectionThread CREATE FAILED (unknown)");
            CloseHandle(hPipe);
        }
    }
}
