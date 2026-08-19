//+---------------------------------------------------------------------------
//
//  PinyinEngine.cpp
//
//  Pinyin-Plus 拼音引擎实现
//
//----------------------------------------------------------------------------

#include "Private.h"
#include "PinyinEngine.h"
#include "PinyinIpc.h"
#include "PathUtil.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>

//+---------------------------------------------------------------------------
//
// HasNonHanzi — 词条是否含非汉字字符（符号/emoji）
//
// 用于简拼候选排序：音节数完全匹配的组内，符号词条（如 △/≌/😄）优先于
// 汉字词（sjx → △ 直达，不被"三角形/数据线"等高频词淹没）。
// 汉字判定必须覆盖：基本区 U+4E00-9FFF、扩展 A U+3400-4DBF、兼容表意
// U+F900-FAFF、扩展 B-G（UTF-16 代理对，0xD800-DFFF）——否则主词库
// 海量生僻字（㨃/𬘘/䐛 等扩展区汉字）会被误判为"符号"而置顶候选，
// 把"的/大/了"等常用字挤到后面（2026-08-13 修复）。
//
//----------------------------------------------------------------------------

static bool IsHanzi(wchar_t c)
{
    if (c >= 0x4E00 && c <= 0x9FFF) return true;   // 基本区
    if (c >= 0x3400 && c <= 0x4DBF) return true;   // 扩展 A
    if (c >= 0xF900 && c <= 0xFAFF) return true;   // 兼容表意
    if (c >= 0xD800 && c <= 0xDFFF) return true;   // 代理对 → 扩展 B-G 汉字
    return false;
}

static bool HasNonHanzi(const std::wstring &s)
{
    for (wchar_t c : s)
    {
        if (!IsHanzi(c))
        {
            return true;
        }
    }
    return false;
}

//+---------------------------------------------------------------------------
//
// ctor / dtor
//
//----------------------------------------------------------------------------

CPinyinEngine::CPinyinEngine()
{
}

CPinyinEngine::~CPinyinEngine()
{
}

//+---------------------------------------------------------------------------
//
// Initialize
//
// 读取 UTF-8 词库文件，解析并按 (pinyin, -freq) 排序。
//
//----------------------------------------------------------------------------

BOOL CPinyinEngine::Initialize(_In_z_ LPCWSTR pwszDictPath)
{
    if (pwszDictPath != nullptr)
    {
        _dictPath = pwszDictPath;
    }
    if (_dictPath.empty())
    {
        return FALSE;
    }
    // 用户词库路径 = 数据目录（%AppData%\NovaInput\userdict.txt，可写）。
    // 不再跟随主词库目录：安装目录只读（Program Files 场景），用户数据必须独立。
    _userDictPath = EnginePaths::DataFile(L"userdict.txt");
    _downWordsPath = EnginePaths::DataFile(L"downweight.txt");
    LoadDownWords();
    return ReloadAll();
}

//+---------------------------------------------------------------------------
//
// LoadMainDict — 加载主词库（UTF-8）：解析 pinyin\tword\tfreq[\tinitial]
//
// _entries 清空后重建，供 Initialize / ReloadAll 共用。
// 优先走二进制缓存（<dict>.bin，见 TryLoadMainDictCache）——冷启动 5-10s → <1s。
//
//----------------------------------------------------------------------------

BOOL CPinyinEngine::LoadMainDict()
{
    _entries.clear();

    // 1. 二进制缓存快速加载（跳过 29MB 文本解析 + 88 万条排序）
    if (TryLoadMainDictCache())
    {
        _mainDictEntries = _entries.size();
        return !_entries.empty();
    }

    HANDLE hFile = CreateFileW(_dictPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        return FALSE;
    }

    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart == 0 || fileSize.QuadPart > 256 * 1024 * 1024)
    {
        CloseHandle(hFile);
        return FALSE;
    }

    DWORD bufSize = static_cast<DWORD>(fileSize.QuadPart);
    std::vector<char> buf(bufSize);
    DWORD bytesRead = 0;
    if (!ReadFile(hFile, buf.data(), bufSize, &bytesRead, nullptr) || bytesRead == 0)
    {
        CloseHandle(hFile);
        return FALSE;
    }
    CloseHandle(hFile);

    // UTF-8 -> UTF-16
    int wcharLen = MultiByteToWideChar(CP_UTF8, 0, buf.data(), static_cast<int>(bytesRead), nullptr, 0);
    if (wcharLen <= 0)
    {
        return FALSE;
    }
    std::wstring text(wcharLen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, buf.data(), static_cast<int>(bytesRead), &text[0], wcharLen);

    // 逐行解析：pinyin\tword\tfreq
    size_t pos = 0;
    const size_t textLen = text.size();
    while (pos < textLen)
    {
        size_t nl = text.find(L'\n', pos);
        std::wstring line;
        if (nl == std::wstring::npos)
        {
            line.assign(text, pos, textLen - pos);
            pos = textLen;
        }
        else
        {
            line.assign(text, pos, nl - pos);
            pos = nl + 1;
        }

        if (!line.empty() && line.back() == L'\r')
        {
            line.pop_back();
        }
        if (line.empty())
        {
            continue;
        }

        size_t t1 = line.find(L'\t');
        if (t1 == std::wstring::npos)
        {
            continue;
        }
        size_t t2 = line.find(L'\t', t1 + 1);
        if (t2 == std::wstring::npos)
        {
            continue;
        }

        std::wstring pinyin = line.substr(0, t1);
        std::wstring word = line.substr(t1 + 1, t2 - t1 - 1);
        if (pinyin.empty() || word.empty())
        {
            continue;
        }
        float freq = static_cast<float>(_wtof(line.substr(t2 + 1).c_str()));

        // 可选第 4 列：简拼（声母串），旧格式词库无此列
        std::wstring initial;
        size_t t3 = line.find(L'\t', t2 + 1);
        if (t3 != std::wstring::npos)
        {
            initial = line.substr(t3 + 1);
        }

        Entry entry;
        entry.pinyin = pinyin;
        entry.word = word;
        entry.initial = initial;
        entry.freq = freq;
        entry.isUser = false;   // 主词库条目：不可删除（删除功能只删用户造词）
        _entries.push_back(std::move(entry));
    }

    // 聚合字频校正（2026-08-13）：词库 freq 对虚词/助词（"了"等）严重低估
    //（"了" 3.48M < "来" 14.8M，但真实语料"了"出现频率远高于"来"）。
    // 统计每个汉字在全部词条中出现位置的词频总和作为"真实字频"，覆盖
    // 单字条目的 freq——单音节/简拼候选按此排序，高频字（了/的/一/是…）
    // 自然靠前。词条（多字）不覆盖，词组排序不受影响。
    {
        std::map<wchar_t, double> charFreq;
        for (const Entry &e : _entries)
        {
            const double f = e.freq;
            for (wchar_t c : e.word)
            {
                charFreq[c] += f;
            }
        }
        for (Entry &e : _entries)
        {
            if (e.word.size() == 1)
            {
                auto it = charFreq.find(e.word[0]);
                if (it != charFreq.end())
                {
                    e.freq = static_cast<float>(it->second);
                }
            }
        }
    }

    // 排序：拼音升序；同拼音按词频降序
    std::sort(_entries.begin(), _entries.end(),
        [](const Entry &a, const Entry &b)
        {
            int cmp = a.pinyin.compare(b.pinyin);
            if (cmp != 0)
            {
                return cmp < 0;
            }
            return a.freq > b.freq;
        });

    // 2. 写二进制缓存（下次启动直接读入，跳过文本解析；失败无妨，下次重新解析）
    SaveMainDictCache();

    _mainDictEntries = _entries.size();
    return !_entries.empty();
}

//+---------------------------------------------------------------------------
//
// TryLoadMainDictCache — 从二进制缓存读主词库（<dict>.bin）
//
// 缓存格式（全部小端，本地生成/读取，无跨平台需求）：
//   header: u32 magic=0x50424C4E | u32 version=2 | u64 txtSize | u64 txtMtime | u32 count
//   每条 Entry: u32 pinyinLen + pinyin(UTF-16) | u32 wordLen + word | u32 initialLen + initial
//               | f32 freq | u8 isUser
// 有效性 = magic/version 正确 + txt 的 size/mtime 未变（txt 更新自动失效）。
// 任何损坏（长度越界/读到 EOF）→ 清空 _entries 返回 FALSE，回退文本解析。
//
//----------------------------------------------------------------------------

BOOL CPinyinEngine::TryLoadMainDictCache()
{
    const std::wstring cachePath = _dictPath + L".bin";

    // 主词库 txt 的 size + mtime（校验缓存是否过期）
    WIN32_FILE_ATTRIBUTE_DATA attrs{};
    if (!GetFileAttributesExW(_dictPath.c_str(), GetFileExInfoStandard, &attrs))
    {
        return FALSE;
    }
    const ULONGLONG txtSize = ((ULONGLONG)attrs.nFileSizeHigh << 32) | attrs.nFileSizeLow;
    const ULONGLONG txtMtime = ((ULONGLONG)attrs.ftLastWriteTime.dwHighDateTime << 32) | attrs.ftLastWriteTime.dwLowDateTime;

    HANDLE hFile = CreateFileW(cachePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        CPinyinIpc::DebugLog(L"Cache: MISS %ls", cachePath.c_str());
        return FALSE;
    }

    struct Header
    {
        unsigned int magic;
        unsigned int version;
        ULONGLONG txtSize;
        ULONGLONG txtMtime;
        unsigned int count;
    } hdr{};

    // 一次性读入整个缓存（替代逐条 ReadFile：88 万条 × 7 次 = 616 万次系统调用，
    // 在 CPU 满载下 ~22s；单次大读 + 内存解析 <1s）
    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(hFile, &fileSize) ||
        fileSize.QuadPart < static_cast<LONGLONG>(sizeof(hdr)) ||
        fileSize.QuadPart > 512LL * 1024 * 1024)
    {
        CloseHandle(hFile);
        return FALSE;
    }
    std::vector<BYTE> buf(static_cast<size_t>(fileSize.QuadPart));
    DWORD rd = 0;
    const BOOL ok = ReadFile(hFile, buf.data(), static_cast<DWORD>(buf.size()), &rd, nullptr);
    CloseHandle(hFile);
    if (!ok || rd != buf.size())
    {
        return FALSE;
    }

    // 内存解析
    const BYTE *p = buf.data();
    const BYTE *end = p + buf.size();
    auto readU32 = [&](unsigned int &out) -> bool
    {
        if (end - p < 4) return false;
        memcpy(&out, p, 4); p += 4; return true;
    };
    if (!readU32(hdr.magic) || !readU32(hdr.version) ||
        end - p < 16 || (memcpy(&hdr.txtSize, p, 8), p += 8, 0) ||
        (memcpy(&hdr.txtMtime, p, 8), p += 8, 0) ||
        !readU32(hdr.count))
    {
        return FALSE;
    }
    if (hdr.magic != 0x50424C4E || hdr.version != 2 ||
        hdr.txtSize != txtSize || hdr.txtMtime != txtMtime ||
        hdr.count == 0 || hdr.count > 2000000)
    {
        CPinyinIpc::DebugLog(L"Cache: STALE/INVALID magic=%08X ver=%u size=%llu/%llu mtime=%llu/%llu count=%u",
            hdr.magic, hdr.version, hdr.txtSize, txtSize, hdr.txtMtime, txtMtime, hdr.count);
        return FALSE;   // 缓存缺失/过期/损坏 → 回退文本解析
    }

    const unsigned int count = hdr.count;
    _entries.reserve(count);

    auto readStr = [&](std::wstring &out) -> bool
    {
        unsigned int len = 0;
        if (!readU32(len) || len > 64)
        {
            return false;   // 单字段超长 → 损坏
        }
        if (static_cast<size_t>(end - p) < static_cast<size_t>(len) * sizeof(wchar_t))
        {
            return false;
        }
        if (len > 0)
        {
            out.assign(reinterpret_cast<const wchar_t *>(p), len);
            p += static_cast<size_t>(len) * sizeof(wchar_t);
        }
        return true;
    };

    for (unsigned int i = 0; i < count; i++)
    {
        Entry e;
        if (!readStr(e.pinyin) || !readStr(e.word) || !readStr(e.initial) ||
            end - p < 4 + 1 || (memcpy(&e.freq, p, 4), p += 4, 0))
        {
            _entries.clear();
            return FALSE;
        }
        e.isUser = (*p != 0);
        p += 1;
        _entries.push_back(std::move(e));
    }
    CPinyinIpc::DebugLog(L"Cache: HIT %ls entries=%u", cachePath.c_str(), count);
    return TRUE;
}

