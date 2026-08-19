// ============================================================
//  ImeProbe - Nova 输入法调试探针（开发用）
//  枚举当前语言的全部 TSF 输入法，打印 Nova 是否已启用。
//  用法：ImeProbe.exe
// ============================================================
#include <windows.h>
#include <msctf.h>
#include <stdio.h>

static const CLSID CLSID_NovaIme = {
    0xd2291a80, 0x84d8, 0x4641, {0x9a, 0xb2, 0xbd, 0xd1, 0x47, 0x2c, 0x84, 0x6b}
};
static const GUID GUID_NovaProfile = {
    0x83955c0e, 0x2c09, 0x47a5, {0xbc, 0xf3, 0xf2, 0xb9, 0x8e, 0x11, 0xee, 0x8b}
};

typedef HRESULT(WINAPI *PFN_TF_CreateInputProcessorProfiles)(ITfInputProcessorProfiles **ppIPP);

static wchar_t GuidStr(const GUID *g, wchar_t *buf, int len)
{
    wsprintfW(buf, L"{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
        g->Data1, g->Data2, g->Data3,
        g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3],
        g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
    return *buf;
}

int wmain()
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HMODULE hMsctf = LoadLibraryW(L"msctf.dll");
    if (!hMsctf) { wprintf(L"msctf.dll load failed\n"); return 1; }
    auto pfn = (PFN_TF_CreateInputProcessorProfiles)GetProcAddress(hMsctf, "TF_CreateInputProcessorProfiles");
    if (!pfn) { wprintf(L"TF_CreateInputProcessorProfiles not found\n"); return 1; }

    ITfInputProcessorProfiles *p = nullptr;
    pfn(&p);
    if (!p) { wprintf(L"profiles obj null\n"); return 1; }

    LANGID lang = 0;
    p->GetCurrentLanguage(&lang);
    wprintf(L"Current langid: 0x%04X\n", lang);

    IEnumTfLanguageProfiles *pEnum = nullptr;
    hr = p->EnumLanguageProfiles(lang, &pEnum);
    if (FAILED(hr)) { wprintf(L"EnumLanguageProfiles hr=0x%08X\n", hr); return 1; }

    TF_LANGUAGEPROFILE lp;
    ULONG fetched = 0;
    int idx = 0;
    wchar_t szClsid[64], szProfile[64], szDesc[256];
    while (pEnum->Next(1, &lp, &fetched) == S_OK && fetched > 0)
    {
        BOOL enabled = FALSE;
        p->IsEnabledLanguageProfile(lp.clsid, lp.langid, lp.guidProfile, &enabled);
        BSTR bstr = nullptr;
        p->GetLanguageProfileDescription(lp.clsid, lp.langid, lp.guidProfile, &bstr);
        if (bstr) { wcsncpy_s(szDesc, bstr, 255); SysFreeString(bstr); }
        else      { wcscpy_s(szDesc, L"(no desc)"); }
        GuidStr(&lp.clsid, szClsid, 64);
        GuidStr(&lp.guidProfile, szProfile, 64);
        int isNova = (lp.clsid == CLSID_NovaIme && lp.guidProfile == GUID_NovaProfile);
        wprintf(L"[%d] %s%s clsid=%s profile=%s\n", idx,
            szDesc, isNova ? L"  <<< NOVA" : L"", szClsid, szProfile);
        wprintf(L"     langid=0x%04X fActive=%d fEnabled=%d\n",
            lp.langid, lp.fActive, enabled ? 1 : 0);
        idx++;
        fetched = 0;
    }

    pEnum->Release();
    p->Release();
    FreeLibrary(hMsctf);
    CoUninitialize();
    return 0;
}
