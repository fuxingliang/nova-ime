#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Nova 词库质量验证脚本 —— 每次 build_dict.py 构建后运行
检查：总规模 / 扩展区生僻字 / 繁体残留 / 拼音格式 / 常用字词覆盖 / 指定拼音候选
用法：python tools/verify_dict.py [拼音...]   （可附加要抽查的拼音，默认 che/zhu rong ji）
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_dict import TRAD_CHARS

OUT = Path(__file__).resolve().parent.parent / "bin" / "pinyin-plus.txt"

def main():
    lines = OUT.read_text(encoding="utf-8").splitlines()
    entries = [l.split("\t") for l in lines if l and not l.startswith("#")]
    total = len(entries)

    ext = sum(1 for _, w, *_ in entries if any(ord(c) >= 0x20000 for c in w))
    trad = sum(1 for _, w, *_ in entries if any(c in TRAD_CHARS for c in w))
    badpy = sum(1 for py, w, *_ in entries if not (py.islower() and py.isascii()) or " " in py)
    nocol = sum(1 for l in entries if len(l) < 4)

    print(f"总词条: {total}")
    print(f"CJK扩展区(应=0): {ext}")
    print(f"含繁体(应≈0): {trad}")
    print(f"拼音格式异常(应=0): {badpy}")
    print(f"缺简拼列(应=0): {nocol}")

    # 常用字/词覆盖
    testmap = {}
    for py, w, *_ in entries:
        testmap.setdefault(w, set()).add(py)
    tests = ["啊", "我", "你", "的", "车", "国", "朱镕基", "镕", "龘", "中华人民共和国",
             "你好", "世界", "计算机", "人工智能", "笔记本电脑", "北京大学", "阿里巴巴", "量子计算"]
    print("--- 常用字/词覆盖 ---")
    for t in tests:
        pys = testmap.get(t)
        if pys:
            print(f"  ✓ {t}  {sorted(pys)}")
        else:
            print(f"  ✗ {t}  缺失")

    # 抽查拼音候选（默认 che + zhu rong ji）
    pys_to_check = ["che", "zhurongji"] + sys.argv[1:]
    for py in pys_to_check:
        cands = [(w, f) for p, w, f, *_ in entries if p == py][:10]
        print(f"--- {py} 候选前10 ---")
        if not cands:
            print("  （无结果）")
        for w, f in cands:
            print(f"  {w}  freq={f}")


if __name__ == "__main__":
    main()