//+---------------------------------------------------------------------------
//
// SaveMainDictCache — 把主词库写为二进制缓存（<dict>.bin）
//
// 只在文本解析成功后调用（缓存保存排序后的顺序，读入无需再 sort）。
// 写入失败静默删除缓存，不影响功能。
//
//----------------------------------------------------------------------------

void CPinyinEngine::SaveMainDictCache()
{
    if (_entries.empty())
    {
        return;
    }
    const std::wstring cachePath = _dictPath + L".bin";

    WIN32_FILE_ATTRIBUTE_DATA attrs{};
    if (!GetFileAttributesExW(_dictPath.c_str(), GetFileExInfoStandard, &attrs))
    {
        return;
    }
    const ULONGLONG txtSize = ((ULONGLONG)attrs.nFileSizeHigh << 32) | attrs.nFileSizeLow;
    const ULONGLONG txtMtime = ((ULONGLONG)attrs.ftLastWriteTime.dwHighDateTime << 32) | attrs.ftLastWriteTime.dwLowDateTime;

    HANDLE hFile = CreateFileW(cachePath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        return;
    }

    struct Header
    {
        unsigned int magic;
        unsigned int version;
        ULONGLONG txtSize;
        ULONGLONG txtMtime;
        unsigned int count;
    } hdr;
    hdr.magic = 0x50424C4E;
    hdr.version = 2;
    hdr.txtSize = txtSize;
    hdr.txtMtime = txtMtime;
    hdr.count = static_cast<unsigned int>(_entries.size());

    // 一次性构建内存缓冲区再写入（替代逐条 WriteFile：88 万条 × 5 次 = 440 万次
    // 系统调用，首次写缓存 ~10s+；单次大写 <1s）。
    // 注意：header 逐字段写入（28 字节），不可用 struct 整块写——u64 成员会引入
    // 对齐填充（sizeof=32），读取端按 28 字节解析会错位 4 字节导致缓存永远失效。
    std::vector<BYTE> buf;
    buf.reserve(static_cast<size_t>(hdr.count) * 64 + 32);
    auto append = [&](const void *data, size_t len)
    {
        const BYTE *b = static_cast<const BYTE *>(data);
        buf.insert(buf.end(), b, b + len);
    };
    append(&hdr.magic, sizeof(hdr.magic));
    append(&hdr.version, sizeof(hdr.version));
    append(&hdr.txtSize, sizeof(hdr.txtSize));
    append(&hdr.txtMtime, sizeof(hdr.txtMtime));
    append(&hdr.count, sizeof(hdr.count));
    for (const Entry &e : _entries)
    {
        const unsigned int plen = static_cast<unsigned int>(e.pinyin.size());
        const unsigned int wlen = static_cast<unsigned int>(e.word.size());
        const unsigned int ilen = static_cast<unsigned int>(e.initial.size());
        append(&plen, sizeof(plen));
        append(e.pinyin.data(), plen * sizeof(wchar_t));
        append(&wlen, sizeof(wlen));
        append(e.word.data(), wlen * sizeof(wchar_t));
        append(&ilen, sizeof(ilen));
        append(e.initial.data(), ilen * sizeof(wchar_t));
        append(&e.freq, sizeof(e.freq));
        const BYTE isUser = e.isUser ? 1 : 0;
        append(&isUser, 1);
    }

    DWORD wr = 0;
    BOOL ok = WriteFile(hFile, buf.data(), static_cast<DWORD>(buf.size()), &wr, nullptr) && wr == buf.size();
    CloseHandle(hFile);
    if (!ok)
    {
        DeleteFileW(cachePath.c_str());   // 写坏 → 删掉，下次重新生成
    }
    else
    {
        CPinyinIpc::DebugLog(L"Cache: WRITTEN %ls entries=%u", cachePath.c_str(), hdr.count);
    }
}

//+---------------------------------------------------------------------------
//
// ReloadAll — 词库热重载（主词库 + 用户词库全部重建）
//
// 导入词库后由管道消息触发（EnginePipe case 11，在引擎锁内执行，与查询互斥）。
// 重建流程与 Initialize 一致：主词库 → 用户词库 → 简拼索引 → 音节表。
//
//----------------------------------------------------------------------------

BOOL CPinyinEngine::ReloadAll()
{
    auto t0 = std::chrono::steady_clock::now();
    auto mark = [&](const wchar_t *stage) {
        auto now = std::chrono::steady_clock::now();
        CPinyinIpc::DebugLog(L"ReloadAll[%ls]: %lld ms", stage,
            std::chrono::duration_cast<std::chrono::milliseconds>(now - t0).count());
    };

    if (!LoadMainDict())
    {
        CPinyinIpc::DebugLog(L"ReloadAll FAILED: LoadMainDict");
        return FALSE;
    }
    mark(L"LoadMainDict");

    LoadUserDict();
    mark(L"LoadUserDict");

    // 构建音节表与单字索引（混合简拼/全拼匹配用）——补算简拼依赖音节表
    BuildSyllableIndex();
    CPinyinIpc::DebugLog(L"ReloadAll: syllables=%zu", _syllableTable.size());
    mark(L"BuildSyllableIndex");

    // 补算存量词条简拼（旧 3 列 userdict 遗留，重启后简拼/混合查不到），再建简拼索引
    FillMissingInitials();
    mark(L"FillMissingInitials");

    // 构建简拼索引（按 initial 升序，用于二分前缀查找）
    RebuildInitialIndex();
    CPinyinIpc::DebugLog(L"ReloadAll: entries=%zu initialEntries=%zu", _entries.size(), _initialEntries.size());
    mark(L"RebuildInitialIndex");

    // 索引一致性自检（异常自动重建）
    ValidateInitialIndex();
    mark(L"ValidateInitialIndex");

    return !_entries.empty();
}

//+---------------------------------------------------------------------------
//
// FillMissingInitials — 补算存量词条简拼
//
// 旧版 BoostWord 对词库不存在的词只落盘 3 列（无 initial），重启后词条
// initial 为空 → 简拼/混合（_initialEntries）查不到。这里在音节表构建后
// 统一补算：仅完整拼音词条（IsValidFullPinyin）可算；畸形简拼 key
// （如 zt→钟婷，自身就是简拼入口）跳过——它们本就不该进简拼索引。
//
//----------------------------------------------------------------------------

void CPinyinEngine::FillMissingInitials()
{
    if (_syllableTable.empty())
    {
        return;
    }
    for (auto &e : _entries)
    {
        if (!e.initial.empty())
        {
            continue;
        }
        if (!IsValidFullPinyin(e.pinyin))
        {
            continue;
        }
        std::vector<std::wstring> syls = SegmentToSyllablesBest(e.pinyin);
        for (const std::wstring &s : syls)
        {
            if (!s.empty())
            {
                e.initial += s[0];
            }
        }
    }
}

//+---------------------------------------------------------------------------
//
// RebuildInitialIndex — 重建简拼索引
//
// 按 initial 升序排序的指针视图；同 initial 内按词频降序。
// 用户造词新增词条后需要重建，否则新词的简拼匹配不到。
//
//----------------------------------------------------------------------------

void CPinyinEngine::RebuildInitialIndex()
{
    _initialEntries.clear();
    _initialEntries.reserve(_entries.size());
    for (size_t i = 0; i < _entries.size(); i++)
    {
        if (!_entries[i].initial.empty())
        {
            _initialEntries.push_back(i);
        }
    }
    std::sort(_initialEntries.begin(), _initialEntries.end(),
        [this](size_t a, size_t b)
        {
            int cmp = _entries[a].initial.compare(_entries[b].initial);
            if (cmp != 0)
            {
                return cmp < 0;
            }
            return _entries[a].freq > _entries[b].freq;
        });
}

//+---------------------------------------------------------------------------
//
// ValidateInitialIndex — 校验简拼索引一致性，异常自动重建
//
// 下标索引的"局部维护"（insert 平移/sort 交换/局部重排）一旦漏一步就整体错位
// （2026-08-13 根因 4：造词后 fxl 查不到傅兴亮）。本校验在启动与每次增删词后
// 执行，O(88 万) 遍历 ~2ms：发现下标越界 / initial 为空 / 排序错乱 → 全量重建兜底。
//
//----------------------------------------------------------------------------

void CPinyinEngine::ValidateInitialIndex()
{
    bool bad = false;
    for (size_t i = 0; i < _initialEntries.size(); i++)
    {
        const size_t idx = _initialEntries[i];
        if (idx >= _entries.size() || _entries[idx].initial.empty())
        {
            bad = true;
            break;
        }
        if (i > 0)
        {
            const size_t prev = _initialEntries[i - 1];
            const int cmp = _entries[prev].initial.compare(_entries[idx].initial);
            if (cmp > 0 || (cmp == 0 && _entries[prev].freq < _entries[idx].freq))
            {
                bad = true;
                break;
            }
        }
    }
    if (bad)
    {
        CPinyinIpc::DebugLog(L"ValidateInitialIndex: 索引不一致，自动重建 (entries=%zu index=%zu)",
            _entries.size(), _initialEntries.size());
        RebuildInitialIndex();
    }
}

//+---------------------------------------------------------------------------
//
// BuildSyllableIndex — 构建音节表与单字索引
//
// 音节表 = 词库中全部单字词条的拼音（合法完整音节），按长度降序供贪心切分。
// 单字索引 = 音节 → 该音节下最高频的若干个单字（组合人名/生僻词兜底）。
//
//----------------------------------------------------------------------------

void CPinyinEngine::BuildSyllableIndex()
{
    _syllableTable.clear();
    _singleCharBySyllable.clear();

    std::map<std::wstring, std::vector<const Entry*>> bySyl;
    for (const Entry &e : _entries)
    {
        // 单字条目（word 长度 1 且拼音无分隔符）才纳入音节表；单字池存该音节全部单字（按 freq 降序）。
        // 单字母拼音只允许零声母（a/e/o）——畸形简拼条目（d→大、l→来 等历史遗留，
        // 常见于用户词库早期自学习写入）会被过滤，避免输入单字母时被单个错配字短路。
        if (e.word.size() == 1 && e.pinyin.find(L'\'') == std::wstring::npos
            && !(e.pinyin.size() == 1 && e.pinyin[0] != L'a' && e.pinyin[0] != L'e' && e.pinyin[0] != L'o'))
        {
            bySyl[e.pinyin].push_back(&e);
        }
    }

    for (auto &kv : bySyl)
    {
        _syllableTable.push_back(kv.first);
        std::vector<SingleChar> chars;
        chars.reserve(kv.second.size());
        for (const Entry *e : kv.second)
        {
            SingleChar sc;
            sc.word = e->word;
            sc.freq = e->freq;
            chars.push_back(sc);
        }
        _singleCharBySyllable[kv.first] = std::move(chars);
    }

    // 音节表按长度降序（切分优先完整长音节），同长按字典序
    std::sort(_syllableTable.begin(), _syllableTable.end(),
        [](const std::wstring &a, const std::wstring &b)
        {
            if (a.size() != b.size())
            {
                return a.size() > b.size();
            }
            return a < b;
        });

    // 首字母分桶（SearchPrefix 定位首音节用，避免每条匹配遍历全部音节）
    _syllableByInitial.clear();
    for (const std::wstring &s : _syllableTable)
    {
        _syllableByInitial[s[0]].push_back(s);
    }
}

