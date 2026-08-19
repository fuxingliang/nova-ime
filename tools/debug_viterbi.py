# -*- coding: utf-8 -*-
"""调试 Viterbi 束搜索内部状态"""
import math, importlib.util

spec = importlib.util.spec_from_file_location("tv", r"g:\pinyin-plus\tools\test_viterbi.py")
tv = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tv)

D = tv.DICT
SY = tv.SYLLABLES
LAMBDA, K, W = 5.0, 3, 3

for s in ["zhongguoren", "jintiantianqizhenhao"]:
    print("=" * 20, s)
    n = len(s)
    dp = [[] for _ in range(n + 1)]
    dp[0] = [(0.0, "")]
    for i in range(1, n + 1):
        for j in range(i):
            if not dp[j]:
                continue
            sy = s[j:i]
            if sy not in SY:
                continue
            for cost, sent in dp[j]:
                for w, f in sorted(D.get(sy, []), key=lambda x: -x[1])[:W]:
                    dp[i].append((cost - math.log(max(f, 0.01)) + LAMBDA, sent + w))
        dp[i] = sorted(dp[i])[:K]
        print(f"  pos{i}: {dp[i]}")
