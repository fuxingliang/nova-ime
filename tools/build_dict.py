#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Nova 输入法词库构建脚本 v2 (2026-08-13)
========================================
数据源：rime-ice 纯简体精校词库（LGPL-3.0，github.com/iDvel/rime-ice）
  - 8105.dict.yaml  《通用规范汉字表》单字+字频 —— 常用字兜底（保证每个规范汉字可达）
  - base.dict.yaml  核心词库（真实语料词频，含人名如"朱镕基"）
  - ext.dict.yaml   扩展词库
  - 41448.dict.yaml 大字表（4 万+ 生僻字，仅 --big 模式启用，作为第二字表殿后）

输出：
  - bin/pinyin-plus.txt      默认词库（仅 CJK 基本区，无生僻字）
  - bin/pinyin-plus-big.txt  大字库（--big：加入 41448 生僻字，放宽到扩展区）
格式均为 Nova 引擎格式：拼音\t词\t词频\t简拼
  - 按拼音字典序排序（引擎二分前缀查找），同拼音按词频降序
  - 拼音已规范化：小写、无空格、ü→v

过滤层（对齐 docs/DEVELOPMENT.md「词库调研结论」）：
  - 仅保留 CJK 基本区 (U+4E00-9FFF) 纯汉字词 → 砍掉扩展区生僻字（v1 中占 11.4%）
    --big 模式放宽到全部 CJK 汉字区（基本区+扩展 A-H+兼容区），生僻字才可达
  - 词长 1-10
  - 繁体字兜底过滤（rime-ice 为纯简体，此层为保险）

历史：
  v1（已废弃）使用 CC-CEDICT + wordfreq + pinyin-data：
    简繁混杂（愛/車/對 高频繁体挤占候选）、11.4% 扩展区生僻字刷屏、
    wordfreq 英文主词频导致中文排序不准。
