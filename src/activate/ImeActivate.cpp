// ============================================================
//  ImeActivate - Nova 输入法安装后激活工具
//
//  作用（安装器 [Run] 段静默运行，实现"装完即用"）：
//    1. EnableLanguageProfile：TSF 激活（进入 Win+Space 切换列表）
//    2. SetDefaultIme：把 Nova 设为默认输入法（语言栏第一位）
//  实现：TF_CreateInputProcessorProfiles（msctf.dll 导出函数）
//        + SortOrder\AssemblyItem 注册表重排。
//  无运行时依赖（纯 Win32 + COM），几十 KB 单文件 exe。
// ============================================================
#include <windows.h>
#include <msctf.h>
#include <tchar.h>
#include <stdio.h>

// ---- Nova 输入法注册标识（与 Globals.cpp / Define.h 一致）----
// {D2291A80-84D8-4641-9AB2-BDD1472C846B}
static const CLSID CLSID_NovaIme = {
    0xd2291a80, 0x84d8, 0x4641, {0x9a, 0xb2, 0xbd, 0xd1, 0x47, 0x2c, 0x84, 0x6b}
};
// {83955C0E-2C09-47A5-BCF3-F2B98E11EE8B}
static const GUID GUID_NovaProfile = {
    0x83955c0e, 0x2c09, 0x47a5, {0xbc, 0xf3, 0xf2, 0xb9, 0x8e, 0x11, 0xee, 0x8b}
};

static const wchar_t kCatKey[] =
    L"Software\\Microsoft\\CTF\\SortOrder\\AssemblyItem\\0x00000804\\"
    L"{34745C63-B2F0-4784-8B67-5E12C8701A31}";   // GUID_TFCAT_TIP_KEYBOARD
static const wchar_t kNovaClsid[]  = L"{D2291A80-84D8-4641-9AB2-BDD1472C846B}";
static const wchar_t kNovaProfile[] = L"{83955C0E-2C09-47A5-BCF3-F2B98E11EE8B}";

typedef HRESULT(WINAPI *PFN_TF_CreateInputProcessorProfiles)(ITfInputProcessorProfiles **ppIPP);

//---------------------------------------------------------------------
// 把 Nova 设为默认输入法：重排 AssemblyItem，Nova 放 00000000（第一位）。
// 若 Nova 已在第一位则不动。
//---------------------------------------------------------------------
static void SetDefaultIme()
{
    HKEY hk = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kCatKey, 0,
        KEY_READ | KEY_WRITE, &hk) != ERROR_SUCCESS)
        return;

    struct Entry { wchar_t clsid[64]; wchar_t profile[64]; };
    Entry entries[16] = {};
    int count = 0;

    // 1. 按序枚举现有输入法（00000000 开始）
    for (DWORD i = 0; i < 16; i++)
    {
        wchar_t name[16] = {};
        DWORD nlen = (DWORD)ARRAYSIZE(name);
        if (RegEnumKeyExW(hk, i, name, &nlen, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;
        wchar_t sub[300] = {};
        swprintf_s(sub, L"%s\\%s", kCatKey, name);
        HKEY hsub = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, sub, 0, KEY_READ, &hsub) == ERROR_SUCCESS)
        {
            wchar_t buf[64] = {};
            DWORD size = (DWORD)sizeof(buf);
            if (RegQueryValueExW(hsub, L"CLSID", nullptr, nullptr, (BYTE *)buf, &size) == ERROR_SUCCESS)
                wcscpy_s(entries[count].clsid, buf);
            size = (DWORD)sizeof(buf);
            if (RegQueryValueExW(hsub, L"Profile", nullptr, nullptr, (BYTE *)buf, &size) == ERROR_SUCCESS)
                wcscpy_s(entries[count].profile, buf);
            RegCloseKey(hsub);
            count++;
        }
    }

    // 若 00000000 已是 Nova，无需重排
    if (count > 0 && _wcsicmp(entries[0].clsid, kNovaClsid) == 0)
    {
        RegCloseKey(hk);
        return;
    }

    // 2. 删除全部子键后重建：Nova 在 00000000，其余按原顺序后移
    for (DWORD i = 0; i < 16; i++)
    {
        wchar_t name[16] = {};
        DWORD nlen = (DWORD)ARRAYSIZE(name);
        if (RegEnumKeyExW(hk, 0, name, &nlen, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;
        RegDeleteKeyW(hk, name);
    }
    RegCloseKey(hk);

    int slot = 0;
    auto writeEntry = [&](const wchar_t *clsid, const wchar_t *profile) {
        wchar_t sub[64] = {};
        swprintf_s(sub, L"%s\\%08d", kCatKey, slot);
        HKEY hnew = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, sub, 0, nullptr, 0,
            KEY_WRITE, nullptr, &hnew, nullptr) == ERROR_SUCCESS)
        {
            if (clsid[0])
                RegSetValueExW(hnew, L"CLSID", 0, REG_SZ,
                    (const BYTE *)clsid, (DWORD)((wcslen(clsid) + 1) * sizeof(wchar_t)));
            if (profile[0])
                RegSetValueExW(hnew, L"Profile", 0, REG_SZ,
                    (const BYTE *)profile, (DWORD)((wcslen(profile) + 1) * sizeof(wchar_t)));
            DWORD kb = 0;
            RegSetValueExW(hnew, L"KeyboardLayout", 0, REG_DWORD, (const BYTE *)&kb, sizeof(kb));
            RegCloseKey(hnew);
        }
        slot++;
    };

    writeEntry(kNovaClsid, kNovaProfile);   // 00000000 = Nova（默认）
    for (int i = 0; i < count; i++)
    {
        if (_wcsicmp(entries[i].clsid, kNovaClsid) == 0)
            continue;                       // 跳过 Nova 旧条目
        writeEntry(entries[i].clsid, entries[i].profile);
    }
}

//---------------------------------------------------------------------
// TSF 激活：EnableLanguageProfile（进入语言栏 / Win+Space 列表）
//---------------------------------------------------------------------
static int ActivateIme()
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr))
        return 1;

    int rc = 1;
    HMODULE hMsctf = LoadLibraryW(L"msctf.dll");
    if (hMsctf)
    {
        auto pfn = (PFN_TF_CreateInputProcessorProfiles)GetProcAddress(
            hMsctf, "TF_CreateInputProcessorProfiles");
        if (pfn)
        {
            ITfInputProcessorProfiles *pProfiles = nullptr;
            hr = pfn(&pProfiles);
            if (SUCCEEDED(hr) && pProfiles)
            {
                hr = pProfiles->EnableLanguageProfile(CLSID_NovaIme,
                    MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED),
                    GUID_NovaProfile, TRUE);
                rc = SUCCEEDED(hr) ? 0 : 2;
                pProfiles->Release();
            }
        }
        FreeLibrary(hMsctf);
    }

    CoUninitialize();
    return rc;
}

int wmain()
{
    int rc = ActivateIme();
    SetDefaultIme();
    return rc;
}
