//+---------------------------------------------------------------------------
//
//  PathUtil.h —— 引擎进程运行时路径（header-only）
//
//  安装包化约定（与 DLL 侧 Globals.h 的 g_installDir/g_dataDir 一致）：
//    安装目录 = 引擎 exe 所在目录（只读：词库 pinyin-plus*.txt / OpenCC 表 / server\）
//    数据目录 = %AppData%\NovaInput（可写：engine.conf / userdict.txt / 日志）
//
//  引擎是独立进程，用 GetModuleFileNameW(nullptr) 定位自身；
//  数据目录每用户（%APPDATA%），升级/卸载天然保留用户词库与配置。
//  header-only + 局部静态缓存：首次调用后不再重复查询。
//
//----------------------------------------------------------------------------

#pragma once

#include <windows.h>
#include <string>

namespace EnginePaths {

inline std::wstring InstallDir()
{
    static std::wstring s_dir = [] {
        WCHAR exe[MAX_PATH] = { 0 };
        if (GetModuleFileNameW(nullptr, exe, MAX_PATH) > 0)
        {
            WCHAR* slash = wcsrchr(exe, L'\\');
            if (slash)
            {
                *slash = L'\0';
            }
            return std::wstring(exe);
        }
        return std::wstring();
    }();
    return s_dir;
}

inline std::wstring DataDir()
{
    static std::wstring s_dir = [] {
        WCHAR env[MAX_PATH] = { 0 };
        DWORD len = GetEnvironmentVariableW(L"APPDATA", env, MAX_PATH);
        std::wstring dir;
        if (len > 0 && len < MAX_PATH)
        {
            dir = env;
            dir += L"\\NovaInput";
            // 首次运行创建数据目录（每用户可写，无需提权）
            CreateDirectoryW(dir.c_str(), nullptr);
        }
        return dir;
    }();
    return s_dir;
}

inline std::wstring InstallFile(const WCHAR* fileName)
{
    return InstallDir() + L"\\" + fileName;
}

inline std::wstring DataFile(const WCHAR* fileName)
{
    return DataDir() + L"\\" + fileName;
}

} // namespace EnginePaths
