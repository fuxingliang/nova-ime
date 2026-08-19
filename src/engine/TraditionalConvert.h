//+---------------------------------------------------------------------------
//
//  TraditionalConvert.h —— 简繁转换模块（输出层转换）
//
//  设计（2026-08-13，繁体输出功能）：
//    - 词库恒为简体（rime-ice），引擎核心（PinyinEngine.cpp）零改动。
//    - 查询输出：候选词 ToTraditional（简体→繁体）——候选窗显示与上屏文本
//      统一为繁体；DLL / Server 显示层零改动。
//    - 用户词回存：繁体模式下选词/造词入库的词是繁体，入库前 ToSimplified
//      （繁体→简体）——保证词库恒为简体，切换简繁模式无繁体残留。
//    - 转换表：OpenCC（BYVoid，Apache-2.0）官方字典 4 张：
//        bin\STPhrases.txt / STCharacters.txt  （简→繁，词组优先 + 逐字兜底）
//        bin\TSPhrases.txt / TSCharacters.txt  （繁→简）
//      格式：`key<TAB>value1 value2 ...`（多值按空格分隔，取第一个 = OpenCC
//      规范首选；恒等条目与 `#` 注释行跳过）。
//    - tradition=0（默认）：转换零开销（直接返回原串，不做查表）。
//
//----------------------------------------------------------------------------

#pragma once

#include <string>
#include <unordered_map>

class CConverter
{
public:
    // 加载 4 张转换表（bin\*.txt，进程内加载一次，幂等）。
    // 失败返回 false（引擎可继续工作，仅关闭转换能力）。
    static bool LoadTables();

    // 简繁输出开关（engine.conf tradition=1，运行期由管道 type 15 切换）。
    static bool IsTraditionEnabled();
    static void SetTraditionEnabled(bool on);

    // 简体 → 繁体：整词查词组表（STPhrases），未命中逐字查字表（STCharacters），
    // 无映射原样保留（英文/数字/符号天然安全）。
    static std::wstring ToTraditional(const std::wstring& w);

    // 繁体 → 简体：整词查 TSPhrases（防"乾隆/乾坤"逐字误转成"干隆/干坤"），
    // 未命中逐字查 TSCharacters。
    static std::wstring ToSimplified(const std::wstring& w);

private:
    // 读一个 UTF-8 表文件进 map（key → 首选值；恒等条目跳过）。
    static bool LoadTable(const wchar_t* path, std::unordered_map<std::wstring, std::wstring>& map);

    static std::wstring Convert(const std::wstring& w,
        const std::unordered_map<std::wstring, std::wstring>& phrase,
        const std::unordered_map<std::wstring, std::wstring>& chars);

    static std::unordered_map<std::wstring, std::wstring> s_s2tPhrase;
    static std::unordered_map<std::wstring, std::wstring> s_s2tChar;
    static std::unordered_map<std::wstring, std::wstring> s_t2sPhrase;
    static std::unordered_map<std::wstring, std::wstring> s_t2sChar;
    static bool s_loaded;
    static bool s_tradition;
};
