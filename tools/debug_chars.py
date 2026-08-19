# -*- coding: utf-8 -*-
"""临时：单字覆盖分析——单字总数/音节分布/生僻字/多音字可达性"""
from collections import defaultdict

OUT = open(r"g:\pinyin-plus\tools\debug_chars_out.txt", "w", encoding="utf-8")
log = lambda *a: print(*a, file=OUT)

lines = open(r"g:\pinyin-plus\bin\pinyin-plus.txt", encoding="utf-8").read().splitlines()
entries = [l.split("\t") for l in lines if l and not l.startswith("#")]

single = defaultdict(list)   # 音节 -> [(字, freq)]
for py, w, f, *_ in entries:
    if len(w) == 1:
        single[py].append((w, float(f)))

total_chars = sum(len(v) for v in single.values())
log("单字条目总数: %d" % total_chars)
log("覆盖音节数: %d" % len(single))

sizes = sorted((len(v) for v in single.values()), reverse=True)
log("音节最大单字数: %d, 前10: %s" % (sizes[0], sizes[:10]))
over50 = sum(1 for s in sizes if s > 50)
log("单字数>50(超候选截断)的音节数: %d" % over50)

big = sorted(single.items(), key=lambda kv: -len(kv[1]))[:8]
for py, chars in big:
    log("  %s (%d字): %s..." % (py, len(chars), "".join(c for c, _ in chars[:20])))

tests = ["啊", "我", "你", "的", "长", "行", "重", "乾", "囧", "龘", "爨", "齉", "靁",
         "氤", "氲", "旖", "旎", "觊", "觎", "龉", "嶙", "峋", "缱", "绻", "睿", "鑫", "淼"]
charset = set()
for v in single.values():
    for c, _ in v:
        charset.add(c)
log("\n单字去重总数: %d" % len(charset))
for t in tests:
    if t in charset:
        pys = [py for py, v in single.items() if any(c == t for c, _ in v)]
        log("  %s OK %s" % (t, pys))
    else:
        log("  %s MISS" % t)

for t in ["长", "行", "重", "乐", "还"]:
    pys = [py for py, v in single.items() if any(c == t for c, _ in v)]
    log("多音字 %s: %s" % (t, pys))

OUT.close()
