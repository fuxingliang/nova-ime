//+---------------------------------------------------------------------------
//
//  PinyinEngine.h
//
//  Pinyin-Plus 拼音引擎：内存词库 + 二分前缀查找 + 词频排序
//
//  词库格式 (pinyin-plus.txt)：
//      拼音(小写,无声调,无空格)\t汉字词\t词频
//  按拼音字典序排列，同拼音内按词频降序（由 build_dict.py 生成）
//
//----------------------------------------------------------------------------

#pragma once

#include "SampleIMEBaseStructure.h"
#include <vector>
#include <deque>
#include <map>
#include <string>
#include <unordered_set>

class CPinyinEngine
{
public:
    CPinyinEngine();
    ~CPinyinEngine();

    // 加载词库文件（UTF-8）。失败返回 FALSE。
    BOOL Initialize(_In_z_ LPCWSTR pwszDictPath);
    BOOL IsAvailable() const { return !_entries.empty(); }

    // 当前主词库路径（双缓冲后台重建时用于构造新实例）
    _In_ const std::wstring& GetDictPath() const { return _dictPath; }

    // 词库热重载（主词库 + 用户词库全部重建）。导入词库后由管道消息触发。
    BOOL ReloadAll();

    // 精确拼音匹配（候选按词频降序）
    void CollectWord(_In_ CStringRange *pKeyCode, _Inout_ CSampleImeArray<CCandidateListItem> *pItemList);

    // 前缀匹配（pKeyCode 允许末尾带 '*' 通配符；内部 '*' 取前面部分）
    void CollectWordForWildcard(_In_ CStringRange *pKeyCode, _Inout_ CSampleImeArray<CCandidateListItem> *pItemList);

    // 汉字前缀匹配（造词模式：按已上屏汉字联想）
    void CollectWordFromConvertedStringForWildcard(_In_ CStringRange *pString, _Inout_ CSampleImeArray<CCandidateListItem> *pItemList);

    // 简拼（声母串）匹配：pKeyCode 为声母串（如 "nh"→你好）。
    // 用户输入不足全拼时,按简拼索引精确/前缀匹配。
    void CollectWordByInitial(_In_ CStringRange *pKeyCode, _Inout_ CSampleImeArray<CCandidateListItem> *pItemList);

    // 混合简拼/全拼匹配（搜狗式）：nhao→你好、fuxl→傅兴亮。
    // 输入串按音节切分（每音节可全拼或声母），与词库词条逐音节前缀匹配；
    // 词库无词时按音节拼单字组合（人名/生僻词兜底）。
    void CollectWordByMixed(_In_ CStringRange *pKeyCode, _Inout_ CSampleImeArray<CCandidateListItem> *pItemList);

    // 长输入补充（≥3 音节，疑似人名/长词）：无论是否已有候选，都按音节拼单字
    // 追加一组组词候选（搜狗式组词，低频姓氏字如"傅"也能组合出来，选词后自学习）。
    void CollectSingleCharsByKey(_In_ CStringRange *pKeyCode, _Inout_ CSampleImeArray<CCandidateListItem> *pItemList);

    // 模糊音匹配：全拼结果不足时，对输入拼音做模糊展开（平翘舌/前后鼻音等）
    // 再前缀匹配，合并结果。常见错误发音（z/zh、in/ing、an/ang…）也能命中。
    void CollectWordFuzzy(_In_ CStringRange *pKeyCode, _Inout_ CSampleImeArray<CCandidateListItem> *pItemList);

    // 将连续拼音串切分为音节序列（声母/全拼混合）。失败返回空向量。
    std::vector<std::wstring> SegmentToSyllables(_In_ const std::wstring &key) const;

    // 造词入口切分：完整音节全切分 + 打分，返回最优切分。
    // 解决贪心切分歧义（ganganganpin → [gan,gan,gan,pin] 而非 [gang,ang,an,pin]，
    // 后者造词选不出"敢干敢拼"）。无完整音节切分时回退贪心（含声母）。
    std::vector<std::wstring> SegmentToSyllablesBest(_In_ const std::wstring &key) const;

    // 造词用：把某音节的全部单字追加到候选列表（按词频降序）。
    void CollectSyllableChars(_In_ const std::wstring &syl, _Inout_ CSampleImeArray<CCandidateListItem> *pItemList);

