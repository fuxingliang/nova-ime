//+---------------------------------------------------------------------------
//
//  EngineClient.h
//
//  TSF DLL → 独立引擎进程(PinyinPlus.Engine.exe)的命名管道客户端。
//  请求-响应同步调用，带超时；引擎崩溃/卡死时 DLL 自动重启引擎进程并重连。
//
//  核心保证：任何调用都不会无限阻塞 TSF 线程（超时即放弃），
//  宿主应用输入永不被引擎问题拖住。
//
//----------------------------------------------------------------------------

#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <atomic>

class CEngineClient
{
public:
    // DLL 生命周期
    static void Initialize();
    static void Uninitialize();

    // 复合候选查询（整句 + 前缀 + 简拼/混合回退 + 组词），引擎侧复刻 GetCandidateList
    static bool QueryCandidates(_In_ const std::wstring &pinyin, _Inout_ std::vector<std::wstring> &out);

    // 音节全部单字（造词分段选字）
    static bool QuerySyllableChars(_In_ const std::wstring &syl, _Inout_ std::vector<std::wstring> &out);

    // 连续拼音串切分为音节（造词触发判断）
    static bool SegmentToSyllables(_In_ const std::wstring &key, _Inout_ std::vector<std::wstring> &out);

    // 从文本造词反查：按已选字匹配同音/前缀词（引擎侧 CollectWordFromConvertedStringForWildcard）
    static bool QueryConvertedWildcard(_In_ const std::wstring &pattern, _Inout_ std::vector<std::wstring> &out);

    // 用户选词：提升 (pinyin, word) 词频并持久化（自学习）
    static bool BoostWord(_In_ const std::wstring &pinyin, _In_ const std::wstring &word);

    // 用户造词入库（词库无则新增，含简拼）
    static bool AddUserWord(_In_ const std::wstring &pinyin, _In_ const std::wstring &word);

    // 删除用户词（候选模式 Ctrl+Delete）：按 word 删除，词库自带词忽略
    static bool DeleteUserWord(_In_ const std::wstring &word);

    // 降权（候选模式 Ctrl+PageDown / 右键"降低排位"）：把词加入降权黑名单，
    // 此后候选排序强制沉底。引擎侧持久化到 downweight.txt。
    static bool DemoteWord(_In_ const std::wstring &word);

private:
    static HANDLE EnsureConnected();   // 零阻塞：只读当前管道句柄，无效立即返回
    static void StartEngineProcess();
    static void ResetPipe();
    static bool RequestResponse(unsigned int reqType, _In_ const std::vector<BYTE> &reqPayload,
        _Out_ unsigned int &respType, _Inout_ std::vector<BYTE> &respPayload, DWORD timeoutMs);

    // 后台保活线程：负责拉起引擎 + 重连管道，TSF 线程永不在此等待。
    // 惰性启动（首次请求时创建），避免在 DllMain loader lock 中创建线程。
    static void EnsureKeepAliveStarted();
    static DWORD WINAPI KeepAliveThreadProc(LPVOID param);

    static void AppendU32(_Inout_ std::vector<BYTE> &v, unsigned int value);
    static void AppendUtf8(_Inout_ std::vector<BYTE> &v, const std::wstring &s);
    static bool ReadU32(_In_ const std::vector<BYTE> &p, _Inout_ size_t &pos, _Out_ unsigned int &out);
    static bool ReadUtf8(_In_ const std::vector<BYTE> &p, _Inout_ size_t &pos, _Inout_ std::wstring &out);

    static HANDLE _hPipe;
    static CRITICAL_SECTION _cs;          // 串行化请求（管道为单连接请求-响应）
    static std::atomic<bool> _running;
    static DWORD _lastLaunchTick;         // 上次拉起引擎的时间戳（防崩溃循环，最小间隔重启）
    static DWORD _connectedTick;          // 管道最近一次连接成功的时间（引擎冷启动窗口检测）
    static DWORD _lastSuccessTick;        // 最近一次请求成功的时间（引擎就绪证明，冷窗口门控）
    static std::atomic<bool> _keepAliveStarted;   // 保活线程只启动一次
    static HANDLE _keepAliveThread;
};
