//+---------------------------------------------------------------------------
//
//  PinyinIpc.cpp
//
//----------------------------------------------------------------------------

#include "Private.h"
#include "PinyinIpc.h"
#include "Globals.h"

#include <cstdio>
#include <cstdarg>
#include <mutex>

HANDLE CPinyinIpc::_hPipe = INVALID_HANDLE_VALUE;
CRITICAL_SECTION CPinyinIpc::_cs;
std::thread CPinyinIpc::_readThread;
std::atomic<bool> CPinyinIpc::_running{ false };
std::atomic<bool> CPinyinIpc::_threadActive{ false };
CPinyinIpc::CandidateSelectCallback CPinyinIpc::_onCandidateSelect;
CPinyinIpc::TextCommandCallback CPinyinIpc::_onDeleteUserWord;
CPinyinIpc::TextCommandCallback CPinyinIpc::_onDemoteWord;
CPinyinIpc::TextCommandCallback CPinyinIpc::_onInsertText;

std::deque<std::vector<BYTE>> CPinyinIpc::_sendQueue;
HANDLE CPinyinIpc::_sendEvent = nullptr;
CRITICAL_SECTION CPinyinIpc::_sendCs;
std::thread CPinyinIpc::_sendThread;
std::atomic<bool> CPinyinIpc::_sendThreadStarted{ false };

//+---------------------------------------------------------------------------
//
// DebugLog — 开发期调试日志
//
//----------------------------------------------------------------------------

void CPinyinIpc::DebugLog(_In_ const WCHAR* fmt, ...)
{
    WCHAR buf[512] = {0};
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, args);
    va_end(args);

    // 毫秒时间戳：用于与 Server 端日志（server_debug.log）精确对齐定位问题
    SYSTEMTIME st;
    GetLocalTime(&st);
    WCHAR ts[32] = {0};
    swprintf_s(ts, _countof(ts), L"[%02d:%02d:%02d.%03d] ",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    WCHAR full[600] = {0};
    swprintf_s(full, _countof(full), L"%s%s", ts, buf);

    // 纯 Win32 写日志（UTF-8 追加）。不用 CRT 的 fwprintf/_wfopen_s：
    // 它们在宿主（如 QQ 的 MSVCP140.dll 14.29）的 TSF 线程上会走 iostream/locale
    // 的 vtable 路径，宿主多线程下可触发空指针崩溃（QQ 崩溃点即 locale 风格 vtable 解引用）。
    // 纯 Win32 无 locale/facet/CRT 堆依赖，跨模块最安全。
    std::wstring logPath = GetDataPath(L"ime_debug.log");

    // 日志轮转：超过 64MB 改名为 .old 重新写，防止异常循环（如引擎反复拉起失败）
    // 把日志无限膨胀到数百 MB 放大磁盘压力（2026-08-15 安装事故曾膨胀到 230MB）。
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

    HANDLE h = CreateFileW(
        logPath.c_str(),
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        return;
    }

    char utf8[1200] = {0};
    int len = WideCharToMultiByte(CP_UTF8, 0, full, -1, utf8, (int)sizeof(utf8) - 1, nullptr, nullptr);
    if (len > 1)
    {
        DWORD written = 0;
        WriteFile(h, utf8, (DWORD)(len - 1), &written, nullptr);
        static const char g_crlf[] = "\r\n";
        WriteFile(h, g_crlf, 2, &written, nullptr);
    }
    CloseHandle(h);
}

//+---------------------------------------------------------------------------
//
// Initialize / Uninitialize
//
//----------------------------------------------------------------------------

void CPinyinIpc::Initialize()
{
    InitializeCriticalSectionAndSpinCount(&_cs, 1000);
    InitializeCriticalSectionAndSpinCount(&_sendCs, 1000);
    _running = true;
    // 发送线程不在 DllMain（loader lock）里创建，改为首次 EnqueueFrame 时惰性启动
    _sendEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);   // auto-reset
}

void CPinyinIpc::Uninitialize()
{
    _running = false;
    _onCandidateSelect = nullptr;
    _onDeleteUserWord = nullptr;
    _onDemoteWord = nullptr;
    _onInsertText = nullptr;
    if (_sendEvent)
    {
        SetEvent(_sendEvent);   // 唤醒发送线程退出
    }
    StopReadThread();
    if (_sendEvent)
    {
        CloseHandle(_sendEvent);
        _sendEvent = nullptr;
    }
    // 注意：不在 DLL_PROCESS_DETACH 阶段 DeleteCriticalSection，
    // 因为发送/读线程可能仍持锁阻塞在管道 I/O 上，删除会造成未定义行为。
    // 该 DLL 进程内单例，进程退出时由操作系统回收。
}