    // 整句预测（Viterbi 束搜索）：将拼音串切分为音节并输出概率最高的整句候选
    void CollectSentence(_In_ CStringRange *pKeyCode, _Inout_ CSampleImeArray<CCandidateListItem> *pItemList);

    // 用户选词：提升 (pinyin, word) 词频并持久化到用户词库（userdict.txt）
    void BoostWord(_In_ const WCHAR *pinyin, size_t pinyinLen, _In_ const WCHAR *word, size_t wordLen);

    // 用户造词（分段选字完成的新词，如 fuxingliang→傅兴亮）：
    // 词库不存在则新增条目并持久化（含简拼），下次全拼/简拼均可直达。
    void AddUserWord(_In_ const WCHAR *pinyin, size_t pinyinLen, _In_ const WCHAR *word, size_t wordLen);

    // 删除用户词（候选模式 Ctrl+Delete）：按 word 删除 _userFreq 中有记录的全部
    // 用户条目（含多音），同步内存 _entries + 简拼索引 + userdict 文件。
    // 词库自带词（_userFreq 无记录）不受影响。返回是否删除了任何条目。
    BOOL DeleteUserWordByWord(_In_ const WCHAR *word, size_t wordLen);

    // 降权（候选模式 Ctrl+PageDown / 右键"降低排位"）：把该词加入降权黑名单并
    // 持久化到 downweight.txt。此后它在候选排序里强制沉底（EffFreq=-1，排到
    // 末尾，候选超过 50 条时会被挤出第一屏）——"不符合用户需求的词让位"。
    void DemoteWord(_In_ const WCHAR *word, size_t wordLen);

    // 查询某词是否在降权黑名单中（右键菜单显示"降低/恢复排位"状态用）
    BOOL IsDownWord(_In_ const std::wstring &word) const;

    // 精确查询 (pinyin, word) 是否在内存词库中（含用户词库）
    BOOL IsEntry(_In_ const std::wstring &pinyin, _In_ const std::wstring &word) const;

private:
    struct Entry
    {
        std::wstring pinyin;   // 拼音 key
        std::wstring word;     // 汉字词
        std::wstring initial;  // 简拼（声母串）
        float        freq;     // 词频
        bool         isUser;   // true=用户造词/用户词库独有条目（Ctrl+Delete 可删）；false=词库自带（不可删）
    };

    struct DpState
    {
        float        cost;       // 累积代价（-log 词频）
        std::wstring sentence;   // 到当前位置的整句
    };

    struct SingleChar
    {
        std::wstring word;   // 单字
        float        freq;   // 词频
    };

    std::vector<Entry> _entries;   // 按 (pinyin 升序, freq 降序) 排序
    // 按 initial 升序排序的下标视图（简拼二分查找）。
    // 用下标而非指针：AddUserWord 局部插入会触发 _entries realloc，指针会悬垂，下标不受影响。
    std::vector<size_t> _initialEntries;
    std::map<std::pair<std::wstring, std::wstring>, float> _userFreq;  // 用户词频加成
    std::map<std::pair<std::wstring, std::wstring>, std::wstring> _userInitial;  // 用户词简拼（造词时保存）

    std::wstring _dictPath;       // 主词库路径（ReloadAll 重建时用）
    std::wstring _userDictPath;   // 用户词库路径（与主词库同目录）
    size_t _mainDictEntries = 0;  // 主词库条数（LoadUserDict 归并基准：主词库段已有序，仅用户词段排序后归并）

    // 降权黑名单（候选模式 Ctrl+PageDown / 右键"降低排位"写入，downweight.txt 持久化）：
    // 排序比较器里降权词 freq 按 -1 参与比较 → 强制沉底。与用户自学习（freq+1）互不影响。
    std::unordered_set<std::wstring> _downWords;
    std::wstring _downWordsPath;  // %AppData%\NovaInput\downweight.txt
    void LoadDownWords();
    void SaveDownWords();
    // 排序用有效词频：降权词返回 -1（沉底），其余返回原 freq
    float EffectiveFreq(_In_ const Entry &e) const { return _downWords.count(e.word) ? -1.0f : e.freq; }

    std::deque<std::wstring> _sentencePool;   // 整句候选字符串池（deque 保证元素地址稳定，避免 SSO 悬垂）