//+---------------------------------------------------------------------------
//
// SegmentPinyin — 将连续拼音串切分为音节序列（贪心 + 回溯）
//
// 音节候选 = 23 个声母 ∪ 完整音节表。按长度降序尝试，保证 "xian" 切成
// 一个音节而非 "xi"+"an"；切分走不通时回溯。
//
//----------------------------------------------------------------------------

bool CPinyinEngine::SegmentPinyin(_In_ const std::vector<std::wstring> &sylTable, _In_ const std::wstring &s, _Inout_ std::vector<std::wstring> &out)
{
    // 声母表（长度降序：zh/ch/sh 在前）
    static const wchar_t* INITIALS[] = {
        L"zh", L"ch", L"sh",
        L"b", L"p", L"m", L"f", L"d", L"t", L"n", L"l", L"g", L"k", L"h", L"j", L"q", L"x", L"r", L"z", L"c", L"s", L"y", L"w"
    };

    // 回溯切分
    std::function<bool(size_t)> dfs = [&](size_t pos) -> bool
    {
        if (pos == s.size())
        {
            return true;
        }
        // 完整音节优先（长度降序已保证）
        for (const std::wstring &syl : sylTable)
        {
            if (pos + syl.size() <= s.size() && s.compare(pos, syl.size(), syl) == 0)
            {
                out.push_back(syl);
                if (dfs(pos + syl.size()))
                {
                    return true;
                }
                out.pop_back();
            }
        }
        // 声母（含 zh/ch/sh 等双字母）
        for (const wchar_t *init : INITIALS)
        {
            size_t ilen = wcslen(init);
            if (pos + ilen <= s.size() && s.compare(pos, ilen, init) == 0)
            {
                out.push_back(init);
                if (dfs(pos + ilen))
                {
                    return true;
                }
                out.pop_back();
            }
        }
        return false;
    };

    out.clear();
    return dfs(0);
}

//+---------------------------------------------------------------------------
//
// IsValidFullPinyin — 拼音串是否可完整切分为合法完整音节
//
// SegmentPinyin 允许声母作为切分候选（混合简拼匹配用），但整句预测把
// 每个子串当"词条拼音 key"查词时，绝不能把 z→钟婷、zt→钟婷天气 这类
// 简拼/畸形词条当作合法音节组合——否则会拼出怪异整句（如 zting → "钟婷听"）。
// 因此这里额外要求：切出的每一段都必须存在于完整音节表 _syllableTable。
//
//----------------------------------------------------------------------------

bool CPinyinEngine::IsValidFullPinyin(const std::wstring &key) const
{
    if (key.empty() || _syllableTable.empty())
    {
        return false;
    }
    std::vector<std::wstring> syls;
    if (!SegmentPinyin(_syllableTable, key, syls) || syls.empty())
    {
        return false;
    }
    for (const std::wstring &s : syls)
    {
        if (std::find(_syllableTable.begin(), _syllableTable.end(), s) == _syllableTable.end())
        {
            return false;
        }
    }
    return true;
}

//+---------------------------------------------------------------------------
//
// SearchPrefix
//
// 二分定位前缀区间，收集所有匹配项，并按词频降序保留前 MAX_RESULTS 个。
//
//----------------------------------------------------------------------------

void CPinyinEngine::SearchPrefix(const std::wstring &prefix, _Out_ std::vector<const Entry*> &matched) const
{
    matched.clear();
    if (prefix.empty())
    {
        return;
    }

    auto it = std::lower_bound(_entries.begin(), _entries.end(), prefix,
        [](const Entry &e, const std::wstring &p) { return e.pinyin < p; });

    // 收集截断：_entries 按 (pinyin, -freq) 排序，同 pinyin 组内已按词频降序。
    // 每个【首音节】只保留词频最高的前 GROUP_TOP 条，并设总量上限——
    // 按首音节分组（而非完整拼音组）是关键：单字母前缀（"d"）匹配的
    // 完整拼音组多达数百（dahao/dahui/dai...），若按完整拼音组+总量截断，
    // 会在遍历到 de/di 组之前就攒满 COLLECT_CAP 而 break，导致"的"（de）
    // "第"（di）等常用字永远进不了候选。按首音节分组保证每个首音节
    //（da/de/di/du...）都有代表进候选，再按词频全局排序。
    const size_t GROUP_TOP = 40;
    const size_t COLLECT_CAP = 6000;
    std::wstring curGroup;
    size_t inGroup = 0;
    size_t collected = 0;
    for (; it != _entries.end(); ++it)
    {
        if (it->pinyin.compare(0, prefix.size(), prefix) != 0)
        {
            break;
        }
        // 取 pinyin 的首音节：只遍历同首字母的音节分桶（~20 个，而非全部 477 个）
        const std::wstring &py = it->pinyin;
        std::wstring firstSyl;
        auto bucket = _syllableByInitial.find(py[0]);
        if (bucket != _syllableByInitial.end())
        {
            for (const std::wstring &s : bucket->second)
            {
                if (py.size() >= s.size() && py.compare(0, s.size(), s) == 0)
                {
                    firstSyl = s;
                    break;
                }
            }
        }
        if (firstSyl.empty())
        {
            firstSyl = py.substr(0, 1);
        }
        if (firstSyl != curGroup)
        {
            curGroup = firstSyl;
            inGroup = 0;
        }
        if (++inGroup > GROUP_TOP)
        {
            continue;   // 首音节组内低频词跳过（组内已按词频降序）
        }
        matched.push_back(&(*it));
        if (++collected >= COLLECT_CAP)
        {
            break;
        }
    }

    if (matched.size() > MAX_RESULTS)
    {
        std::partial_sort(matched.begin(), matched.begin() + MAX_RESULTS, matched.end(),
            [this](const Entry *a, const Entry *b) { return EffectiveFreq(*a) > EffectiveFreq(*b); });
        matched.resize(MAX_RESULTS);
    }
    else
    {
        std::sort(matched.begin(), matched.end(),
            [this](const Entry *a, const Entry *b) { return EffectiveFreq(*a) > EffectiveFreq(*b); });
    }
}

//+---------------------------------------------------------------------------
//
// FillResults
//
//----------------------------------------------------------------------------

void CPinyinEngine::FillResults(_In_ std::vector<const Entry*> &matched, _Inout_ CSampleImeArray<CCandidateListItem> *pItemList)
{
    for (const Entry *e : matched)
    {
        CCandidateListItem *pLI = pItemList->Append();
        if (!pLI)
        {
            break;
        }
        pLI->_ItemString.Set(e->word.c_str(), static_cast<DWORD_PTR>(e->word.size()));
        pLI->_FindKeyCode.Set(e->pinyin.c_str(), static_cast<DWORD_PTR>(e->pinyin.size()));
    }
}

//+---------------------------------------------------------------------------
//
// CollectWord — 精确拼音匹配
//
//----------------------------------------------------------------------------

void CPinyinEngine::CollectWord(_In_ CStringRange *pKeyCode, _Inout_ CSampleImeArray<CCandidateListItem> *pItemList)
{
    if (!pKeyCode || !pItemList)
    {
        return;
    }

    std::wstring key(pKeyCode->Get(), pKeyCode->GetLength());

    auto lo = std::lower_bound(_entries.begin(), _entries.end(), key,
        [](const Entry &e, const std::wstring &p) { return e.pinyin < p; });
    auto hi = std::upper_bound(lo, _entries.end(), key,
        [](const std::wstring &p, const Entry &e) { return p < e.pinyin; });

    std::vector<const Entry*> matched;
    for (auto it = lo; it != hi; ++it)
    {
        matched.push_back(&(*it));
        if (matched.size() >= MAX_RESULTS)
        {
            break;
        }
    }
    FillResults(matched, pItemList);
}

//+---------------------------------------------------------------------------
//
// CollectWordForWildcard — 前缀匹配（pKeyCode 末尾可带 '*'）
//
//----------------------------------------------------------------------------

void CPinyinEngine::CollectWordForWildcard(_In_ CStringRange *pKeyCode, _Inout_ CSampleImeArray<CCandidateListItem> *pItemList)
{
    if (!pKeyCode || !pItemList)
    {
        return;
    }

    std::wstring pattern(pKeyCode->Get(), pKeyCode->GetLength());

    // '*' 通配符：只取第一个 '*' 之前的部分做前缀匹配
    size_t star = pattern.find(L'*');
    if (star != std::wstring::npos)
    {
        pattern = pattern.substr(0, star);
    }
    if (pattern.empty())
    {
        return;
    }

    // 单音节输入（如 "fu"）：单字加权混排，保证低频常用字（如姓氏"傅"）可达。
    // 单字母 pattern 仅当零声母音节（a/e/o）时走单字池——畸形简拼条目
    // （d→大、l→来）即使混入索引也不得短路正常前缀匹配。
    const bool singleLetterZeroInitial = pattern.size() == 1
        && (pattern[0] == L'a' || pattern[0] == L'e' || pattern[0] == L'o');
    auto sit = _singleCharBySyllable.find(pattern);
    if (singleLetterZeroInitial && sit != _singleCharBySyllable.end() && !sit->second.empty())
    {
        CollectSingleSyllable(pattern, sit->second, pItemList);
        return;
    }

    std::vector<const Entry*> matched;
    SearchPrefix(pattern, matched);
    // 过滤简拼畸形词条（用户词库历史格式：拼音=简拼，如 sjx→数据线）：
    // 若不过滤，ForWildcard("sjx*") 会命中它们导致候选非空，从而截断后续
    // 简拼回退路径（CollectWordByInitial 被跳过，sjx 打不出 △）。
    // 正常词条 pinyin 都是合法完整音节组合，IsValidFullPinyin 校验不受影响。
    matched.erase(
        std::remove_if(matched.begin(), matched.end(),
            [this](const Entry *e) { return !IsValidFullPinyin(e->pinyin); }),
        matched.end());
    FillResults(matched, pItemList);
}

//+---------------------------------------------------------------------------
//
// CollectSingleSyllable — 单音节输入：只展示单字（用户找字场景）
//
// 输入单个完整音节（如 "fu"）时，用户一定是在找一个字（打"父母"会输 fumu），
// 因此候选列表只输出该音节的全部单字（按词频降序，已由 _entries 排序保证），
// 不混入"父母/负责"等词语干扰。单字表来自词库真实数据，保证每个汉字可达。
//
//----------------------------------------------------------------------------

void CPinyinEngine::CollectSingleSyllable(_In_ const std::wstring &syl, _In_ const std::vector<SingleChar> &chars, _Inout_ CSampleImeArray<CCandidateListItem> *pItemList)
{
    // 全部单字按词频降序输出（chars 顺序即词频降序）
    const size_t MAX_OUT = 500;
    size_t out = 0;
    for (const SingleChar &sc : chars)
    {
        CCandidateListItem *pLI = pItemList->Append();
        if (!pLI)
        {
            break;
        }
        pLI->_ItemString.Set(sc.word.c_str(), static_cast<DWORD_PTR>(sc.word.size()));
        pLI->_FindKeyCode.Set(syl.c_str(), static_cast<DWORD_PTR>(syl.size()));
        out++;
        if (out >= MAX_OUT)
        {
            break;
        }
    }
}

//+---------------------------------------------------------------------------
//
// CollectWordByInitial — 简拼（声母串）匹配
//
// 在 _initialEntries 上做前缀二分查找：输入 "nh" 命中 "你好"(nihao→nh)。
// 简拼索引按 initial 升序；同 initial 内按词频降序。
//
//----------------------------------------------------------------------------