//+---------------------------------------------------------------------------
//
// 工具函数
//
//----------------------------------------------------------------------------

void CPinyinIpc::AppendU32(_Inout_ std::vector<BYTE>& v, unsigned int value)
{
    v.push_back(static_cast<BYTE>(value & 0xFF));
    v.push_back(static_cast<BYTE>((value >> 8) & 0xFF));
    v.push_back(static_cast<BYTE>((value >> 16) & 0xFF));
    v.push_back(static_cast<BYTE>((value >> 24) & 0xFF));
}

void CPinyinIpc::AppendI32(_Inout_ std::vector<BYTE>& v, int value)
{
    AppendU32(v, static_cast<unsigned int>(value));
}

void CPinyinIpc::AppendWStringUtf8(_Inout_ std::vector<BYTE>& v, _In_ const WCHAR* pwsz, int len)
{
    if (pwsz == nullptr || len <= 0)
    {
        AppendI32(v, 0);
        return;
    }
    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, pwsz, len, nullptr, 0, nullptr, nullptr);
    if (utf8Len <= 0)
    {
        AppendI32(v, 0);
        return;
    }
    std::vector<BYTE> utf8(utf8Len);
    WideCharToMultiByte(CP_UTF8, 0, pwsz, len, reinterpret_cast<LPSTR>(utf8.data()), utf8Len, nullptr, nullptr);
    AppendI32(v, utf8Len);
    v.insert(v.end(), utf8.begin(), utf8.end());
}

//+---------------------------------------------------------------------------
//
// EnsureConnected — 惰性连接服务端管道（失败返回 INVALID_HANDLE_VALUE）
//
//----------------------------------------------------------------------------

HANDLE CPinyinIpc::EnsureConnected()
{
    if (_hPipe != INVALID_HANDLE_VALUE)
    {
        return _hPipe;
    }

    HANDLE hPipe = CreateFileW(
        L"\\\\.\\pipe\\PinyinPlus.Service",
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED,   // 异步 I/O：避免 WriteFile 在服务端不读时无限阻塞
        nullptr);

    if (hPipe != INVALID_HANDLE_VALUE)
    {
        _hPipe = hPipe;
        StartReadThreadIfNeeded();   // 连上后启动读线程，接收服务进程命令
    }
    else
    {
        DebugLog(L"EnsureConnected FAILED err=%lu", GetLastError());
    }
    return _hPipe;
}

//+---------------------------------------------------------------------------
//
// ReadLoop — 读线程：接收服务进程命令（SelectCandidate）
//
//----------------------------------------------------------------------------

