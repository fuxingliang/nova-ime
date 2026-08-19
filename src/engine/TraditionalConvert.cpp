//+---------------------------------------------------------------------------
//
//  TraditionalConvert.cpp —— 简繁转换模块实现
//
//----------------------------------------------------------------------------

#include "TraditionalConvert.h"
#include "PathUtil.h"

#include <windows.h>
#include <cstdio>
#include <vector>

std::unordered_map<std::wstring, std::wstring> CConverter::s_s2tPhrase;
std::unordered_map<std::wstring, std::wstring> CConverter::s_s2tChar;
std::unordered_map<std::wstring, std::wstring> CConverter::s_t2sPhrase;
std::unordered_map<std::wstring, std::wstring> CConverter::s_t2sChar;
bool CConverter::s_loaded = false;
bool CConverter::s_tradition = false;

bool CConverter::IsTraditionEnabled()
{
    return s_tradition;
}

void CConverter::SetTraditionEnabled(bool on)
{
    s_tradition = on;
}

//+---------------------------------------------------------------------------
//
// LoadTable —— 读一个 OpenCC UTF-8 字典文件
//
//  格式：`key<TAB>value1 value2 ...`（值以空格分隔，取第一个 = 规范首选）；
//  `#` 注释行、空行、恒等条目（key == value）跳过。
//
//----------------------------------------------------------------------------

bool CConverter::LoadTable(const wchar_t* path, std::unordered_map<std::wstring, std::wstring>& map)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || f == nullptr)
    {
        return false;
    }

    // 整块读入（表文件最大 ~1MB），一次性转宽字符
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0)
    {
        fclose(f);
        return false;
    }

    std::vector<char> buf(static_cast<size_t>(size));
    if (fread(buf.data(), 1, buf.size(), f) != buf.size())
    {
        fclose(f);
        return false;
    }
    fclose(f);

    int wlen = MultiByteToWideChar(CP_UTF8, 0, buf.data(), static_cast<int>(buf.size()), nullptr, 0);
    if (wlen <= 0)
    {
        return false;
    }
    std::wstring text(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, buf.data(), static_cast<int>(buf.size()), &text[0], wlen);

    size_t pos = 0;
    const size_t n = text.size();
    while (pos < n)
    {
        size_t eol = text.find(L'\n', pos);
        if (eol == std::wstring::npos)
        {
            eol = n;
        }
        std::wstring line = text.substr(pos, eol - pos);
        pos = eol + 1;

        // 去尾部 \r
        if (!line.empty() && line.back() == L'\r')
        {
            line.pop_back();
        }
        if (line.empty() || line[0] == L'#')
        {
            continue;
        }

        // 按 \t 分割 key 与 value 串
        size_t tab = line.find(L'\t');
        if (tab == std::wstring::npos)
        {
            continue;
        }
        std::wstring key = line.substr(0, tab);
        if (key.empty())
        {
            continue;
        }

        // value 串取第一个空格分隔的 token（OpenCC 首选）
        std::wstring values = line.substr(tab + 1);
        size_t sp = values.find_first_of(L" \t");
        std::wstring first = (sp == std::wstring::npos) ? values : values.substr(0, sp);
        if (first.empty())
        {
            continue;
        }
        // 恒等条目（key == value）必须保留：TSPhrases 用它们做"整词不转换"的
        // 防护（乾隆→乾隆，防逐字误转成"干隆"）。命中整词直接返回原样。
        map.emplace(std::move(key), std::move(first));
    }
    return true;
}

bool CConverter::LoadTables()
{
    if (s_loaded)
    {
        return true;   // 幂等：进程内只加载一次
    }

    bool ok = true;
    // OpenCC 转换表在安装目录（只读）：STPhrases/STCharacters（简→繁）、TSPhrases/TSCharacters（繁→简）
    ok &= LoadTable(EnginePaths::InstallFile(L"STPhrases.txt").c_str(), s_s2tPhrase);
    ok &= LoadTable(EnginePaths::InstallFile(L"STCharacters.txt").c_str(), s_s2tChar);
    ok &= LoadTable(EnginePaths::InstallFile(L"TSPhrases.txt").c_str(), s_t2sPhrase);
    ok &= LoadTable(EnginePaths::InstallFile(L"TSCharacters.txt").c_str(), s_t2sChar);
    s_loaded = true;
    return ok;
}

//+---------------------------------------------------------------------------
//
// Convert —— 整词查词组表，未命中逐字查字表，无映射原样保留
//
//----------------------------------------------------------------------------

std::wstring CConverter::Convert(const std::wstring& w,
    const std::unordered_map<std::wstring, std::wstring>& phrase,
    const std::unordered_map<std::wstring, std::wstring>& chars)
{
    if (w.empty())
    {
        return w;
    }

    // 1. 整词词组表（命中即整体替换——歧义词的准确来源："后面"→"後面"）
    auto it = phrase.find(w);
    if (it != phrase.end())
    {
        return it->second;
    }

    // 2. 逐字字表（未命中原样保留，英文/数字/符号天然安全）
    std::wstring out;
    out.reserve(w.size());
    for (wchar_t ch : w)
    {
        std::wstring one(1, ch);
        auto cit = chars.find(one);
        if (cit != chars.end())
        {
            out += cit->second;
        }
        else
        {
            out += ch;
        }
    }
    return out;
}

std::wstring CConverter::ToTraditional(const std::wstring& w)
{
    if (!s_tradition || w.empty())
    {
        return w;   // 默认关闭：零开销
    }
    return Convert(w, s_s2tPhrase, s_s2tChar);
}

std::wstring CConverter::ToSimplified(const std::wstring& w)
{
    if (!s_tradition || w.empty())
    {
        return w;
    }
    return Convert(w, s_t2sPhrase, s_t2sChar);
}
