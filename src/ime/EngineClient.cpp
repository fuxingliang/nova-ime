//+---------------------------------------------------------------------------
//
//  EngineClient.cpp
//
//  引擎客户端实现：连接/拉起引擎、请求-响应（带超时）、断连自愈。
//
//  消息类型（与 EnginePipe 服务端一致）：
//    1 RequestCandidates    2 ResponseCandidates
//    3 RequestSyllableChars 4 ResponseSyllChars
//    5 BoostWord            6 AddUserWord
//    7 SegmentToSyllables   8 ResponseSyllables
//
//----------------------------------------------------------------------------

#include "EngineClient.h"
#include "Globals.h"
#include <cstdio>
#include <cwchar>

HANDLE CEngineClient::_hPipe = INVALID_HANDLE_VALUE;
CRITICAL_SECTION CEngineClient::_cs;
std::atomic<bool> CEngineClient::_running{ false };
DWORD CEngineClient::_lastLaunchTick = 0;
DWORD CEngineClient::_connectedTick = 0;
DWORD CEngineClient::_lastSuccessTick = 0;
std::atomic<bool> CEngineClient::_keepAliveStarted{ false };
HANDLE CEngineClient::_keepAliveThread = nullptr;

// 引擎崩溃后的最小重启间隔（毫秒）：防止崩溃循环导致疯狂拉起进程
static const DWORD kMinLaunchIntervalMs = 2000;

static void ClientLog(const WCHAR* fmt, ...)
{
    WCHAR buf[512] = {0};
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, args);
    va_end(args);

    // 纯 Win32 写日志（UTF-8 追加）。不用 CRT fwprintf：宿主进程（QQ 等）的
    // TSF 线程上 fwprintf 走 iostream/locale vtable 路径，可能触发宿主
    // MSVCP140.dll 的空指针崩溃。
    std::wstring logPath = GetDataPath(L"engine_client.log");
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
    int len = WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8, (int)sizeof(utf8) - 1, nullptr, nullptr);
    if (len > 1)
    {
        DWORD written = 0;
        WriteFile(h, utf8, (DWORD)(len - 1), &written, nullptr);
        static const char g_crlf[] = "\r\n";
        WriteFile(h, g_crlf, 2, &written, nullptr);
    }
    CloseHandle(h);
}

// ---- 帧编解码 ----

void CEngineClient::AppendU32(_Inout_ std::vector<BYTE>& v, unsigned int value)
{
    v.push_back(static_cast<BYTE>(value & 0xFF));
    v.push_back(static_cast<BYTE>((value >> 8) & 0xFF));
    v.push_back(static_cast<BYTE>((value >> 16) & 0xFF));
    v.push_back(static_cast<BYTE>((value >> 24) & 0xFF));
}

void CEngineClient::AppendUtf8(_Inout_ std::vector<BYTE>& v, const std::wstring& s)
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

bool CEngineClient::ReadU32(_In_ const std::vector<BYTE>& p, _Inout_ size_t& pos, _Out_ unsigned int& out)
{
    if (p.size() - pos < 4) return false;
    out = p[pos] | (p[pos+1] << 8) | (p[pos+2] << 16) | (p[pos+3] << 24);
    pos += 4;
    return true;
}

bool CEngineClient::ReadUtf8(_In_ const std::vector<BYTE>& p, _Inout_ size_t& pos, _Inout_ std::wstring& out)
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
        out.clear();
    }
    return true;
}

// ---- 进程管理 ----