void CPinyinIpc::ReadLoop()
{
    std::vector<BYTE> pending;
    BYTE buf[4096];

    try
    {
        while (_running)
        {
            HANDLE hPipe = INVALID_HANDLE_VALUE;
            EnterCriticalSection(&_cs);
            hPipe = _hPipe;
            LeaveCriticalSection(&_cs);
            if (hPipe == INVALID_HANDLE_VALUE)
            {
                break;
            }

            DWORD read = 0;
            // 同步读取：管道断开（服务进程退出/被杀）时 ReadFile 立即返回，
            // 无等待中的异步 I/O，也不存在句柄被并发关闭的竞态。
            BOOL ok = ReadFile(hPipe, buf, sizeof(buf), &read, nullptr);
            if (!ok)
            {
                DWORD err = GetLastError();
                if (err != ERROR_BROKEN_PIPE && err != ERROR_PIPE_NOT_CONNECTED &&
                    err != ERROR_OPERATION_ABORTED)
                {
                    DebugLog(L"IPC ReadLoop ReadFile FAILED err=%lu", err);
                }
                break;
            }

            if (read == 0)
            {
                DebugLog(L"IPC ReadLoop exit err=%lu", GetLastError());
                break;
            }

            pending.insert(pending.end(), buf, buf + read);

            size_t pos = 0;
            while (pending.size() - pos >= 16)
            {
                const BYTE* h = pending.data() + pos;
                unsigned int magic  = h[0] | (h[1] << 8) | (h[2] << 16) | (h[3] << 24);
                unsigned int type   = h[8] | (h[9] << 8) | (h[10] << 16) | (h[11] << 24);
                unsigned int plen   = h[12] | (h[13] << 8) | (h[14] << 16) | (h[15] << 24);

                if (magic != 0x5050494D)   // "PPIM"
                {
                    // 失步：丢弃 1 字节重试
                    pending.erase(pending.begin());
                    continue;
                }

                size_t total = 16 + plen;
                if (pending.size() - pos < total)
                {
                    break;   // 帧未完整，等下一块
                }

                if (type == static_cast<unsigned int>(MsgType::SelectCandidate) && plen >= 4)
                {
                    int index = static_cast<int>(h[16] | (h[17] << 8) | (h[18] << 16) | (h[19] << 24));
                    DebugLog(L"IPC SelectCandidate index=%d", index);
                    auto cb = _onCandidateSelect;
                    if (cb)
                    {
                        cb(index);
                    }
                }
                else if ((type == static_cast<unsigned int>(MsgType::DeleteUserWord) ||
                          type == static_cast<unsigned int>(MsgType::InsertText) ||
                          type == static_cast<unsigned int>(MsgType::DemoteWord)) && plen >= 4)
                {
                    // 负载 = [int32 len][UTF-8 bytes]
                    int slen = static_cast<int>(h[16] | (h[17] << 8) | (h[18] << 16) | (h[19] << 24));
                    if (slen >= 0 && static_cast<unsigned int>(slen) <= plen - 4)
                    {
                        std::wstring wstr;
                        if (slen > 0)
                        {
                            std::string utf8(reinterpret_cast<const char*>(h + 20), static_cast<size_t>(slen));
                            int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
                            if (wlen > 0)
                            {
                                wstr.resize(wlen);
                                MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), &wstr[0], wlen);
                            }
                        }
                        if (type == static_cast<unsigned int>(MsgType::DeleteUserWord))
                        {
                            DebugLog(L"IPC DeleteUserWord text=%s", wstr.c_str());
                            auto cb = _onDeleteUserWord;
                            if (cb)
                            {
                                cb(wstr);
                            }
                        }
                        else if (type == static_cast<unsigned int>(MsgType::DemoteWord))
                        {
                            DebugLog(L"IPC DemoteWord text=%s", wstr.c_str());
                            auto cb = _onDemoteWord;
                            if (cb)
                            {
                                cb(wstr);
                            }
                        }
                        else
                        {
                            DebugLog(L"IPC InsertText text=%s", wstr.c_str());
                            auto cb = _onInsertText;
                            if (cb)
                            {
                                cb(wstr);
                            }
                        }
                    }
                }

                pos += total;
            }

            if (pos > 0)
            {
                pending.erase(pending.begin(), pending.begin() + pos);
            }
        }
    }
    catch (...)
    {
        // 读线程任何异常都不允许逃逸：std::terminate 会直接崩掉整个宿主进程
        DebugLog(L"IPC ReadLoop EXCEPTION");
    }

    // 管道断开/进程退出：本线程是句柄的唯一关闭方（StopReadThread 只 CancelIoEx
    // 唤醒，从不 CloseHandle），从根本上杜绝"句柄被并发关闭后再使用"的崩溃竞态。
    EnterCriticalSection(&_cs);
    if (_hPipe != INVALID_HANDLE_VALUE)
    {
        CloseHandle(_hPipe);
        _hPipe = INVALID_HANDLE_VALUE;
    }
    LeaveCriticalSection(&_cs);

    _threadActive = false;
    DebugLog(L"IPC ReadLoop stopped");
}

void CPinyinIpc::StartReadThreadIfNeeded()
{
    if (!_running || _threadActive || _hPipe == INVALID_HANDLE_VALUE)
    {
        return;
    }
    _threadActive = true;
    try
    {
        _readThread = std::thread([]() { ReadLoop(); });
        _readThread.detach();
    }
    catch (...)
    {
        _threadActive = false;
        DebugLog(L"IPC StartReadThread FAILED");
    }
}

void CPinyinIpc::StopReadThread()
{
    // 唤醒读线程：取消其阻塞的 ReadFile 让其立即退出。
    // 注意：这里绝不 CloseHandle——管道句柄的唯一关闭方是 ReadLoop 自身，
    // 避免"句柄被并发关闭后仍被 GetOverlappedResult/ReadFile 使用"的崩溃竞态。
    // 若发送线程正持锁阻塞在管道 I/O 上，死等会让进程卸载/切换输入法卡死；
    // 拿不到锁就放弃，句柄交由读线程退出时清理，或进程退出时操作系统回收。
    int attempts = 0;
    while (!TryEnterCriticalSection(&_cs))
    {
        if (++attempts > 100)   // ~1 秒上限
        {
            return;
        }
        Sleep(10);
    }
    HANDLE hPipe = _hPipe;
    if (hPipe != INVALID_HANDLE_VALUE)
    {
        CancelIoEx(hPipe, nullptr);   // 取消句柄上的阻塞读，让 ReadLoop 返回并自行清理
    }
    LeaveCriticalSection(&_cs);
    // 不 join：DLL_PROCESS_DETACH 阶段线程可能仍在收尾，detach 由操作系统回收
}