void CPinyinEngine::CollectWordByInitial(_In_ CStringRange *pKeyCode, _Inout_ CSampleImeArray<CCandidateListItem> *pItemList)
{
    if (!pKeyCode || !pItemList || _initialEntries.empty())
    {
        return;
    }

    std::wstring key(pKeyCode->Get(), pKeyCode->GetLength());

    // 去尾部通配符
    size_t star = key.find(L'*');
    if (star != std::wstring::npos)
    {
        key = key.substr(0, star);
    }
    if (key.empty())
    {
        return;
    }

    auto lo = std::lower_bound(_initialEntries.begin(), _initialEntries.end(), key,
        [this](size_t i, const std::wstring &p) { return _entries[i].initial < p; });

    // 收集上限放宽：前缀匹配（音节数多于输入的词）不能挤掉音节数完全匹配的词。
    // 例：输入 "sjh"，若 4+ 音节词的简拼以 "sjh" 开头，按 MAX_RESULTS 提前截断
    // 会把 "数据库"(sjh) 这类完全匹配挤出候选。
    const size_t COLLECT_CAP = 500;
    std::vector<const Entry*> matched;
    for (; lo != _initialEntries.end(); ++lo)
    {
        if (_entries[*lo].initial.compare(0, key.size(), key) != 0)
        {
            break;
        }
        matched.push_back(&_entries[*lo]);
        if (matched.size() >= COLLECT_CAP)
        {
            break;
        }
    }

    // 简拼候选排序：音节数越接近输入串越靠前（完全匹配 > 多音节前缀匹配），
    // 音节数完全匹配的组内：
    //   1. 用户词优先（isUser，用户造词/自学习）——否则低词频用户词
    //      （如"傅兴亮"freq=2）会被主词库高频词（"放下了/复兴路"）压到
    //      候选末尾甚至 50 名之外，简拼/混合永远打不出来。
    //   2. 符号词条（含非汉字字符）优先 ——保证简拼直达符号（sjx→△、qdy→≌）
    //   3. 词频降序（降权词 EffectiveFreq=-1 → 沉底）。
    std::sort(matched.begin(), matched.end(),
        [this, &key](const Entry *a, const Entry *b)
        {
            int da = static_cast<int>(a->initial.size()) - static_cast<int>(key.size());
            int db = static_cast<int>(b->initial.size()) - static_cast<int>(key.size());
            if (da != db)
            {
                return da < db;
            }
            bool aUser = a->isUser;
            bool bUser = b->isUser;
            if (aUser != bUser)
            {
                return aUser;
            }
            bool aSym = HasNonHanzi(a->word);
            bool bSym = HasNonHanzi(b->word);
            if (aSym != bSym)
            {
                return aSym;
            }
            return EffectiveFreq(*a) > EffectiveFreq(*b);
        });
    if (matched.size() > MAX_RESULTS)
    {
        matched.resize(MAX_RESULTS);
    }
    FillResults(matched, pItemList);
}

//+---------------------------------------------------------------------------
//
// CollectWordByMixed — 混合简拼/全拼匹配（搜狗式）
//
// 输入串按音节切分（每音节可为完整拼音或声母），与词库词条逐音节前缀匹配：
//   "nhao" → [n, hao] 命中 "nihao"→你好（n 是 ni 的声母）
//   "fuxl" → [fu, xl]? 切分为 [fu, x, l]，命中 "fuxingliang" 风格的词
// 词库无词时降级为按音节拼单字（人名/生僻词兜底，如 fuxingliang→傅兴亮）。
//
//----------------------------------------------------------------------------

void CPinyinEngine::CollectWordByMixed(_In_ CStringRange *pKeyCode, _Inout_ CSampleImeArray<CCandidateListItem> *pItemList)
{
    if (!pKeyCode || !pItemList || _syllableTable.empty())
    {
        return;
    }

    std::wstring key(pKeyCode->Get(), pKeyCode->GetLength());
    size_t star = key.find(L'*');
    if (star != std::wstring::npos)
    {
        key = key.substr(0, star);
    }
    if (key.empty())
    {
        return;
    }

    // 输入串切分音节：完整拼音用最优切分（ganganganpin→[gan,gan,gan,pin]→gggp），
    // 含声母输入（fuxl）无完整切分时回退贪心（fu,x,l）。
    std::vector<std::wstring> inSyls = SegmentToSyllablesBest(key);
    if (inSyls.empty())
    {
        return;
    }
    size_t k = inSyls.size();
    if (k < 2 || k > 5)
    {
        return;   // 混合匹配限 2-5 音节
    }

    // 声母串 = 每音节声母首字母（zh/ch/sh → z/c/s），与词库 initial 列同构。
    // 混合匹配等价于"词条简拼 == 输入声母串"（声母相同即用户意图），因此直接走
    // _initialEntries 精确二分（O(log n)）——替代原实现"遍历首音节前缀区间 + 每条
    // SegmentPinyin 切分"（86.9 万条词库下 fuxl 类输入实测 130ms+，卡顿主因）。
    std::wstring initials;
    initials.reserve(k);
    for (const std::wstring &s : inSyls)
    {
        if (s.compare(0, 2, L"zh") == 0) initials += L'z';
        else if (s.compare(0, 2, L"ch") == 0) initials += L'c';
        else if (s.compare(0, 2, L"sh") == 0) initials += L's';
        else initials += s[0];
    }

    auto lo = std::lower_bound(_initialEntries.begin(), _initialEntries.end(), initials,
        [this](size_t i, const std::wstring &p) { return _entries[i].initial < p; });
    auto hi = std::upper_bound(lo, _initialEntries.end(), initials,
        [this](const std::wstring &p, size_t i) { return p < _entries[i].initial; });

    // 同 initial 命中通常很少；仍做逐音节前缀校验，剔除声母相同但音节不匹配的词
    // （如输入 "fuxl" 会命中 initial=="fxl" 的 "fengxianling"，首音节不是 fu）。
    // 校验先走贪心切分（快），失败时用最优切分重试一次——覆盖切分歧义词条
    // （如"敢干敢拼"，贪心切 [gang,ang,an,pin] 与输入的 [gan,gan,gan,pin] 不匹配）。
    std::vector<const Entry*> matched;
    for (auto it = lo; it != hi; ++it)
    {
        const Entry *e = &_entries[*it];
        std::vector<std::wstring> eSyls;
        bool ok = false;
        if (SegmentPinyin(_syllableTable, e->pinyin, eSyls) && eSyls.size() == k)
        {
            ok = true;
            for (size_t j = 0; j < k; j++)
            {
                if (eSyls[j].compare(0, inSyls[j].size(), inSyls[j]) != 0)
                {
                    ok = false;
                    break;
                }
            }
        }
        if (!ok)
        {
            // 贪心失败：若首音节本身不匹配（声母相同但首音节不同，如输入 fu 开头
            // 却命中 fengxianling），Best 重切结果相同，直接跳过；
            // 否则是切分歧义（如"敢干敢拼"，贪心 [gang,ang,an,pin] 与输入
            // [gan,gan,gan,pin] 中间错位），Best 重切一次可救活。
            if (eSyls.empty() || eSyls[0].compare(0, inSyls[0].size(), inSyls[0]) != 0)
            {
                continue;
            }
            eSyls = SegmentToSyllablesBest(e->pinyin);
            if (eSyls.size() == k)
            {
                ok = true;
                for (size_t j = 0; j < k; j++)
                {
                    if (eSyls[j].compare(0, inSyls[j].size(), inSyls[j]) != 0)
                    {
                        ok = false;
                        break;
                    }
                }
            }
        }
        if (ok)
        {
            matched.push_back(e);
            if (matched.size() >= MAX_RESULTS)
            {
                break;
            }
        }
    }

    if (matched.empty())
    {
        // 词库无匹配 → 按音节拼单字（人名/生僻组合兜底）
        CollectBySingleChars(inSyls, pItemList);
        return;
    }

    // 同 initial 内：用户词优先（用户造词/自学习）——否则低词频用户词
    // （如"傅兴亮"freq=2）会被主词库高频词（"复兴路/负心郎"）压出候选，
    // 混合简拼永远打不出来；组内按词频降序（降权词 EffectiveFreq=-1 → 沉底）。
    std::sort(matched.begin(), matched.end(),
        [this](const Entry *a, const Entry *b)
        {
            bool aUser = a->isUser;
            bool bUser = b->isUser;
            if (aUser != bUser)
            {
                return aUser;
            }
            return EffectiveFreq(*a) > EffectiveFreq(*b);
        });
    FillResults(matched, pItemList);
}

//+---------------------------------------------------------------------------
//
// CollectSingleCharsByKey — 长输入补充组词（人名兜底）
//
// 与 CollectWordByMixed 不同：不查词库词条，直接按音节拼单字，
// 供长输入（≥3 音节）时在已有候选之外补充一组组词候选。
//
//----------------------------------------------------------------------------

void CPinyinEngine::CollectSingleCharsByKey(_In_ CStringRange *pKeyCode, _Inout_ CSampleImeArray<CCandidateListItem> *pItemList)
{
    if (!pKeyCode || !pItemList || _syllableTable.empty())
    {
        return;
    }

    std::wstring key(pKeyCode->Get(), pKeyCode->GetLength());
    size_t star = key.find(L'*');
    if (star != std::wstring::npos)
    {
        key = key.substr(0, star);
    }
    if (key.empty())
    {
        return;
    }

    std::vector<std::wstring> syls = SegmentToSyllablesBest(key);
    if (syls.size() < 2)
    {
        return;
    }
    CollectBySingleChars(syls, pItemList);
}

//+---------------------------------------------------------------------------
//
// SegmentToSyllables — 造词入口切分（完整音节全切分 + 打分，最优切分）
//
//----------------------------------------------------------------------------

std::vector<std::wstring> CPinyinEngine::SegmentToSyllables(_In_ const std::wstring &key) const
{
    return SegmentToSyllablesBest(key);
}

//+---------------------------------------------------------------------------
//
// SegmentToSyllablesBest — 完整音节全切分 + 打分，返回最优切分
//
// SegmentPinyin 贪心只取第一个成功切分（完整音节按长度降序优先），导致
// `ganganganpin` 切为 [gang,ang,an,pin]（刚昂安拼）——造词选不出"敢干敢拼"。
// 本方法枚举全部"完整音节"切分，按打分选最优：
//   - 零声母韵母音节（an/ang/ai/ao/e/ei/en/eng/er/o/ou）作为词部件罕见 → 重罚
//   - 音节内单字最高词频 → 加分（log 压缩量级）
// `ganganganpin` 的 4 种合法切分中 [gan,gan,gan,pin]（0 零声母音节）得分最高。
// 无完整音节切分（输入含声母/生僻音）→ 回退贪心 SegmentPinyin。
//
//----------------------------------------------------------------------------

std::vector<std::wstring> CPinyinEngine::SegmentToSyllablesBest(_In_ const std::wstring &key) const
{
    std::vector<std::wstring> fallback;
    if (key.empty() || _syllableTable.empty())
    {
        return fallback;
    }

    // 1. 枚举全部完整音节切分（递归内自带剪枝：枚举上限 + 音节数上限）
    std::vector<std::vector<std::wstring>> all;
    std::vector<std::wstring> cur;
    EnumerateFullSyllables(_syllableTable, key, 0, cur, all);
    if (all.empty())
    {
        SegmentPinyin(_syllableTable, key, fallback);
        return fallback;
    }

    // 2. 打分选最优
    size_t bestIdx = 0;
    double bestScore = ScoreSegmentation(all[0]);
    for (size_t i = 1; i < all.size(); i++)
    {
        double sc = ScoreSegmentation(all[i]);
        if (sc > bestScore)
        {
            bestScore = sc;
            bestIdx = i;
        }
    }
    return all[bestIdx];
}