// 安装器抑制标志（2026-08-18 修复安装"文件写保护"中止）：
// 安装器杀引擎后，保活线程若立刻拉起新引擎，新引擎会锁定词库文件，
// 导致安装器复制词库时报"文件写保护"而中止。安装器在 PrepareToInstall
// 创建 {userappdata}\NovaInput\installing.flag、在 DeinitializeSetup 清除。
// 用 mtime 做 10 分钟 TTL：即使安装器异常退出残留 flag，也不会永久抑制拉起。
static bool IsInstallInProgress()
{
    std::wstring flag = GetDataPath(L"installing.flag");
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(flag.c_str(), GetFileExInfoStandard, &fa))
    {
        return false;   // 无 flag → 正常拉起
    }
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER a, b;
    a.LowPart = fa.ftLastWriteTime.dwLowDateTime;
    a.HighPart = fa.ftLastWriteTime.dwHighDateTime;
    b.LowPart = now.dwLowDateTime;
    b.HighPart = now.dwHighDateTime;
    // FILETIME 单位 = 100ns；10 分钟 = 600 秒 = 6,000,000,000 个 100ns
    if (b.QuadPart - a.QuadPart < 600ULL * 10000000ULL)
    {
        return true;    // 10 分钟内 → 安装进行中，抑制拉起
    }
    DeleteFileW(flag.c_str());   // 陈旧 flag，清除后恢复拉起
    return false;
}