"""
import argparse
from pathlib import Path

ROOT = Path(__file__).resolve().parent
RIME = ROOT / "data" / "rime-ice"
OPENCC = ROOT / "data" / "opencc" / "TSCharacters.txt"
SYMBOLS = ROOT / "data" / "symbols.txt"
OUT = ROOT.parent / "bin" / "pinyin-plus.txt"
OUT_BIG = ROOT.parent / "bin" / "pinyin-plus-big.txt"

MAX_WORD_LEN = 10
# 大字表（41448.dict.yaml）无权重列，给超低词频保证"生僻字殿后"：
# 同音候选中 8105/base/ext 的真实词频（数千~数万）自然排前，生僻字垫底不刷屏。
BIG_TABLE_FREQ = 0.5
# 符号表（symbols.txt）词频：与大字表一致的低值，保证标点/数学/emoji 候选
# 排在常用字之后（如输入 dui 时 ✓ 排在"对/队/堆"后面），不挤占候选前位。
SYMBOL_FREQ = 0.5


def load_trad_chars(path: Path) -> set:
    """从 OpenCC TSCharacters.txt（繁→简单字映射，Apache-2.0）提取"纯繁体字"集合。

    格式 `key\tvalue(s)`（value 空格分隔）。关键：OpenCC 收录了**繁简共用字**
    （么/像/坏/乾/覆/沈/俱 等），它们的 value 包含自己（如 `乾\t干 乾`）——
    这些字在简体中同样合法（乾坤/做什么/图像/破坏），**不能过滤**。
    只有 value 不含自己的 key 才是纯繁体字形（如 `車\t车`、`愛\t爱`）。
    2026-08-13 实测：修正前误杀"乾/乾坤/乾隆/慰藉/做什么"等 1300+ 高频简体词。
    """
    trad = set()
    if not path.exists():
        return trad
    with path.open(encoding="utf-8") as fh:
        for line in fh:
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) < 2:
                continue
            key = parts[0]
            values = parts[1].split()
            if len(key) == 1 and key not in values:
                trad.add(key)
    return trad


TRAD_CHARS = load_trad_chars(OPENCC)

# 声母首字母（zh/ch/sh 取首字母 z/c/s，与搜狗一致）
def initial_of(py: str) -> str:
    if not py:
        return ""
    if py.startswith("zh"):
        return "z"
    if py.startswith("ch"):
        return "c"
    if py.startswith("sh"):
        return "s"
    return py[0]


def initials_of(pys: list) -> str:
    return "".join(initial_of(p) for p in pys).lower()


def norm_py(p: str) -> str:
    """rime 拼音 → Nova 拼音：小写、去空格、ü→v"""
    s = p.strip().lower().replace(" ", "")
    s = s.replace("ü", "v").replace("ǖ", "v").replace("ǘ", "v").replace("ǚ", "v").replace("ǜ", "v")
    return s


def is_pure_chinese(s: str) -> bool:
    """仅 CJK 基本区汉字（U+4E00-9FFF），砍掉扩展区生僻字与杂项字符"""
    return bool(s) and all("\u4e00" <= c <= "\u9fff" for c in s)


def is_hanzi(s: str) -> bool:
    """全部 CJK 汉字区（大字库模式用）：基本区 + 扩展 A-H + 兼容区 + 兼容补充区"""
    def _one(c: str) -> bool:
        o = ord(c)
        return (
            0x4E00 <= o <= 0x9FFF or   # 基本区
            0x3400 <= o <= 0x4DBF or   # 扩展 A
            0x20000 <= o <= 0x2EBEF or # 扩展 B-F
            0x30000 <= o <= 0x323AF or # 扩展 G-H
            0xF900 <= o <= 0xFAFF or   # 兼容汉字
            0x2F800 <= o <= 0x2FA1F    # 兼容补充区
        )
    return bool(s) and all(_one(c) for c in s)


def parse_rime(path: Path, hanzi_check) -> list:
    """解析 rime dict.yaml → [(word, py_norm, freq, initial)]

    rime 行格式：
      `词\t拼音(空格分隔)\t权重`（8105/base/ext/tencent 三列）
      `字\t拼音`（41448 大字表两列，无权重 → BIG_TABLE_FREQ 殿后）
    hanzi_check：is_pure_chinese（默认）或 is_hanzi（--big 大字库）
    """
    rows = []
    with path.open(encoding="utf-8") as fh:
        for line in fh:
            line = line.rstrip("\n")
            if not line or line.startswith("#") or line.startswith("---"):
                continue
            parts = line.split("\t")
            if len(parts) < 2:
                continue
            word = parts[0]
            pys = parts[1].split()
            # 过滤：非汉字（按模式） / 超长 / 含繁体（兜底）
            if not hanzi_check(word) or len(word) > MAX_WORD_LEN:
                continue
            if any(c in TRAD_CHARS for c in word):
                continue
            norm = "".join(norm_py(p) for p in pys)
            if not norm:
                continue
            if len(parts) >= 3 and parts[2].strip():
                freq = float(parts[2])
            else:
                freq = BIG_TABLE_FREQ   # 大字表无权重 → 低词频殿后
            rows.append((word, norm, freq, initials_of(pys)))
    return rows


def load_symbols(path: Path) -> list:
    """解析符号表（`拼音\t符号[\t简拼]`）→ [(word, py_norm, freq, initial)]

    符号（标点/数学/emoji 等）不是汉字，**绕过** is_pure_chinese/is_hanzi
    过滤（该层会把一切非汉字砍掉）。给 SYMBOL_FREQ 低词频保证殿后，
    不挤占常用字候选。
    简拼：可选的第三列（如 `sanjiao\t△\tsjx` —— "三角形"三字首字母），
    缺省用拼音首字母（dui→d）。多字名称的符号配显式简拼才能"简拼直达"
    （如 sjx→△、qdy→≌、rmb→¥），否则单音节拼音只能首字母前缀匹配。
    """
    rows = []
    if not path.exists():
        print(f"[!] 缺少符号表 {path.name} —— 跳过（符号输入暂不可用）")
        return rows
    with path.open(encoding="utf-8") as fh:
        for line in fh:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) < 2:
                continue
            py = norm_py(parts[0])
            sym = parts[1].strip()
            if not py or not sym or len(sym) > MAX_WORD_LEN:
                continue
            initial = initial_of(py)
            if len(parts) >= 3 and parts[2].strip():
                initial = parts[2].strip().lower()
            rows.append((sym, py, SYMBOL_FREQ, initial))
    return rows


def main():
    parser = argparse.ArgumentParser(description="构建 Nova 词库（rime-ice 数据源）")
    parser.add_argument("--big", action="store_true",
                        help="大字库模式：加入 41448 大字表（4万+生僻字），放宽到 CJK 扩展区")
    args = parser.parse_args()

    out = OUT_BIG if args.big else OUT
    hanzi_check = is_hanzi if args.big else is_pure_chinese
    mode_label = "大字库(big)" if args.big else "默认"

    sources = [
        (RIME / "8105.dict.yaml", "8105 常用字表"),
        (RIME / "base.dict.yaml", "base 核心词库"),
        (RIME / "ext.dict.yaml", "ext 扩展词库"),
    ]
    if args.big:
        # rime-ice 官方建议：41448 作为"第二字表"放到靠后位置 → 同音字按权重靠前，
        # 生僻字自动殿后（大字表词频 BIG_TABLE_FREQ=0.5 已保证这一点）。
        sources.append((RIME / "41448.dict.yaml", "41448 大字表"))

    entries = {}  # (py, word) -> [freq, initial]
    for path, label in sources:
        if not path.exists():
            print(f"[!] 缺少 {path.name}（{label}），跳过 —— 请先运行 tools/download_rime_dicts.ps1")
            continue
        parsed = parse_rime(path, hanzi_check)
        n = 0
        for word, py, freq, ini in parsed:
            key = (py, word)
            old = entries.get(key)
            if old is None or freq > old[0]:
                entries[key] = (freq, ini)
            n += 1
        print(f"[1] {label} {path.name}: 解析 {n} 行, 累计去重 {len(entries)}")

    # 符号层（方案 A：拼音直达标点/数学/货币/emoji 等）。
    # 符号不是汉字，绕过汉字过滤；低词频（SYMBOL_FREQ）自动殿后不刷屏。
    n_sym = 0
    for word, py, freq, ini in load_symbols(SYMBOLS):
        key = (py, word)
        old = entries.get(key)
        if old is None or freq > old[0]:
            entries[key] = (freq, ini)
        n_sym += 1
    if n_sym:
        print(f"[1.5] 符号表 symbols.txt: 并入 {n_sym} 条（低词频殿后）")

    print(f"[2] 过滤后词条总数 ({mode_label}): {len(entries)}")

    # 排序：拼音升序；同拼音按词频降序（引擎二分前缀查找 + 候选顺序依赖此序）
    rows = sorted(entries.items(), key=lambda kv: (kv[0][0], -kv[1][0]))

    tag = "big" if args.big else "default"
    with out.open("w", encoding="utf-8") as fh:
        fh.write(f"# Nova 主词库[{tag}] —— 由 tools/build_dict.py v2 构建 (rime-ice 数据源)\n")
        fh.write(f"# 总词条: {len(rows)}  格式: 拼音\\t词\\t词频\\t简拼\n")
        for (py, word), (freq, ini) in rows:
            fh.write(f"{py}\t{word}\t{freq:g}\t{ini}\n")

    size_mb = out.stat().st_size / 1024 / 1024
    print(f"[3] 已输出: {out} ({len(rows)} 条, {size_mb:.1f} MB)")


if __name__ == "__main__":
    main()