// 零声母韵母音节（作为词的一部分较罕见，打分重罚）
bool CPinyinEngine::IsZeroInitialSyllable(_In_ const std::wstring &s)
{
    static const wchar_t* ZERO_INITIALS[] = {
        L"a", L"ai", L"an", L"ang", L"ao", L"e", L"ei", L"en", L"eng", L"er", L"o", L"ou"
    };
    for (const wchar_t* z : ZERO_INITIALS)
    {
        if (s == z)
        {
            return true;
        }
    }
    return false;
}

// 枚举全部完整音节切分（DFS）
void CPinyinEngine::EnumerateFullSyllables(_In_ const std::vector<std::wstring> &sylTable, _In_ const std::wstring &s,
    size_t pos, _Inout_ std::vector<std::wstring> &cur, _Inout_ std::vector<std::vector<std::wstring>> &all)
{
    if (all.size() >= 512)
    {
        return;   // 枚举上限，防极端输入组合爆炸（正常造词输入远小于此）
    }
    if (pos == s.size())
    {
        all.push_back(cur);
        return;
    }
    for (const std::wstring &syl : sylTable)
    {
        if (pos + syl.size() <= s.size() && s.compare(pos, syl.size(), syl) == 0)
        {
            if (cur.size() >= 12)
            {
                continue;   // 音节数上限（每音节至少 1 字符，12 音节已远超实际词长）
            }
            cur.push_back(syl);
            EnumerateFullSyllables(sylTable, s, pos + syl.size(), cur, all);
            cur.pop_back();
        }
    }
}

// 切分打分（2026-08-13 修正）：启发式
//   真实词平均 2-4 音节，音节越多越可疑 → 每多 1 音节扣 10 分；
//   零声母音节（an/ang/ai 等）作为词部件罕见 → 每音节扣 12 分；
//   音节内单字最高词频（log 压缩）作为同音节数时的排序依据。
//   若只累加 log(freq)，多音节切分（如 zhongguo→[zhong,gu,o]）会因音节数多而
//   虚高分数，压过正确切分（[zhong,guo]）——必须按音节数扣分平衡。
double CPinyinEngine::ScoreSegmentation(_In_ const std::vector<std::wstring> &syls) const
{
    double score = 0.0;
    for (const std::wstring &s : syls)
    {
        if (IsZeroInitialSyllable(s))
        {
            score -= 12.0;
        }
        auto it = _singleCharBySyllable.find(s);
        if (it != _singleCharBySyllable.end() && !it->second.empty())
        {
            score += std::log(it->second[0].freq);
        }
    }
    score -= static_cast<double>(syls.size() - 1) * 10.0;
    return score;
}

//+---------------------------------------------------------------------------
//
// CollectSyllableChars — 把某音节的全部单字追加为候选（造词分段用）
//
//----------------------------------------------------------------------------

void CPinyinEngine::CollectSyllableChars(_In_ const std::wstring &syl, _Inout_ CSampleImeArray<CCandidateListItem> *pItemList)
{
    if (syl.empty() || pItemList == nullptr)
    {
        return;
    }
    auto it = _singleCharBySyllable.find(syl);
    if (it == _singleCharBySyllable.end() || it->second.empty())
    {
        return;
    }
    CollectSingleSyllable(syl, it->second, pItemList);
}

//+---------------------------------------------------------------------------
//
// CollectBySingleChars — 按音节拼单字（人名/生僻词兜底）
//
// 每个音节用该音节全部单字做笛卡尔组合（低频姓氏字如"傅"也能进入候选），
// 每轮用 beam 裁剪（保留权重最高的 100 个）控制规模，最后按词频乘积降序
// 输出前 MAX_RESULTS 个。候选 _FindKeyCode 用完整拼音拼接，便于自学习提升。
//
//----------------------------------------------------------------------------

void CPinyinEngine::CollectBySingleChars(_In_ const std::vector<std::wstring> &syls, _Inout_ CSampleImeArray<CCandidateListItem> *pItemList)
{
    if (syls.empty() || pItemList == nullptr)
    {
        return;
    }

    // 每音节全部单字（按词频降序，已由 _entries 排序保证）
    std::vector<std::vector<SingleChar>> cand;
    for (const std::wstring &s : syls)
    {
        auto it = _singleCharBySyllable.find(s);
        if (it == _singleCharBySyllable.end() || it->second.empty())
        {
            return;   // 任一音节无单字则放弃
        }
        cand.push_back(it->second);
    }

    const size_t BEAM = 100;   // 每轮裁剪规模

    // 笛卡尔组合（权重 = 词频乘积），beam 裁剪防爆炸
    struct Cand
    {
        std::wstring word;
        float        score;
    };
    std::vector<Cand> combos;
    combos.push_back(Cand{ L"", 1.0f });
    for (const auto &ch : cand)
    {
        std::vector<Cand> next;
        next.reserve(combos.size() * ch.size());
        for (const auto &c : combos)
        {
            for (const auto &sc : ch)
            {
                next.push_back(Cand{ c.word + sc.word, c.score * sc.freq });
            }
        }
        combos = std::move(next);
        if (combos.size() > BEAM)
        {
            std::partial_sort(combos.begin(), combos.begin() + BEAM, combos.end(),
                [](const Cand &a, const Cand &b) { return a.score > b.score; });
            combos.resize(BEAM);
        }
    }

    std::sort(combos.begin(), combos.end(),
        [](const Cand &a, const Cand &b) { return a.score > b.score; });

    // 完整拼音拼接（候选 key，供自学习提升）
    std::wstring fullKey;
    for (const std::wstring &s : syls)
    {
        fullKey += s;
    }

    size_t limit = (std::min)(combos.size(), static_cast<size_t>(MAX_RESULTS));

    // 完整拼音 key 进字符串池（deque 地址稳定），候选 _FindKeyCode 引用池中内存，
    // 避免持有局部 combos 的悬垂指针（此前会偶发崩溃，独立引擎进程中更明显）
    _sentencePool.push_back(fullKey);
    const std::wstring& stableKey = _sentencePool.back();

    for (size_t i = 0; i < limit; i++)
    {
        // 与已有候选去重（整句预测可能已输出相同字面，如"复兴亮"）
        bool dup = false;
        for (UINT k = 0; k < pItemList->Count(); k++)
        {
            const CCandidateListItem *existing = pItemList->GetAt(k);
            if (existing->_ItemString.GetLength() == combos[i].word.size() &&
                wcsncmp(existing->_ItemString.Get(), combos[i].word.c_str(), combos[i].word.size()) == 0)
            {
                dup = true;
                break;
            }
        }
        if (dup)
        {
            continue;
        }

        CCandidateListItem *pLI = pItemList->Append();
        if (!pLI)
        {
            break;
        }
        // word 同样进池后引用，杜绝悬垂
        _sentencePool.push_back(combos[i].word);
        const std::wstring& stableWord = _sentencePool.back();
        pLI->_ItemString.Set(stableWord.c_str(), static_cast<DWORD_PTR>(stableWord.size()));
        pLI->_FindKeyCode.Set(stableKey.c_str(), static_cast<DWORD_PTR>(stableKey.size()));
    }
}

//+---------------------------------------------------------------------------
//
// CollectWordFromConvertedStringForWildcard — 汉字前缀匹配（造词模式）
//
//----------------------------------------------------------------------------

void CPinyinEngine::CollectWordFromConvertedStringForWildcard(_In_ CStringRange *pString, _Inout_ CSampleImeArray<CCandidateListItem> *pItemList)
{
    if (!pString || !pItemList)
    {
        return;
    }

    std::wstring pattern(pString->Get(), pString->GetLength());

    size_t star = pattern.find(L'*');
    if (star != std::wstring::npos)
    {
        pattern = pattern.substr(0, star);
    }
    if (pattern.empty())
    {
        return;
    }

    std::vector<const Entry*> matched;
    for (const Entry &e : _entries)
    {
        if (e.word.compare(0, pattern.size(), pattern) == 0)
        {
            matched.push_back(&e);
        }
    }

    std::sort(matched.begin(), matched.end(),
        [](const Entry *a, const Entry *b) { return a->freq > b->freq; });
    if (matched.size() > MAX_RESULTS)
    {
        matched.resize(MAX_RESULTS);
    }
    FillResults(matched, pItemList);
}

//+---------------------------------------------------------------------------
//
// GetWordsForPinyin — 精确拼音查词，返回按词频降序的前 maxW 个
//
//----------------------------------------------------------------------------

void CPinyinEngine::GetWordsForPinyin(const std::wstring &py, size_t maxW, _Out_ std::vector<const Entry*> &out) const
{
    out.clear();
    if (py.empty())
    {
        return;
    }

    auto lo = std::lower_bound(_entries.begin(), _entries.end(), py,
        [](const Entry &e, const std::wstring &p) { return e.pinyin < p; });
    auto hi = std::upper_bound(lo, _entries.end(), py,
        [](const std::wstring &p, const Entry &e) { return p < e.pinyin; });

    for (auto it = lo; it != hi && out.size() < maxW; ++it)
    {
        out.push_back(&(*it));
    }
}

//+---------------------------------------------------------------------------
//
// CollectWordFuzzy — 模糊音匹配（平翘舌 / 前后鼻音等）
//
// 用户输入全拼但结果不足时启用：对输入拼音做模糊展开生成变体
// （如 zhan↔zan、yin↔ying、an↔ang），用变体前缀匹配词库并合并去重。
// 变体数量受限（每条规则至多 1 个），防止组合爆炸拖慢响应。
//
//----------------------------------------------------------------------------

struct FuzzyRule
{
    const wchar_t* from;
    const wchar_t* to;
    bool finalOnly;   // 韵母类规则仅允许词尾替换（避免破坏音节边界）
};

// 常见模糊音规则（双向）。
// 声母类(zh/z、ch/c、sh/s、l/n、f/h、r/l)任意位置替换安全；
// 韵母类(an/ang、en/eng、in/ing…)仅词尾替换，防止 "yixing" 之类把中部的
// "in" 误换导致非法拼音。
static const FuzzyRule kFuzzyRules[] = {
    { L"zh", L"z", false }, { L"z", L"zh", false },
    { L"ch", L"c", false }, { L"c", L"ch", false },
    { L"sh", L"s", false }, { L"s", L"sh", false },
    { L"l", L"n", false },  { L"n", L"l", false },
    { L"f", L"h", false },  { L"h", L"f", false },
    { L"r", L"l", false },  { L"l", L"r", false },
    { L"an", L"ang", true }, { L"ang", L"an", true },
    { L"en", L"eng", true }, { L"eng", L"en", true },
    { L"in", L"ing", true }, { L"ing", L"in", true },
    { L"ian", L"iang", true }, { L"iang", L"ian", true },
    { L"uan", L"uang", true }, { L"uang", L"uan", true },
};