void CEngineClient::StartEngineProcess()
{
    if (IsInstallInProgress())
    {
        return;   // 安装期间不拉起引擎，避免锁定安装文件
    }
    // 引擎路径 = 安装目录（DLL 同目录）PinyinPlus.Engine.exe，运行时定位
    std::wstring exePath = GetInstallPath(L"PinyinPlus.Engine.exe");
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    BOOL ok = CreateProcessW(exePath.c_str(), nullptr, nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (ok)
    {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        ClientLog(L"StartEngineProcess OK pid=%lu", pi.dwProcessId);
    }
    else
    {
        ClientLog(L"StartEngineProcess FAILED err=%lu", GetLastError());
    }
}

void CEngineClient::ResetPipe()
{
    if (_hPipe != INVALID_HANDLE_VALUE)
    {
        CloseHandle(_hPipe);
        _hPipe = INVALID_HANDLE_VALUE;
        _connectedTick = 0;
    }
}

// 零阻塞连接：只读当前管道句柄。无效立即返回（引擎拉起/重连由后台保活线程负责）。
HANDLE CEngineClient::EnsureConnected()
{
    EnsureKeepAliveStarted();
    return _hPipe;
}

// ---- 后台保活线程 ----
//
//  职责：引擎未运行或管道断开时，负责拉起引擎并重连管道。
//  为什么需要它：TSF 击键在宿主应用 UI 线程上执行，若在击键路径上
//  Sleep(400)+拉起引擎+重连，宿主应用（Trae/记事本）会被阻塞几百毫秒
//  表现为"失去响应"（热更新引擎时尤为明显）。本线程把全部等待移出
//  TSF 线程，击键侧只读句柄，零阻塞。

void CEngineClient::EnsureKeepAliveStarted()
{
    if (_keepAliveStarted.exchange(true))
    {
        return;
    }
    _keepAliveThread = CreateThread(nullptr, 0, KeepAliveThreadProc, nullptr, 0, nullptr);
    if (!_keepAliveThread)
    {
        _keepAliveStarted = false;   // 允许下次再试
        ClientLog(L"KeepAliveThread CREATE FAILED err=%lu", GetLastError());
    }
}

DWORD WINAPI CEngineClient::KeepAliveThreadProc(LPVOID)
{
    ClientLog(L"KeepAlive thread started");

    while (_running)
    {
        bool needLaunch = false;
        DWORD now = GetTickCount();
        if (_hPipe == INVALID_HANDLE_VALUE)
        {
            needLaunch = (_lastLaunchTick == 0 || (now - _lastLaunchTick) >= kMinLaunchIntervalMs);
        }

        if (needLaunch)
        {
            StartEngineProcess();
            _lastLaunchTick = now;
        }

        // 尝试连接（无论刚拉起还是已运行，都试连一次）
        HANDLE hPipe = CreateFileW(
            L"\\\\.\\pipe\\PinyinPlus.Engine",
            GENERIC_READ | GENERIC_WRITE,
            0, nullptr, OPEN_EXISTING, 0, nullptr);

        if (hPipe != INVALID_HANDLE_VALUE)
        {
            EnterCriticalSection(&_cs);
            if (_hPipe == INVALID_HANDLE_VALUE)
            {
                _hPipe = hPipe;
                _connectedTick = GetTickCount();
                ClientLog(L"KeepAlive connected (tick=%lu)", GetTickCount());
            }
            else
            {
                CloseHandle(hPipe);   // 已被别的线程占用，丢弃重复连接
            }
            LeaveCriticalSection(&_cs);
        }
        else
        {
            // 连接失败记录错误码（ERROR_PIPE_BUSY / ERROR_FILE_NOT_FOUND 等），
            // 便于定位"引擎在跑却连不上管道"的问题（2026-08-18 排查用）。
            ClientLog(L"KeepAlive connect FAILED err=%lu tick=%lu", GetLastError(), GetTickCount());
        }

        // 未连接时 100ms 快轮询（尽快拉起/重连，缩短首键丢失窗口）；
        // 已连接后 300ms 慢轮询（仅探活，避免空转）
        Sleep(_hPipe == INVALID_HANDLE_VALUE ? 100 : 300);
    }

    ClientLog(L"KeepAlive thread exited");
    return 0;
}

// ---- 请求-响应（带超时，绝不阻塞 TSF 线程） ----

bool CEngineClient::RequestResponse(unsigned int reqType, _In_ const std::vector<BYTE>& reqPayload,
    _Out_ unsigned int& respType, _Inout_ std::vector<BYTE>& respPayload, DWORD timeoutMs)
{
    respType = 0;
    respPayload.clear();
    if (!_running)
    {
        return false;
    }

    DWORD t0 = GetTickCount();

    EnterCriticalSection(&_cs);
    // 快速路径：管道句柄无效时同步试连一次。引擎侧管道为 PIPE_UNLIMITED_INSTANCES，
    // CreateFile 非阻塞（引擎在跑=毫秒级成功；引擎不在=立即 ERROR_FILE_NOT_FOUND）。
    // 新宿主应用注入 DLL 后首键即连上（引擎由 Server 看门狗保活，几乎总在跑），
    // 首键不再丢失；引擎真不在时本处立即失败，由保活线程负责拉起，零等待。
    if (_hPipe == INVALID_HANDLE_VALUE)
    {
        HANDLE hFast = CreateFileW(
            L"\\\\.\\pipe\\PinyinPlus.Engine",
            GENERIC_READ | GENERIC_WRITE,
            0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (hFast != INVALID_HANDLE_VALUE)
        {
            _hPipe = hFast;
            _connectedTick = GetTickCount();
            ClientLog(L"FastPath connected (tick=%lu)", GetTickCount());
        }
    }
    HANDLE hPipe = EnsureConnected();
    if (hPipe == INVALID_HANDLE_VALUE)
    {
        LeaveCriticalSection(&_cs);
        return false;
    }

    // 冷启动快速失败窗口（2026-08-20 修复"首键反复出英文"死循环）：
    // 旧逻辑"重连后 3 秒内一律 30ms 超时"存在致命矛盾——响应轮询下限 ~31ms
    // （Sleep(1) 在 Windows 实际粒度 ~15ms），30ms 超时在引擎完全就绪时
    // 也必然超时 → ResetPipe → 保活 300ms 后重连 → _connectedTick 刷新 →
    // 窗口重置 → 持续打字时每个键都在窗口内必超时（候选窗不显示，拼音原样
    // 上屏表现为"英文"），只有停顿 3 秒以上才恢复（用户感知的"预热"）。
    // 现改为：仅当"重连后尚无一次成功请求"（_lastSuccessTick < _connectedTick）
    // 才启用快速失败；超时取 150ms = 高于轮询下限(~31ms)保证就绪引擎必成功，
    // 又低于常规 300ms 保持快速失败。首个成功请求即证明引擎就绪（引擎管道
    // 在词库加载完成后才创建），窗口永久解除直至下次重连。
    DWORD effectiveTimeout = timeoutMs;
    if (_connectedTick != 0 && _lastSuccessTick < _connectedTick
        && (GetTickCount() - _connectedTick) < 3000)
    {
        effectiveTimeout = 150;
    }

    // 组装请求帧
    std::vector<BYTE> frame;
    frame.reserve(16 + reqPayload.size());
    AppendU32(frame, 0x5050494D);
    AppendU32(frame, 1);
    AppendU32(frame, reqType);
    AppendU32(frame, static_cast<unsigned int>(reqPayload.size()));
    frame.insert(frame.end(), reqPayload.begin(), reqPayload.end());

    // 写请求
    DWORD written = 0;
    BOOL ok = WriteFile(hPipe, frame.data(), static_cast<DWORD>(frame.size()), &written, nullptr);
    if (!ok || written != frame.size())
    {
        ResetPipe();
        LeaveCriticalSection(&_cs);
        return false;
    }

    // 读响应：PeekNamedPipe 轮询 + 超时
    // 优化：先快速轮询（Sleep(0)）几轮，再慢速轮询（Sleep(1)），降低延迟
    DWORD deadline = GetTickCount() + effectiveTimeout;
    int fastPollCount = 0;
    for (;;)
    {
        DWORD avail = 0;
        if (!PeekNamedPipe(hPipe, nullptr, 0, nullptr, &avail, nullptr))
        {
            ResetPipe();   // 引擎断开/崩溃
            LeaveCriticalSection(&_cs);
            return false;
        }
        if (avail >= 16)
        {
            BYTE header[16];
            DWORD rd = 0;
            if (!ReadFile(hPipe, header, 16, &rd, nullptr) || rd != 16)
            {
                ResetPipe();
                LeaveCriticalSection(&_cs);
                return false;
            }
            unsigned int magic = header[0] | (header[1] << 8) | (header[2] << 16) | (header[3] << 24);
            respType = header[8] | (header[9] << 8) | (header[10] << 16) | (header[11] << 24);
            unsigned int plen = header[12] | (header[13] << 8) | (header[14] << 16) | (header[15] << 24);
            if (magic != 0x5050494D || plen > 512 * 1024)
            {
                ResetPipe();
                LeaveCriticalSection(&_cs);
                return false;
            }
            respPayload.resize(plen);
            DWORD off = 0;
            while (off < plen)
            {
                DWORD r2 = 0;
                if (!ReadFile(hPipe, respPayload.data() + off, plen - off, &r2, nullptr) || r2 == 0)
                {
                    ResetPipe();
                    LeaveCriticalSection(&_cs);
                    return false;
                }
                off += r2;
            }
            break;
        }
        if (GetTickCount() >= deadline)
        {
            DWORD elapsed = GetTickCount() - t0;
            ClientLog(L"RequestResponse TIMEOUT type=%u elapsed=%lums", reqType, elapsed);
            ResetPipe();   // 超时：放弃本次请求，避免阻塞输入
            LeaveCriticalSection(&_cs);
            return false;
        }
        // 前 10 轮快速轮询（Sleep(0) 让出 CPU），之后慢速轮询（Sleep(1)）
        if (fastPollCount < 10)
        {
            Sleep(0);
            fastPollCount++;
        }
        else
        {
            Sleep(1);
        }
    }

    DWORD elapsed = GetTickCount() - t0;
    if (elapsed > 20)
    {
        ClientLog(L"RequestResponse SLOW type=%u elapsed=%lums", reqType, elapsed);
    }

    // 成功 = 引擎就绪证明：解除冷窗口门控，直至下次重连
    _lastSuccessTick = GetTickCount();
    LeaveCriticalSection(&_cs);
    return true;
}

// ---- 公开接口 ----

void CEngineClient::Initialize()
{
    InitializeCriticalSectionAndSpinCount(&_cs, 1000);
    _running = true;
}

void CEngineClient::Uninitialize()
{
    _running = false;
    // 加锁复位句柄：与保活线程的写入互斥，避免关闭正在使用的句柄
    EnterCriticalSection(&_cs);
    ResetPipe();
    LeaveCriticalSection(&_cs);
    // 不在 DllMain 删除 CriticalSection：线程可能仍在使用（进程退出时 OS 回收）
}

bool CEngineClient::QueryCandidates(_In_ const std::wstring& pinyin, _Inout_ std::vector<std::wstring>& out)
{
    std::vector<BYTE> req;
    AppendUtf8(req, pinyin);
    unsigned int respType = 0;
    std::vector<BYTE> resp;
    if (!RequestResponse(1, req, respType, resp, 300))
    {
        return false;
    }
    if (respType != 2)
    {
        return false;
    }
    size_t pos = 0;
    unsigned int count = 0;
    if (!ReadU32(resp, pos, count))
    {
        return false;
    }
    out.clear();
    for (unsigned int i = 0; i < count; i++)
    {
        std::wstring w;
        if (!ReadUtf8(resp, pos, w))
        {
            return false;
        }
        out.push_back(std::move(w));
    }
    return true;
}

bool CEngineClient::QuerySyllableChars(_In_ const std::wstring& syl, _Inout_ std::vector<std::wstring>& out)
{
    std::vector<BYTE> req;
    AppendUtf8(req, syl);
    unsigned int respType = 0;
    std::vector<BYTE> resp;
    if (!RequestResponse(3, req, respType, resp, 300))
    {
        return false;
    }
    if (respType != 4)
    {
        return false;
    }
    size_t pos = 0;
    unsigned int count = 0;
    if (!ReadU32(resp, pos, count))
    {
        return false;
    }
    out.clear();
    for (unsigned int i = 0; i < count; i++)
    {
        std::wstring w;
        if (!ReadUtf8(resp, pos, w))
        {
            return false;
        }
        out.push_back(std::move(w));
    }
    return true;
}

bool CEngineClient::SegmentToSyllables(_In_ const std::wstring& key, _Inout_ std::vector<std::wstring>& out)
{
    std::vector<BYTE> req;
    AppendUtf8(req, key);
    unsigned int respType = 0;
    std::vector<BYTE> resp;
    if (!RequestResponse(7, req, respType, resp, 300))
    {
        return false;
    }
    if (respType != 8)
    {
        return false;
    }
    size_t pos = 0;
    unsigned int count = 0;
    if (!ReadU32(resp, pos, count))
    {
        return false;
    }
    out.clear();
    for (unsigned int i = 0; i < count; i++)
    {
        std::wstring w;
        if (!ReadUtf8(resp, pos, w))
        {
            return false;
        }
        out.push_back(std::move(w));
    }
    return true;
}

bool CEngineClient::BoostWord(_In_ const std::wstring& pinyin, _In_ const std::wstring& word)
{
    std::vector<BYTE> req;
    AppendUtf8(req, pinyin);
    AppendUtf8(req, word);
    unsigned int respType = 0;
    std::vector<BYTE> resp;
    return RequestResponse(5, req, respType, resp, 300);
}

bool CEngineClient::AddUserWord(_In_ const std::wstring& pinyin, _In_ const std::wstring& word)
{
    std::vector<BYTE> req;
    AppendUtf8(req, pinyin);
    AppendUtf8(req, word);
    unsigned int respType = 0;
    std::vector<BYTE> resp;
    return RequestResponse(6, req, respType, resp, 800);
}

bool CEngineClient::DeleteUserWord(_In_ const std::wstring& word)
{
    std::vector<BYTE> req;
    AppendUtf8(req, word);
    unsigned int respType = 0;
    std::vector<BYTE> resp;
    if (!RequestResponse(13, req, respType, resp, 800))
    {
        return false;
    }
    return respType == 14;
}

bool CEngineClient::DemoteWord(_In_ const std::wstring& word)
{
    std::vector<BYTE> req;
    AppendUtf8(req, word);
    unsigned int respType = 0;
    std::vector<BYTE> resp;
    if (!RequestResponse(16, req, respType, resp, 800))
    {
        return false;
    }
    return respType == 17;
}

bool CEngineClient::QueryConvertedWildcard(_In_ const std::wstring& pattern, _Inout_ std::vector<std::wstring>& out)
{
    std::vector<BYTE> req;
    AppendUtf8(req, pattern);
    unsigned int respType = 0;
    std::vector<BYTE> resp;
    if (!RequestResponse(9, req, respType, resp, 300))
    {
        return false;
    }
    if (respType != 10)
    {
        return false;
    }
    size_t pos = 0;
    unsigned int count = 0;
    if (!ReadU32(resp, pos, count))
    {
        return false;
    }
    out.clear();
    for (unsigned int i = 0; i < count; i++)
    {
        std::wstring w;
        if (!ReadUtf8(resp, pos, w))
        {
            return false;
        }
        out.push_back(std::move(w));
    }
    return true;
}