//+---------------------------------------------------------------------------
//
// WriteAll / SendFrame
//
//----------------------------------------------------------------------------

void CPinyinIpc::WriteAll(_In_ HANDLE hPipe, _In_ const void* pData, DWORD dwLen)
{
    const BYTE* p = static_cast<const BYTE*>(pData);
    DWORD offset = 0;
    while (offset < dwLen)
    {
        OVERLAPPED ov = {};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent)
        {
            DebugLog(L"WriteAll: CreateEventW FAILED err=%lu", GetLastError());
            // 仅复位标记，不关闭句柄（唯一关闭方为 ReadLoop）
            if (_hPipe == hPipe)
            {
                _hPipe = INVALID_HANDLE_VALUE;
            }
            return;
        }

        DWORD written = 0;
        BOOL ok = WriteFile(hPipe, p + offset, dwLen - offset, &written, &ov);
        if (!ok)
        {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING)
            {
                // 异步写入已发起，等待完成（最多 2 秒）
                DWORD waitResult = WaitForSingleObject(ov.hEvent, 2000);
                if (waitResult == WAIT_OBJECT_0)
                {
                    ok = GetOverlappedResult(hPipe, &ov, &written, FALSE);
                    if (!ok)
                    {
                        DebugLog(L"WriteAll: GetOverlappedResult FAILED err=%lu", GetLastError());
                    }
                }
                else if (waitResult == WAIT_TIMEOUT)
                {
                    DebugLog(L"WriteAll: WRITE TIMEOUT (2s), cancelling...");
                    CancelIo(hPipe);
                    ok = FALSE;
                    err = ERROR_TIMEOUT;
                }
                else
                {
                    DebugLog(L"WriteAll: WaitForSingleObject FAILED err=%lu", GetLastError());
                    ok = FALSE;
                }
            }
            else
            {
                DebugLog(L"WriteAll: WriteFile FAILED err=%lu", err);
            }
        }

        CloseHandle(ov.hEvent);

        if (!ok)
        {
            // 管道失效（服务端重启/断开）：仅复位句柄标记（调用方 SendThreadProc
            // 已持有 _cs，这里直接操作 _hPipe，不加锁）。
            // 注意：句柄的唯一关闭方是 ReadLoop（读线程退出时关闭自身快照），
            // 这里绝不 CloseHandle——两个线程关闭同一句柄会被 OS 复用误关新对象，
            // 这是宿主进程崩溃的根源之一。
            if (_hPipe == hPipe)
            {
                _hPipe = INVALID_HANDLE_VALUE;
            }
            return;
        }
        offset += written;
    }
}

//+---------------------------------------------------------------------------
//
// EnqueueFrame / SendThreadProc — 发送异步化
//
//  TSF 线程调用 Send* 只做"构造帧 → 入队"（毫秒级），管道连接与写全部
//  交给后台发送线程。即使服务端卡死或管道阻塞，宿主应用的输入线程
//  也永远不会被 IPC 拖住（避免 TSF 线程阻塞 → 应用挂起）。
//
//----------------------------------------------------------------------------