void CPinyinEngine::CollectWordFuzzy(_In_ CStringRange *pKeyCode, _Inout_ CSampleImeArray<CCandidateListItem> *pItemList)
{
    if (!pKeyCode || !pItemList || _entries.empty())
    {
        return;
    }

    std::wstring key(pKeyCode->Get(), pKeyCode->GetLength());
    size_t star = key.find(L'*');
    if (star != std::wstring::npos)
    {
        key = key.substr(0, star);
    }
    if (key.size() < 2)
    {
        return;
    }

    // 生成变体：每条规则只替换第一处匹配，避免组合爆炸；
    // 韵母类规则要求替换点直达词尾（finalOnly），保证变体仍是合法拼音串。
    std::vector<std::wstring> variants;
    variants.push_back(key);
    for (const FuzzyRule &r : kFuzzyRules)
    {
        size_t flen = wcslen(r.from);
        size_t pos = key.find(r.from);
        if (pos == std::wstring::npos)
        {
            continue;
        }
        if (r.finalOnly && pos + flen != key.size())
        {
            continue;   // 非词尾的韵母替换会破坏音节边界，跳过
        }
        std::wstring v = key;
        v.replace(pos, flen, r.to);
        if (v != key)
        {
            variants.push_back(std::move(v));
        }
    }

    // 各变体前缀匹配，按词合并去重
    std::vector<const Entry*> matched;
    for (const std::wstring &v : variants)
    {
        std::vector<const Entry*> m;
        SearchPrefix(v, m);
        for (const Entry *e : m)
        {
            bool dup = false;
            for (const Entry *x : matched)
            {
                if (x->pinyin == e->pinyin && x->word == e->word)
                {
                    dup = true;
                    break;
                }
            }
            if (!dup)
            {
                matched.push_back(e);
                if (matched.size() >= MAX_RESULTS)
                {
                    break;
                }
            }
        }
        if (matched.size() >= MAX_RESULTS)
        {
            break;
        }
    }

    if (matched.empty())
    {
        return;
    }

    std::sort(matched.begin(), matched.end(),
        [](const Entry *a, const Entry *b) { return a->freq > b->freq; });
    FillResults(matched, pItemList);
}

//+---------------------------------------------------------------------------
//
// CollectSentence — 整句预测（Viterbi 束搜索）
//
// 将拼音串切分为合法音节序列，用词频（-log）作为代价做动态规划，
// 输出代价最低的若干整句作为候选（置于词候选之前）。
//
//----------------------------------------------------------------------------

void CPinyinEngine::CollectSentence(_In_ CStringRange *pKeyCode, _Inout_ CSampleImeArray<CCandidateListItem> *pItemList)
{
    if (!pKeyCode || !pItemList)
    {
        return;
    }

    _sentencePool.clear();   // 使上一轮候选的悬垂指针失效（上一轮候选已被消费）

    const std::wstring s(pKeyCode->Get(), pKeyCode->GetLength());

    // ---- 整句预测已禁用（2026-08-13）----
    // 用户反馈：输入 che 按数字 1 会直接选中整句"车到山前自有路"，干扰正常
    // 打字速度。整句预测属于"智能联想"，在基础词库下容易喧宾夺主。
    // 如需恢复：删除下面这行 return 即可（保留完整 Viterbi 实现）。
    return;
    // ------------------------------------

    if (s.size() < 2)
    {
        return;   // 单音节无需整句
    }

    const size_t n = s.size();
    const size_t K = 3;          // 束宽
    const size_t W = 3;          // 每个词条取前 W 个词
    const size_t MAX_KEY = 30;   // 最长词条拼音（约 10 个汉字）

    std::vector<std::vector<DpState>> dp(n + 1);
    DpState init = { 0.0f, L"" };
    dp[0].push_back(init);

    for (size_t i = 1; i <= n; i++)
    {
        // 尝试所有可能的切分起点 j：子串 s[j..i) 直接作为词条 key（任意音节数）
        size_t start = (i > MAX_KEY) ? (i - MAX_KEY) : 0;
        for (size_t j = start; j < i; j++)
        {
            if (dp[j].empty())
            {
                continue;
            }
            std::wstring key = s.substr(j, i - j);

            // 只允许合法完整音节组合作为词条 key：用户词库的简拼/畸形词条
            // （如 z→钟婷、zt→钟婷天气）不得参与整句预测，否则会拼出
            // 怪异整句（zting → "钟婷"+"听"）。
            if (!IsValidFullPinyin(key))
            {
                continue;
            }

            std::vector<const Entry*> words;
            GetWordsForPinyin(key, W, words);
            if (words.empty())
            {
                continue;
            }

            for (const DpState &st : dp[j])
            {
                for (const Entry *w : words)
                {
                    DpState ns;
                    ns.cost = st.cost - std::log(w->freq > 0.0f ? w->freq : 0.01f) + 5.0f;   // 每词分割惩罚
                    ns.sentence = st.sentence + w->word;
                    dp[i].push_back(ns);
                }
            }
        }

        // 束剪枝：每位置只保留代价最低的 K 个状态
        if (dp[i].size() > K)
        {
            std::partial_sort(dp[i].begin(), dp[i].end(), dp[i].begin() + K,
                [](const DpState &a, const DpState &b) { return a.cost < b.cost; });
            dp[i].resize(K);
        }
    }

    if (dp[n].empty())
    {
        return;
    }

    std::sort(dp[n].begin(), dp[n].end(),
        [](const DpState &a, const DpState &b) { return a.cost < b.cost; });

    for (const DpState &st : dp[n])
    {
        if (st.sentence.empty())
        {
            continue;
        }
        CCandidateListItem *pLI = pItemList->Append();
        if (!pLI)
        {
            break;
        }
        // 拷贝到成员池，保证指针在候选列表消费期间稳定
        _sentencePool.push_back(st.sentence);
        const std::wstring &poolStr = _sentencePool.back();
        pLI->_ItemString.Set(poolStr.c_str(), static_cast<DWORD_PTR>(poolStr.size()));
        pLI->_FindKeyCode.Set(pKeyCode->Get(), pKeyCode->GetLength());
    }
}

//+---------------------------------------------------------------------------
//
// IsValidUserRow — 用户词库行合法性校验
//
// 防畸形/乱码垃圾词条（历史遗留：asdjf→asdjf、超长乱码、fxl→发现了 等）：
//   - pinyin 仅小写字母 a-z，长度 1-30（完整拼音或简拼 key 都只含字母）
//   - word 长度 1-20，且含至少一个非 ASCII 字符（汉字/符号/emoji）
//   - freq 必须 > 0
// 纯 ASCII 字母乱码词条（word 无任何非 ASCII 内容）→ 判定无效。
//
//----------------------------------------------------------------------------

static bool IsValidUserRow(const std::wstring &pinyin, const std::wstring &word, float freq)
{
    if (pinyin.empty() || pinyin.size() > 30)
    {
        return false;
    }
    for (wchar_t c : pinyin)
    {
        if (c < L'a' || c > L'z')
        {
            return false;
        }
    }
    if (word.empty() || word.size() > 20)
    {
        return false;
    }
    if (!(freq > 0.0f))
    {
        return false;
    }
    for (wchar_t c : word)
    {
        if (static_cast<unsigned>(c) > 127)
        {
            return true;   // 汉字/符号/emoji 等非 ASCII 内容
        }
    }
    return false;   // 纯 ASCII（乱码）
}

//+---------------------------------------------------------------------------
//
// LoadUserDict — 加载用户词库（userdict.txt），合并进内存词表
//
// 用户词库格式同主词库：pinyin\tword\tfreq
// 合并规则：主词库已有同 (pinyin, word) 的条目 → 词频累加；
//           没有的 → 追加新条目（用户自学习词）。
// 合并完成后按 (pinyin, -freq) 重新排序，保证二分前缀查找有效。
//
//----------------------------------------------------------------------------

BOOL CPinyinEngine::LoadUserDict()
{
    // 支持重复加载（ReloadAll 热重载）：先清空用户加成，避免累加翻倍
    _userFreq.clear();
    _userInitial.clear();

    if (_userDictPath.empty())
    {
        return true;
    }

    HANDLE hFile = CreateFileW(_userDictPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        return true;   // 无用户词库（首次使用）不是错误
    }

    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart == 0 || fileSize.QuadPart > 64 * 1024 * 1024)
    {
        CloseHandle(hFile);
        return true;
    }

    DWORD bufSize = static_cast<DWORD>(fileSize.QuadPart);
    std::vector<char> buf(bufSize);
    DWORD bytesRead = 0;
    if (!ReadFile(hFile, buf.data(), bufSize, &bytesRead, nullptr) || bytesRead == 0)
    {
        CloseHandle(hFile);
        return true;
    }
    CloseHandle(hFile);

    int wcharLen = MultiByteToWideChar(CP_UTF8, 0, buf.data(), static_cast<int>(bytesRead), nullptr, 0);
    if (wcharLen <= 0)
    {
        return true;
    }
    std::wstring text(wcharLen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, buf.data(), static_cast<int>(bytesRead), &text[0], wcharLen);

    size_t pos = 0;
    const size_t textLen = text.size();
    while (pos < textLen)
    {
        size_t nl = text.find(L'\n', pos);
        std::wstring line;
        if (nl == std::wstring::npos)
        {
            line.assign(text, pos, textLen - pos);
            pos = textLen;
        }
        else
        {
            line.assign(text, pos, nl - pos);
            pos = nl + 1;
        }

        if (!line.empty() && line.back() == L'\r')
        {
            line.pop_back();
        }
        if (line.empty())
        {
            continue;
        }

        size_t t1 = line.find(L'\t');
        if (t1 == std::wstring::npos)
        {
            continue;
        }
        size_t t2 = line.find(L'\t', t1 + 1);
        if (t2 == std::wstring::npos)
        {
            continue;
        }

        std::wstring pinyin = line.substr(0, t1);
        std::wstring word = line.substr(t1 + 1, t2 - t1 - 1);
        if (pinyin.empty() || word.empty())
        {
            continue;
        }
        float freq = static_cast<float>(_wtof(line.substr(t2 + 1).c_str()));

        // 行合法性校验（防畸形/乱码垃圾词条）：pinyin 仅小写字母 1-30；
        // word 1-20 且含非 ASCII 内容（汉字/符号/emoji，排除 asdjf→asdjf 类纯字母乱码）。
        // 畸形行跳过（SaveUserDict 只写有效条目，下次写盘自动清理文件）。
        if (!IsValidUserRow(pinyin, word, freq))
        {
            CPinyinIpc::DebugLog(L"LoadUserDict skip invalid: %ls", line.c_str());
            continue;
        }

        // 可选第 4 列：简拼（造词条目才有）
        std::wstring initial;
        size_t t3 = line.find(L'\t', t2 + 1);
        if (t3 != std::wstring::npos)
        {
            initial = line.substr(t3 + 1);
        }

        _userFreq[std::make_pair(pinyin, word)] += freq;
        if (!initial.empty())
        {
            _userInitial[std::make_pair(pinyin, word)] = initial;
        }

        // 合并进内存词表（二分定位拼音区间——原线性扫描 88 万条 × 用户词数，
        // 在 CPU 满载下 ~6s；二分 <1ms）
        bool found = false;
        auto lo = std::lower_bound(_entries.begin(), _entries.end(), pinyin,
            [](const Entry &e, const std::wstring &p) { return e.pinyin < p; });
        auto hi = std::upper_bound(lo, _entries.end(), pinyin,
            [](const std::wstring &p, const Entry &e) { return p < e.pinyin; });
        for (auto it = lo; it != hi; ++it)
        {
            if (it->word == word)
            {
                it->freq += freq;
                if (it->initial.empty() && !initial.empty())
                {
                    it->initial = initial;
                }
                found = true;
                break;
            }
        }
        if (!found)
        {
            Entry entry;
            entry.pinyin = pinyin;
            entry.word = word;
            entry.freq = freq;
            entry.initial = initial;
            entry.isUser = true;   // 用户词库独有条目：删除功能可删
            _entries.push_back(std::move(entry));
        }
    }

    // 保持不变量：主词库段（缓存/解析读入）已按 (pinyin, freq) 有序；
    // 仅用户词新增段排序后线性归并——替代 88 万条全量 sort（CPU 满载下 ~1.2s，
    // 归并 O(n) <0.2s）。注意：用户词命中主词库词条 freq+1 后该词在区间内位置
    // 不前移（轻微顺序不完美，下次全量重建纠正；不影响查询可达性）。
    const size_t mainBase = _mainDictEntries;
    if (_entries.size() > mainBase)
    {
        auto entryCmp = [](const Entry &a, const Entry &b)
        {
            int cmp = a.pinyin.compare(b.pinyin);
            if (cmp != 0)
            {
                return cmp < 0;
            }
            return a.freq > b.freq;
        };
        std::sort(_entries.begin() + static_cast<ptrdiff_t>(mainBase), _entries.end(), entryCmp);
        std::inplace_merge(_entries.begin(), _entries.begin() + static_cast<ptrdiff_t>(mainBase),
            _entries.end(), entryCmp);
    }
    return true;
}