    std::vector<std::wstring> _syllableTable;   // 合法完整拼音音节表（词库统计，按长度降序）
    std::map<wchar_t, std::vector<std::wstring>> _syllableByInitial;  // 首字母 → 音节分桶（SearchPrefix 首音节快速定位，避免逐音节遍历）
    std::map<std::wstring, std::vector<SingleChar>> _singleCharBySyllable;  // 音节 → 单字（词频降序）

    // 精确拼音查词（返回按词频降序的前 maxW 个）
    void GetWordsForPinyin(const std::wstring &py, size_t maxW, _Out_ std::vector<const Entry*> &out) const;

    // 校验拼音串是否可完整切分为合法完整音节（每段都在音节表中）。
    // 用户词库可能含简拼/畸形拼音词条（如 zt→钟婷天气、z→钟婷），
    // 它们可作为简拼直达词条，但绝不能被整句预测当作合法音节组合。
    bool IsValidFullPinyin(const std::wstring &key) const;

    static const int MAX_RESULTS = 50;

    void SearchPrefix(const std::wstring &prefix, _Out_ std::vector<const Entry*> &matched) const;
    static void FillResults(_In_ std::vector<const Entry*> &matched, _Inout_ CSampleImeArray<CCandidateListItem> *pItemList);

    void BuildSyllableIndex();   // 词库加载后构建音节表与单字索引（混合匹配用）
    void RebuildInitialIndex();  // 重建简拼索引（ReloadAll 全量；AddUserWord 改为局部维护）
    void ValidateInitialIndex(); // 校验简拼索引一致性（下标越界/initial 空/排序错乱 → 自动重建，防根因 4 类错位）
    void InsertIntoInitialIndex(size_t idx);   // 简拼索引局部插入新词下标（造词）
    void FixInitialIndexFreqOrder(const std::wstring &initial);  // 词频变化后局部重排同 initial 区间（造词/选词）
    static bool SegmentPinyin(_In_ const std::vector<std::wstring> &sylTable, _In_ const std::wstring &s, _Inout_ std::vector<std::wstring> &out);  // 贪心回溯切分
    static void EnumerateFullSyllables(_In_ const std::vector<std::wstring> &sylTable, _In_ const std::wstring &s,
        size_t pos, _Inout_ std::vector<std::wstring> &cur, _Inout_ std::vector<std::vector<std::wstring>> &all);  // 枚举全部完整音节切分
    double ScoreSegmentation(_In_ const std::vector<std::wstring> &syls) const;  // 切分打分：零声母音节降权 + 单字词频加分
    static bool IsZeroInitialSyllable(_In_ const std::wstring &s);  // 零声母韵母音节（an/ang/ai 等）
    void CollectBySingleChars(_In_ const std::vector<std::wstring> &syls, _Inout_ CSampleImeArray<CCandidateListItem> *pItemList);
    void CollectSingleSyllable(_In_ const std::wstring &syl, _In_ const std::vector<SingleChar> &chars, _Inout_ CSampleImeArray<CCandidateListItem> *pItemList);  // 单音节输入：单字加权混排（搜狗式单字优先）

    BOOL LoadUserDict();
    void SaveUserDict();
    BOOL LoadMainDict();   // 重载主词库（_entries 清空后重建，供 ReloadAll 复用）
    void FillMissingInitials();   // 补算存量词条简拼（旧 3 列 userdict 遗留；须在 BuildSyllableIndex 之后、RebuildInitialIndex 之前调用）

    // 主词库二进制预索引（冷启动 5-10s → <1s）：
    //   txt 解析完成后写 <dict>.bin（含 txt size+mtime 校验），下次启动直接读入，
    //   跳过 29MB 文本解析 + 88 万条排序。缓存损坏/过期自动回退 txt 解析并重建。
    BOOL TryLoadMainDictCache();
    void SaveMainDictCache();
};

// ---- 主词库路径解析（大字库模式，2026-08-13）----
// 读 bin\engine.conf 的 bigdict=0/1（设置面板写入）决定加载哪份词库：
//   0（默认）→ pinyin-plus.txt（仅 CJK 基本区常用字，88 万条）
//   1        → pinyin-plus-big.txt（+41448 大字表 4 万+ 生僻字，92 万条）
// 每次调用实时读文件：引擎启动与热重载（EnginePipe TriggerReload）都经此解析，
// 设置面板切换开关后触发 type 11 热重载即可换词库，无需重启引擎进程。
std::wstring ResolveDictPath();
