//+---------------------------------------------------------------------------
//
//  PinyinIpc.h
//
//  Pinyin-Plus 服务进程 IPC 客户端（命名管道，双向）
//  帧格式与 C# 端 PipeProtocol 保持一致：
//    [uint32 magic 0x5050494D][uint32 version=1][uint32 type][uint32 payloadLen][payload]
//  服务端管道：\\.\pipe\PinyinPlus.Service
//
//  方向说明：
//    DLL → 服务进程：Show / Hide / SetSelection / SetPosition（写）
//    服务进程 → DLL：SelectCandidate（点击选字）/ DeleteUserWord（右键删词）/
//                    InsertText（符号面板上屏）（读线程接收，回调通知 CSampleIME）
//
//----------------------------------------------------------------------------

#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <deque>
#include <functional>
#include <thread>
#include <atomic>

class CPinyinIpc
{
public:
    // DLL 生命周期：DllMain 中调用
    static void Initialize();
    static void Uninitialize();

    // 发送候选列表（UTF-16 转 UTF-8）
    static void SendShow(_In_ const std::vector<std::wstring>& candidates, int selectedIndex, _In_opt_ const WCHAR* pwszBuffer, int bufferLen);
    static void SendHide();
    static void SendSetSelection(int index);
    static void SendSetPosition(int x, int y);

    // 注册"鼠标点击候选"回调（index 为全局候选索引）。
    // 回调在 IPC 读线程触发，调用方负责投递到 TSF 线程。
    using CandidateSelectCallback = std::function<void(int index)>;
    static void SetCandidateSelectCallback(CandidateSelectCallback cb);

    // 注册"服务进程文本命令"回调（右键删除用户词 / 降权 / 符号面板插入文本，负载均为 UTF-8 字符串）。
    // 回调在 IPC 读线程触发，调用方负责投递到 TSF 线程。
    using TextCommandCallback = std::function<void(const std::wstring& text)>;
    static void SetDeleteUserWordCallback(TextCommandCallback cb);
    static void SetDemoteWordCallback(TextCommandCallback cb);
    static void SetInsertTextCallback(TextCommandCallback cb);

    // 调试日志（开发期）：写入 g:\pinyin-plus\bin\ime_debug.log
    static void DebugLog(_In_ const WCHAR* fmt, ...);

private:
    enum class MsgType : unsigned int
    {
        Show = 1,
        Hide = 2,
        SetSelection = 3,
        SetPosition = 4,
        SelectCandidate = 5,
        DeleteUserWord = 6,
        InsertText = 7,
        DemoteWord = 8,   // 右键"降低排位"：把目标词降权沉底
    };

    static HANDLE EnsureConnected();
    static void WriteAll(_In_ HANDLE hPipe, _In_ const void* pData, DWORD dwLen);

    // 发送异步化：TSF 线程只入队（毫秒级，绝不碰管道），发送线程负责连接与写。
    // 这样即使服务端卡死/管道阻塞，宿主应用输入也永远不会被 IPC 拖住。
    static void EnqueueFrame(MsgType type, _In_ const std::vector<BYTE>& payload);
    static void EnsureSendThreadStarted();
    static void SendThreadProc();

    // 读线程：接收服务进程命令（目前仅 SelectCandidate）
    static void ReadLoop();
    static void StartReadThreadIfNeeded();
    static void StopReadThread();

    static void AppendU32(_Inout_ std::vector<BYTE>& v, unsigned int value);
    static void AppendI32(_Inout_ std::vector<BYTE>& v, int value);
    static void AppendWStringUtf8(_Inout_ std::vector<BYTE>& v, _In_ const WCHAR* pwsz, int len);

    static HANDLE _hPipe;
    static CRITICAL_SECTION _cs;
    static std::thread _readThread;
    static std::atomic<bool> _running;       // 进程级开关
    static std::atomic<bool> _threadActive;  // 读线程是否在跑
    static CandidateSelectCallback _onCandidateSelect;
    static TextCommandCallback _onDeleteUserWord;
    static TextCommandCallback _onDemoteWord;
    static TextCommandCallback _onInsertText;

    // 发送队列
    static std::deque<std::vector<BYTE>> _sendQueue;
    static HANDLE _sendEvent;                 // 队列非空信号（auto-reset）
    static CRITICAL_SECTION _sendCs;          // 队列锁
    static std::thread _sendThread;
    static std::atomic<bool> _sendThreadStarted;  // 发送线程启动过一次（幂等）
};