//+---------------------------------------------------------------------------
//
// SaveUserDict — 将用户词频加成全量写回 userdict.txt（UTF-8）
//
// 采用独占打开 + 临时文件改名，避免与其他进程并发写冲突。
//
//----------------------------------------------------------------------------

void CPinyinEngine::SaveUserDict()
{
    if (_userDictPath.empty() || _userFreq.empty())
    {
        return;
    }

    // 构建 UTF-8 文本：pinyin\tword\tfreq\tinitial（第 4 列为简拼，造词条目才有）
    std::wstring text;
    for (const auto &kv : _userFreq)
    {
        text += kv.first.first + L"\t" + kv.first.second + L"\t";
        wchar_t num[32] = {0};
        swprintf_s(num, _countof(num), L"%.2f", kv.second);
        text += num;
        auto it = _userInitial.find(kv.first);
        if (it != _userInitial.end() && !it->second.empty())
        {
            text += L"\t" + it->second;
        }
        text += L"\r\n";
    }

    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (utf8Len <= 0)
    {
        return;
    }
    std::vector<char> utf8(utf8Len);
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), utf8.data(), utf8Len, nullptr, nullptr);

    // 临时文件 + 独占写入 + 改名
    std::wstring tmpPath = _userDictPath + L".tmp";
    HANDLE hFile = CreateFileW(tmpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        return;
    }

    DWORD written = 0;
    BOOL ok = WriteFile(hFile, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    CloseHandle(hFile);
    if (!ok || written != static_cast<DWORD>(utf8.size()))
    {
        DeleteFileW(tmpPath.c_str());
        return;
    }

    MoveFileExW(tmpPath.c_str(), _userDictPath.c_str(), MOVEFILE_REPLACE_EXISTING);
}

//+---------------------------------------------------------------------------
//
// IsLearningEnabled — 自学习开关
//
// 读取 bin\engine.conf 的 learn=0/1（设置面板写入）。每次实时读文件，
// 改配置后无需重启引擎立即生效；文件缺失/损坏默认开启。
//
//----------------------------------------------------------------------------

static bool IsLearningEnabled()
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, EnginePaths::DataFile(L"engine.conf").c_str(), L"r") != 0 || f == nullptr)
    {
        return true;   // 无配置文件 → 默认开启
    }
    char line[64];
    bool enabled = true;
    while (fgets(line, sizeof(line), f) != nullptr)
    {
        if (_strnicmp(line, "learn=", 6) == 0)
        {
            enabled = (line[6] == '1');
            break;
        }
    }
    fclose(f);
    return enabled;
}

//+---------------------------------------------------------------------------
//
// ResolveDictPath — 主词库路径解析（大字库模式，2026-08-13）
//
// 读取 bin\engine.conf 的 bigdict=0/1（设置面板"大字库模式"开关写入）。
//   0（默认）→ pinyin-plus.txt（仅 CJK 基本区常用字，88 万条）
//   1        → pinyin-plus-big.txt（+41448 大字表 4 万+ 生僻字，92 万条）
// 每次实时读文件：引擎启动（engine_main）与双缓冲热重载（EnginePipe
// TriggerReload）都调用本函数 —— 设置面板切换开关后触发 type 11 热重载，
// 后台线程按新路径重建实例并原子切换，打字零阻塞、引擎无需重启。
//
//----------------------------------------------------------------------------

std::wstring ResolveDictPath()
{
    bool big = false;
    FILE* f = nullptr;
    if (_wfopen_s(&f, EnginePaths::DataFile(L"engine.conf").c_str(), L"r") == 0 && f != nullptr)
    {
        char line[64];
        while (fgets(line, sizeof(line), f) != nullptr)
        {
            if (_strnicmp(line, "bigdict=", 8) == 0)
            {
                big = (line[8] == '1');
                break;
            }
        }
        fclose(f);
    }
    // 词库在安装目录（只读）：默认 pinyin-plus.txt，大字库模式 pinyin-plus-big.txt
    return big ? EnginePaths::InstallFile(L"pinyin-plus-big.txt")
               : EnginePaths::InstallFile(L"pinyin-plus.txt");
}

//+---------------------------------------------------------------------------
//
// BoostWord — 用户选词：词频+1 并持久化（自学习词库）
//
// 修复（2026-08-13）：词库不存在的词（组词/人名兜底产物，如"傅兴亮"）
// 原实现只加 _userFreq 落盘 3 列，不真正入库 → 本次会话全拼仍查不到、
// 重启后简拼/混合（_initialEntries）也查不到。现改为与 AddUserWord 相同：
// 立即插入 _entries（含简拼 initial、isUser=true），落盘带第 4 列简拼，
// 三种输入方式（全拼/简拼/混合）当场可用。
//
//----------------------------------------------------------------------------

void CPinyinEngine::BoostWord(_In_ const WCHAR *pinyin, size_t pinyinLen, _In_ const WCHAR *word, size_t wordLen)
{
    if (!pinyin || !word || pinyinLen == 0 || wordLen == 0)
    {
        return;
    }

    // 自学习开关（bin\engine.conf learn=0 关闭）：关掉后选词不再提升词频/写入用户词库。
    // 设置面板改配置后无需重启引擎（每次实时读取，文件仅几字节）。
    if (!IsLearningEnabled())
    {
        return;
    }

    std::wstring py(pinyin, pinyinLen);
    std::wstring wd(word, wordLen);

    // 1. 定位该拼音区间 + 查词是否已存在
    auto lo = std::lower_bound(_entries.begin(), _entries.end(), py,
        [](const Entry &e, const std::wstring &p) { return e.pinyin < p; });
    auto hi = std::upper_bound(lo, _entries.end(), py,
        [](const std::wstring &p, const Entry &e) { return p < e.pinyin; });

    bool found = false;
    std::wstring initial;
    for (auto it = lo; it != hi; ++it)
    {
        if (it->word == wd)
        {
            found = true;
            it->freq += 1.0f;
            initial = it->initial;
            break;
        }
    }
    _userFreq[std::make_pair(py, wd)] += 1.0f;

    if (!found)
    {
        // 2a. 词库无此词（组词/人名兜底产物）：立即入库，含简拼（最优切分取首字母）。
        //     插入该拼音区间 freq 降序位置（新词 freq=1 通常落区间尾部）。
        if (!_syllableTable.empty())
        {
            std::vector<std::wstring> syls = SegmentToSyllablesBest(py);
            for (const std::wstring &s : syls)
            {
                if (!s.empty())
                {
                    initial += s[0];
                }
            }
        }
        Entry entry;
        entry.pinyin = py;
        entry.word = wd;
        entry.initial = initial;
        entry.freq = _userFreq[std::make_pair(py, wd)];
        entry.isUser = true;   // 用户词：删除功能可删
        auto ins = std::upper_bound(lo, hi, entry,
            [](const Entry &a, const Entry &b) { return a.freq > b.freq; });
        auto it = _entries.insert(ins, std::move(entry));
        size_t entryIdx = static_cast<size_t>(it - _entries.begin());
        // ★ 与 AddUserWord 同款修复：insert 中间插入后平移 _initialEntries 中
        //   所有 >= entryIdx 的旧下标（元素后移一位），否则简拼/混合索引错位。
        for (size_t &idx : _initialEntries)
        {
            if (idx >= entryIdx)
            {
                idx++;
            }
        }
        if (!initial.empty())
        {
            _userInitial[std::make_pair(py, wd)] = initial;
            InsertIntoInitialIndex(entryIdx);
        }
    }
    else
    {
        // 2b. 已有词：重排该拼音区间，保证同拼音内词频降序（前缀查找依赖该序）
        std::sort(lo, hi,
            [](const Entry &a, const Entry &b) { return a.freq > b.freq; });

        // 3. 简拼索引局部重排同 initial 区间（freq 序可能被破坏；<1ms，替代全量重建）
        if (!initial.empty())
        {
            FixInitialIndexFreqOrder(initial);
        }
    }

    // 4. 持久化
    SaveUserDict();

    // 5. 索引一致性自检（异常自动重建）
    ValidateInitialIndex();
}

//+---------------------------------------------------------------------------
//
//+---------------------------------------------------------------------------
//
// InsertIntoInitialIndex — 简拼索引局部插入新词下标
//
// _initialEntries 按 (initial 升序, freq 降序) 排序。新词 freq=1 通常落在
// 该 initial 区间尾部；用二分定位插入点，避免全量 RebuildInitialIndex。
//
//----------------------------------------------------------------------------

void CPinyinEngine::InsertIntoInitialIndex(size_t idx)
{
    const Entry &e = _entries[idx];
    auto lo = std::lower_bound(_initialEntries.begin(), _initialEntries.end(), e.initial,
        [this](size_t i, const std::wstring &p) { return _entries[i].initial < p; });
    auto hi = std::upper_bound(lo, _initialEntries.end(), e.initial,
        [this](const std::wstring &p, size_t i) { return p < _entries[i].initial; });
    // freq 降序：第一个 freq <= 新词 freq 的位置即插入点
    auto pos = std::lower_bound(lo, hi, e.freq,
        [this](size_t i, float f) { return _entries[i].freq > f; });
    _initialEntries.insert(pos, idx);
}

//+---------------------------------------------------------------------------
//
// FixInitialIndexFreqOrder — 词频变化后局部重排同 initial 区间
//
// BoostWord/AddUserWord 对已有词 freq+1 后，同 initial 区间内 freq 降序可能
// 被破坏（该词可能前移）。只重排该 initial 区间（几百~几千项，<1ms），
// 替代全量 RebuildInitialIndex（86.9 万指针排序，~0.5s）。
//
//----------------------------------------------------------------------------

void CPinyinEngine::FixInitialIndexFreqOrder(const std::wstring &initial)
{
    if (initial.empty())
    {
        return;
    }
    auto lo = std::lower_bound(_initialEntries.begin(), _initialEntries.end(), initial,
        [this](size_t i, const std::wstring &p) { return _entries[i].initial < p; });
    auto hi = std::upper_bound(lo, _initialEntries.end(), initial,
        [this](const std::wstring &p, size_t i) { return p < _entries[i].initial; });
    if (lo != hi)
    {
        std::sort(lo, hi,
            [this](size_t a, size_t b) { return _entries[a].freq > _entries[b].freq; });
    }
}

//+---------------------------------------------------------------------------
//
// AddUserWord — 用户造词（分段选字完成的新词）入库
//
// 词库存在 (pinyin, word) 则词频+1；不存在则新增条目（含简拼 initial），
// 重建简拼索引并持久化。下次全拼/简拼均可直达。
//
// 性能（2026-08-13 词库扩到 86.9 万条后优化）：
//   原实现每次造词全量 std::sort(_entries) + RebuildInitialIndex（~1.5-3s，
//   且引擎锁内执行 → 造词期间打字卡死）。改为局部维护：
//     - 已有词：只重排该拼音区间 + FixInitialIndexFreqOrder
//     - 新词：vector::insert 到该拼音区间正确位置 + InsertIntoInitialIndex
//   平均 <1ms。
//
//----------------------------------------------------------------------------