void CPinyinIpc::EnsureSendThreadStarted()
{
    if (_sendThreadStarted.exchange(true))
    {
        return;   // 已经尝试启动过（成功或失败），幂等
    }
    if (_sendEvent == nullptr)
    {
        _sendEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    if (_sendEvent == nullptr)
    {
        return;   // 系统资源异常：帧只在队列积压（输入不卡），下次 Uninitialize 清理
    }
    try
    {
        _sendThread = std::thread([]() { SendThreadProc(); });
        _sendThread.detach();
    }
    catch (...)
    {
        _sendThreadStarted = false;   // 允许下次再试
        DebugLog(L"IPC send thread START FAILED");
    }
}

void CPinyinIpc::EnqueueFrame(_In_ MsgType type, _In_ const std::vector<BYTE>& payload)
{
    if (!_running)
    {
        return;
    }

    std::vector<BYTE> frame;
    frame.reserve(16 + payload.size());
    AppendU32(frame, 0x5050494D);   // "PPIM"
    AppendU32(frame, 1);            // version
    AppendU32(frame, static_cast<unsigned int>(type));
    AppendU32(frame, static_cast<unsigned int>(payload.size()));
    frame.insert(frame.end(), payload.begin(), payload.end());

    EnterCriticalSection(&_sendCs);
    if (_sendQueue.size() >= 400)
    {
        _sendQueue.pop_front();     // 防积压：服务端长时间不读时丢弃最旧帧（候选窗滞后可接受）
    }
    _sendQueue.push_back(std::move(frame));
    LeaveCriticalSection(&_sendCs);

    // 惰性启动发送线程（首次调用；绝不在 TSF 线程上做任何管道 I/O）
    EnsureSendThreadStarted();
    if (_sendEvent)
    {
        SetEvent(_sendEvent);
    }
}

void CPinyinIpc::SendThreadProc()
{
    try
    {
        while (_running)
        {
            DWORD waitResult = WaitForSingleObject(_sendEvent, INFINITE);
            if (!_running)
            {
                break;
            }

            // 一次性取空当前队列
            std::deque<std::vector<BYTE>> batch;
            EnterCriticalSection(&_sendCs);
            batch.swap(_sendQueue);
            LeaveCriticalSection(&_sendCs);

            if (batch.empty())
            {
                continue;
            }

            // 管道连接与写入统一持 _cs（与读线程退出清理互斥）
            EnterCriticalSection(&_cs);
            HANDLE hPipe = EnsureConnected();
            if (hPipe != INVALID_HANDLE_VALUE)
            {
                for (auto& frame : batch)
                {
                    WriteAll(hPipe, frame.data(), static_cast<DWORD>(frame.size()));
                    if (_hPipe == INVALID_HANDLE_VALUE)
                    {
                        break;   // 写失败已复位句柄，丢弃剩余帧，下次循环重连
                    }
                }
            }
            LeaveCriticalSection(&_cs);
        }
    }
    catch (...)
    {
        DebugLog(L"IPC SendThreadProc EXCEPTION");
    }

    // 线程退出（含异常）：复位标志，允许下次 EnqueueFrame 重新拉起发送线程。
    // 若此处不复位，线程一旦崩溃，_sendThreadStarted 恒为 true，
    // 帧只入队不发送 → 候选窗永不显示（此前候选窗消失的根因之一）。
    _sendThreadStarted = false;
}

//+---------------------------------------------------------------------------
//
// 公开接口
//
//----------------------------------------------------------------------------

void CPinyinIpc::SendShow(_In_ const std::vector<std::wstring>& candidates, int selectedIndex, _In_opt_ const WCHAR* pwszBuffer, int bufferLen)
{
    if (candidates.empty())
    {
        return;
    }

    std::vector<BYTE> payload;
    AppendWStringUtf8(payload, pwszBuffer, bufferLen);      // 拼音缓冲
    AppendI32(payload, selectedIndex);                      // 选中索引
    AppendI32(payload, 0);                                  // 页起始
    AppendI32(payload, static_cast<int>(candidates.size())); // 候选数
    for (const auto& c : candidates)
    {
        AppendWStringUtf8(payload, c.c_str(), static_cast<int>(c.size()));
    }

    EnqueueFrame(MsgType::Show, payload);
}

void CPinyinIpc::SendHide()
{
    EnqueueFrame(MsgType::Hide, std::vector<BYTE>());
}

void CPinyinIpc::SendSetSelection(int index)
{
    std::vector<BYTE> payload;
    AppendI32(payload, index);
    EnqueueFrame(MsgType::SetSelection, payload);
}

void CPinyinIpc::SetCandidateSelectCallback(CandidateSelectCallback cb)
{
    _onCandidateSelect = std::move(cb);
}

void CPinyinIpc::SetDeleteUserWordCallback(TextCommandCallback cb)
{
    _onDeleteUserWord = std::move(cb);
}

void CPinyinIpc::SetDemoteWordCallback(TextCommandCallback cb)
{
    _onDemoteWord = std::move(cb);
}

void CPinyinIpc::SetInsertTextCallback(TextCommandCallback cb)
{
    _onInsertText = std::move(cb);
}

void CPinyinIpc::SendSetPosition(int x, int y)
{
    std::vector<BYTE> payload;
    AppendI32(payload, x);
    AppendI32(payload, y);
    EnqueueFrame(MsgType::SetPosition, payload);
}
