//+---------------------------------------------------------------------------
//
//  PinyinPlus.Engine —— 独立拼音引擎进程
//
//  词库 / 音节切分 / 候选生成 / 词频学习 / 用户造词全部在本进程内完成；
//  TSF DLL 通过命名管道查询本进程（请求-响应）。
//
//  收益：
//    - 改词库 / 引擎算法 → 重启本进程（秒级）→ DLL 自动重连 → 宿主应用无需重启
//    - 引擎崩溃 → DLL 检测断连自动拉起本进程 → 输入不中断
//
//----------------------------------------------------------------------------

#include <windows.h>
#include <cstdio>
#include <dbghelp.h>
#include <tlhelp32.h>
#include "PinyinEngine.h"
#include "EnginePipe.h"
#include "TraditionalConvert.h"
#include "PathUtil.h"

// 引擎调试日志 → 数据目录（%AppData%\NovaInput\engine_debug.log）
#define ENGINE_DEBUG_LOG EnginePaths::DataFile(L"engine_debug.log").c_str()

static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* pExc)
{
    FILE* f = nullptr;
    _wfopen_s(&f, ENGINE_DEBUG_LOG, L"a, ccs=UTF-8");
    if (f)
    {
        fwprintf(f, L"%lu\tCRASH ExceptionCode=0x%08X Address=0x%p\n",
            GetTickCount(), pExc->ExceptionRecord->ExceptionCode,
            pExc->ExceptionRecord->ExceptionAddress);
        fclose(f);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

// ---- Server 看门狗 ----
//  职责：守护候选窗服务 PinyinPlus.Server.exe。
//  背景：Server 内部的看门狗定时器只负责拉起引擎；Server 自己若崩溃/被杀，
//  没有任何进程守护它 → 候选窗永久消失。引擎是单实例进程（互斥体保证唯一），
//  天然适合作 Server 的守护者：Server 崩溃 → 引擎 1 秒内拉起 Server。
//  于是形成互相守护闭环：Server 看护引擎、引擎看护 Server、DLL 保活线程兜底。
static DWORD WINAPI ServerWatchdogProc(LPVOID)
{
    // Server 路径 = 安装目录 \server\PinyinPlus.Server.exe（与 DLL 同根，运行时定位）
    std::wstring serverExe = EnginePaths::InstallFile(L"server\\PinyinPlus.Server.exe");
    DWORD lastLaunchTick = 0;

    for (;;)
    {
        // 探测 Server 是否在运行（进程枚举）
        bool running = false;
        HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (hSnap != INVALID_HANDLE_VALUE)
        {
            PROCESSENTRY32W pe = { sizeof(pe) };
            if (Process32FirstW(hSnap, &pe))
            {
                do
                {
                    if (_wcsicmp(pe.szExeFile, L"PinyinPlus.Server.exe") == 0)
                    {
                        running = true;
                        break;
                    }
                } while (Process32NextW(hSnap, &pe));
            }
            CloseHandle(hSnap);
        }

        if (!running)
        {
            DWORD now = GetTickCount();
            // 防崩溃循环：上次拉起后至少间隔 2 秒才允许再拉
            if (lastLaunchTick == 0 || (now - lastLaunchTick) >= 2000)
            {
                lastLaunchTick = now;
                STARTUPINFOW si = { sizeof(si) };
                PROCESS_INFORMATION pi = {};
                si.dwFlags = STARTF_USESHOWWINDOW;
                si.wShowWindow = SW_SHOWNORMAL;
                BOOL ok = CreateProcessW(serverExe.c_str(), nullptr, nullptr, nullptr, FALSE,
                    0, nullptr, nullptr, &si, &pi);
                if (ok)
                {
                    CloseHandle(pi.hThread);
                    CloseHandle(pi.hProcess);
                    FILE* f = nullptr;
                    _wfopen_s(&f, ENGINE_DEBUG_LOG, L"a, ccs=UTF-8");
                    if (f)
                    {
                        fwprintf(f, L"%lu\tServerWatchdog: started Server pid=%lu\n", GetTickCount(), pi.dwProcessId);
                        fclose(f);
                    }
                }
                else
                {
                    FILE* f = nullptr;
                    _wfopen_s(&f, ENGINE_DEBUG_LOG, L"a, ccs=UTF-8");
                    if (f)
                    {
                        fwprintf(f, L"%lu\tServerWatchdog: CreateProcess FAILED err=%lu\n", GetTickCount(), GetLastError());
                        fclose(f);
                    }
                }
            }
        }

        Sleep(1000);   // 每秒检查一次，Server 崩溃 1 秒内拉起
    }
    return 0;
}

int wmain()
{
    SetUnhandledExceptionFilter(CrashHandler);
    // ---- 单实例保护 ----
    // bInitialOwner=TRUE：本进程创建/打开互斥体后立即持有所有权。
    //  - 已有实例存活：WaitForSingleObject 返回 WAIT_TIMEOUT → 本进程退出。
    //  - 已有实例崩溃：互斥体被标记 abandoned，WaitForSingleObject 返回
    //    WAIT_ABANDONED 并移交所有权 → 本进程接管（引擎崩溃自愈的关键路径）。
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"PinyinPlus.Engine.SingleInstance");
    if (hMutex && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        DWORD wait = WaitForSingleObject(hMutex, 0);
        if (wait == WAIT_TIMEOUT)
        {
            // 另一实例存活：退出，避免抢管道 / 抢 userdict
            return 0;
        }
        // WAIT_ABANDONED：旧实例崩溃，互斥体已移交本进程，继续运行
    }

    // 引擎实例堆分配：双缓冲热重载时（EnginePipe case 11）会在后台线程重建
    // 新实例并原子切换 g_engine 指针（导入词库不阻塞打字）。
    CPinyinEngine* engine = new CPinyinEngine();
    // 主词库路径由 engine.conf 的 bigdict 标志解析（大字库模式开关），
    // 默认 pinyin-plus.txt，bigdict=1 时加载 pinyin-plus-big.txt。
    std::wstring dictPath = ResolveDictPath();
    if (!engine->Initialize(dictPath.c_str()))
    {
        FILE* f = nullptr;
        _wfopen_s(&f, ENGINE_DEBUG_LOG, L"a, ccs=UTF-8");
        if (f)
        {
            fwprintf(f, L"%lu\tEngine Initialize FAILED\n", GetTickCount());
            fclose(f);
        }
        return 1;
    }

    // 简繁转换表加载（失败仅告警：引擎照常工作，只关闭繁体输出能力）。
    if (!CConverter::LoadTables())
    {
        FILE* f = nullptr;
        _wfopen_s(&f, ENGINE_DEBUG_LOG, L"a, ccs=UTF-8");
        if (f)
        {
            fwprintf(f, L"%lu\tTraditionalConvert: table load FAILED (tradition disabled)\n", GetTickCount());
            fclose(f);
        }
    }

    // 简繁输出初始开关（%AppData%\NovaInput\engine.conf tradition=0/1，设置面板持久化）。
    // 运行期切换由管道 type 15 消息即时生效（引擎无需重启）。
    {
        FILE* f = nullptr;
        if (_wfopen_s(&f, EnginePaths::DataFile(L"engine.conf").c_str(), L"r") == 0 && f != nullptr)
        {
            char line[64];
            while (fgets(line, sizeof(line), f) != nullptr)
            {
                if (_strnicmp(line, "tradition=", 10) == 0)
                {
                    CConverter::SetTraditionEnabled(line[10] == '1');
                    break;
                }
            }
            fclose(f);
        }
    }

    // 启动 Server 看门狗线程（detach）：Server 崩溃/被杀时 1 秒内拉起。
    // 引擎与 Server 互为守护，任何一方崩溃都由另一方秒级恢复。
    CreateThread(nullptr, 0, ServerWatchdogProc, nullptr, 0, nullptr);

    RunEngineServer(engine);
    return 0;
}