void CPinyinEngine::AddUserWord(_In_ const WCHAR *pinyin, size_t pinyinLen, _In_ const WCHAR *word, size_t wordLen)
{
    if (!pinyin || !word || pinyinLen == 0 || wordLen == 0)
    {
        return;
    }

    // 自学习开关：关掉后造词不入库（防止实验/误操作积累垃圾词条）
    if (!IsLearningEnabled())
    {
        return;
    }

    std::wstring py(pinyin, pinyinLen);
    std::wstring wd(word, wordLen);

    // 计算简拼：最优切分取各音节首字母（如 敢干敢拼 → gggp；贪心切分会算成 gaap）
    std::wstring initial;
    if (!_syllableTable.empty())
    {
        std::vector<std::wstring> syls = SegmentToSyllablesBest(py);
        for (const std::wstring &s : syls)
        {
            if (!s.empty())
            {
                initial += s[0];
            }
        }
    }

    // 1. 定位该拼音区间（_entries 按 pinyin 升序，同拼音 freq 降序）
    auto lo = std::lower_bound(_entries.begin(), _entries.end(), py,
        [](const Entry &e, const std::wstring &p) { return e.pinyin < p; });
    auto hi = std::upper_bound(lo, _entries.end(), py,
        [](const std::wstring &p, const Entry &e) { return p < e.pinyin; });

    // 2. 查词是否已存在
    bool found = false;
    bool hadInitial = false;
    bool needRebuildIndex = false;
    for (auto it = lo; it != hi; ++it)
    {
        if (it->word == wd)
        {
            hadInitial = !it->initial.empty();
            it->freq += 1.0f;
            if (it->initial != initial)
            {
                // 简拼变更（切分算法改进后纠正旧简拼，如 gaap→gggp）
                it->initial = initial;
                if (hadInitial)
                {
                    needRebuildIndex = true;   // 旧简拼区间残留该下标，全量重建兜底（罕见，可接受）
                }
            }
            found = true;
            break;
        }
    }

    size_t entryIdx = _entries.size();
    if (!found)
    {
        // 新词：插入该拼音区间正确位置（区间内 freq 降序，新词 freq=1 通常靠后）。
        Entry entry;
        entry.pinyin = py;
        entry.word = wd;
        entry.initial = initial;
        entry.freq = 1.0f;
        entry.isUser = true;   // 用户造词：删除功能可删
        auto ins = std::upper_bound(lo, hi, entry,
            [](const Entry &a, const Entry &b) { return a.freq > b.freq; });
        auto it = _entries.insert(ins, std::move(entry));
        entryIdx = static_cast<size_t>(it - _entries.begin());

        // ★ 关键修复（2026-08-13）：insert 在中间插入后，[entryIdx+1, end) 的元素
        //   下标全部 +1（元素后移一位）。_initialEntries 存的是下标，若不同步平移，
        //   所有 >= entryIdx 的旧下标都会指向错误词条 → 简拼/混合索引错位
        //   （用户造词后 fxl 查不到傅兴亮，正是插入 f 区破坏了 f 区之后全部索引）。
        //   平移不改变 _initialEntries 的排序（这些下标对应元素相对顺序不变）。
        for (size_t &idx : _initialEntries)
        {
            if (idx >= entryIdx)
            {
                idx++;
            }
        }
    }
    else
    {
        // 已有词：freq+1 后该拼音区间内 freq 降序可能被破坏 → 局部重排（替代全量 sort）
        std::sort(lo, hi,
            [](const Entry &a, const Entry &b) { return a.freq > b.freq; });
        // 重定位该词下标（sort 交换元素后位置可能变）
        for (auto it = lo; it != hi; ++it)
        {
            if (it->word == wd)
            {
                entryIdx = static_cast<size_t>(it - _entries.begin());
                break;
            }
        }
    }

    _userFreq[std::make_pair(py, wd)] += 1.0f;
    if (!initial.empty())
    {
        _userInitial[std::make_pair(py, wd)] = initial;
    }

    // 3. 简拼索引局部维护（替代全量 RebuildInitialIndex）
    if (needRebuildIndex)
    {
        RebuildInitialIndex();   // 罕见：切分算法更新后纠正旧简拼，一次全量可接受
    }
    else if (!initial.empty())
    {
        if (!found || !hadInitial)
        {
            InsertIntoInitialIndex(entryIdx);
        }
        else
        {
            FixInitialIndexFreqOrder(initial);
        }
    }

    // 4. 持久化（仅写用户词条，快）
    SaveUserDict();

    // 5. 索引一致性自检（防 insert 平移/sort 交换漏步导致整体错位，异常自动重建）
    ValidateInitialIndex();

    CPinyinIpc::DebugLog(L"AddUserWord py=%s word=%s initial=%s", py.c_str(), wd.c_str(), initial.c_str());
}

//+---------------------------------------------------------------------------
//
// DeleteUserWordByWord — 删除用户词（候选模式 Ctrl+Delete）
//
// 按 word 删除 _userFreq 中有记录的全部用户条目（含多音），同步：
//   _entries 删除对应条目 → 全量重建简拼索引（删除低频，可接受）→
//   SaveUserDict 重写 userdict 文件。
// 词库自带词（_userFreq 无记录）不受影响；无匹配时返回 FALSE（静默忽略）。
//
//----------------------------------------------------------------------------

BOOL CPinyinEngine::DeleteUserWordByWord(_In_ const WCHAR *word, size_t wordLen)
{
    if (!word || wordLen == 0)
    {
        return FALSE;
    }
    std::wstring wd(word, wordLen);

    // 1. 收集要删除的 (pinyin, word) 键：仅限用户词（_userFreq 有记录）
    std::vector<std::pair<std::wstring, std::wstring>> toDelete;
    for (const auto &kv : _userFreq)
    {
        if (kv.first.second == wd)
        {
            toDelete.push_back(kv.first);
        }
    }
    if (toDelete.empty())
    {
        return FALSE;   // 非用户词（词库自带），静默忽略
    }

    // 2. 从 _entries 删除对应条目：仅删"用户造词/用户词库独有"条目（isUser），
    //    词库自带词（含被 BoostWord 提升过的，如符号 △）保留 —— 删除只是
    //    清掉用户造的错误词，绝不破坏主词库。
    _entries.erase(
        std::remove_if(_entries.begin(), _entries.end(),
            [&](const Entry &e)
            {
                return e.isUser && e.word == wd;
            }),
        _entries.end());

    // 3. 清理用户元数据 + 重建简拼索引（删除后下标失效）
    for (const auto &kv : toDelete)
    {
        _userFreq.erase(kv);
        _userInitial.erase(kv);
    }
    RebuildInitialIndex();

    // 4. 同步 userdict 文件（SaveUserDict 遍历 _userFreq，被删词条自然消失）
    SaveUserDict();

    // 5. 索引一致性自检（异常自动重建）
    ValidateInitialIndex();

    CPinyinIpc::DebugLog(L"DeleteUserWordByWord word=%s deleted=%u", wd.c_str(), (unsigned)toDelete.size());
    return TRUE;
}

//+---------------------------------------------------------------------------
//
// DemoteWord — 降权（候选模式 Ctrl+PageDown / 右键"降低排位"）
//
// 把词加入降权黑名单并持久化到 downweight.txt。排序比较器经 EffectiveFreq
// 把降权词 freq 当作 -1 → 强制沉底（候选超过 MAX_RESULTS 时被挤出第一屏）。
// 与自学习（freq+1）互不影响：用户偶尔选一次不会立即拉回前排，符合
// "我不想让这个词靠前"的意图；重复调用幂等。
//
//----------------------------------------------------------------------------

void CPinyinEngine::DemoteWord(_In_ const WCHAR *word, size_t wordLen)
{
    if (!word || wordLen == 0)
    {
        return;
    }
    std::wstring wd(word, wordLen);
    if (wd.find(L'\t') != std::wstring::npos || wd.find(L'\r') != std::wstring::npos ||
        wd.find(L'\n') != std::wstring::npos)
    {
        return;   // 防注入：含控制字符的脏词条不入黑名单
    }
    if (_downWords.insert(wd).second)
    {
        SaveDownWords();
        CPinyinIpc::DebugLog(L"DemoteWord word=%s", wd.c_str());
    }
}

BOOL CPinyinEngine::IsDownWord(_In_ const std::wstring &word) const
{
    return _downWords.count(word) != 0;
}

//+---------------------------------------------------------------------------
//
// LoadDownWords — 读降权黑名单（downweight.txt，UTF-8，一行一词）
//
//----------------------------------------------------------------------------

void CPinyinEngine::LoadDownWords()
{
    _downWords.clear();
    if (_downWordsPath.empty())
    {
        return;
    }
    HANDLE hFile = CreateFileW(_downWordsPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        return;   // 首次使用无文件，正常
    }
    LARGE_INTEGER size;
    if (!GetFileSizeEx(hFile, &size) || size.QuadPart <= 0 || size.QuadPart > 16 * 1024 * 1024)
    {
        CloseHandle(hFile);
        return;
    }
    std::vector<char> utf8(static_cast<size_t>(size.QuadPart) + 2);
    DWORD rd = 0;
    BOOL ok = ReadFile(hFile, utf8.data(), static_cast<DWORD>(size.QuadPart), &rd, nullptr);
    CloseHandle(hFile);
    if (!ok)
    {
        return;
    }
    utf8[rd] = utf8[rd + 1] = 0;

    // UTF-8 → 逐行拆词
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(rd), nullptr, 0);
    if (wlen <= 0)
    {
        return;
    }
    std::vector<wchar_t> wbuf(static_cast<size_t>(wlen) + 2);
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(rd), wbuf.data(), wlen);
    wbuf[wlen] = 0;

    std::wstring cur;
    for (const wchar_t *p = wbuf.data(); *p; ++p)
    {
        if (*p == L'\r' || *p == L'\n')
        {
            if (!cur.empty())
            {
                _downWords.insert(cur);
                cur.clear();
            }
        }
        else
        {
            cur += *p;
        }
    }
    if (!cur.empty())
    {
        _downWords.insert(cur);
    }
    CPinyinIpc::DebugLog(L"LoadDownWords count=%zu", _downWords.size());
}

//+---------------------------------------------------------------------------
//
// SaveDownWords — 持久化降权黑名单（downweight.txt，UTF-8，一行一词）
//
// 临时文件 + 独占写入 + 改名（与 SaveUserDict 同款，防并发写冲突）。
//
//----------------------------------------------------------------------------

void CPinyinEngine::SaveDownWords()
{
    if (_downWordsPath.empty() || _downWords.empty())
    {
        return;
    }
    std::wstring text;
    for (const std::wstring &w : _downWords)
    {
        text += w + L"\r\n";
    }
    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (utf8Len <= 0)
    {
        return;
    }
    std::vector<char> utf8(utf8Len);
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), utf8.data(), utf8Len, nullptr, nullptr);

    std::wstring tmpPath = _downWordsPath + L".tmp";
    HANDLE hFile = CreateFileW(tmpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        return;
    }
    DWORD written = 0;
    BOOL ok = WriteFile(hFile, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    CloseHandle(hFile);
    if (!ok || written != static_cast<DWORD>(utf8.size()))
    {
        DeleteFileW(tmpPath.c_str());
        return;
    }
    MoveFileExW(tmpPath.c_str(), _downWordsPath.c_str(), MOVEFILE_REPLACE_EXISTING);
}

//+---------------------------------------------------------------------------
//
// IsEntry — 精确查询 (pinyin, word) 是否在内存词库中
//
// 用于选词自学习时区分"完整拼音"与"简拼+剩余拼接"。
//
//----------------------------------------------------------------------------

BOOL CPinyinEngine::IsEntry(_In_ const std::wstring &pinyin, _In_ const std::wstring &word) const
{
    if (pinyin.empty() || word.empty())
    {
        return FALSE;
    }

    auto lo = std::lower_bound(_entries.begin(), _entries.end(), pinyin,
        [](const Entry &e, const std::wstring &p) { return e.pinyin < p; });
    auto hi = std::upper_bound(lo, _entries.end(), pinyin,
        [](const std::wstring &p, const Entry &e) { return p < e.pinyin; });

    for (auto it = lo; it != hi; ++it)
    {
        if (it->word == word)
        {
            return TRUE;
        }
    }
    return FALSE;
}
