# -*- coding: utf-8 -*-
"""Viterbi 整句预测算法验证（与 C++ 引擎同逻辑）"""
import math

SYLLABLES = set("""a ai an ang ao ba bai ban bang bao bei ben beng bi bian biao bie bin bing bo bu
ca cai can cang cao ce cei cen ceng cha chai chan chang chao che chen cheng chi chong chou chu chua chuai chuan chuang chui chun chuo ci cong cou cu cuan cui cun cuo
da dai dan dang dao de dei den deng di dia dian diao die ding diu dong dou du duan dui dun duo
e ei en eng er fa fan fang fei fen feng fo fou fu
ga gai gan gang gao ge gei gen geng gong gou gu gua guai guan guang gui gun guo
ha hai han hang hao he hei hen heng hong hou hu hua huai huan huang hui hun huo
ji jia jian jiang jiao jie jin jing jiong jiu ju juan jue jun
ka kai kan kang kao ke kei ken keng kong kou ku kua kuai kuan kuang kui kun kuo
la lai lan lang lao le lei leng li lia lian liang liao lie lin ling liu lo long lou lu luan lun luo lv lve
ma mai man mang mao me mei men meng mi mian miao mie min ming miu mo mou mu
na nai nan nang nao ne nei nen neng ni nian niang niao nie nin ning niu nong nou nu nuan nuo nv nve
o ou pa pai pan pang pao pei pen peng pi pian piao pie pin ping po pou pu
qi qia qian qiang qiao qie qin qing qiong qiu qu quan que qun
ran rang rao re ren reng ri rong rou ru ruan rui run ruo
sa sai san sang sao se sen seng sha shai shan shang shao she shei shen sheng shi shou shu shua shuai shuan shuang shui shun shuo si song sou su suan sui sun suo
ta tai tan tang tao te teng ti tian tiao tie ting tong tou tu tuan tui tun tuo
wa wai wan wang wei wen weng wo wu xi xia xian xiang xiao xie xin xing xiong xiu xu xuan xue xun
ya yan yang yao ye yi yin ying yo yong you yu yuan yue yun
za zai zan zang zao ze zei zen zeng zha zhai zhan zhang zhao zhe zhei zhen zheng zhi zhong zhou zhu zhua zhuai zhuan zhuang zhui zhun zhuo zi zong zou zu zuan zui zun zuo""".split())

# 词库：pinyin -> [(word, freq)]
DICT = {}
with open(r"g:\pinyin-plus\bin\pinyin-plus.txt", encoding="utf-8") as f:
    for line in f:
        parts = line.rstrip("\n").split("\t")
        if len(parts) != 3:
            continue
        py, word, freq = parts[0], parts[1], float(parts[2])
        DICT.setdefault(py, []).append((word, freq))
for v in DICT.values():
    v.sort(key=lambda x: -x[1])

def viterbi(s, K=3, W=3, LAMBDA=5.0, MAX_KEY=18):
    n = len(s)
    dp = [[] for _ in range(n + 1)]
    dp[0] = [(0.0, "")]
    for i in range(1, n + 1):
        for j in range(max(0, i - MAX_KEY), i):
            if not dp[j]:
                continue
            sy = s[j:i]
            words = DICT.get(sy, [])[:W]
            if not words:
                continue
            for cost, sent in dp[j]:
                for w, freq in words:
                    dp[i].append((cost - math.log(max(freq, 0.01)) + LAMBDA, sent + w))
        dp[i] = sorted(dp[i])[:K]
    return [sent for _, sent in sorted(dp[n])]

for test in ["woxiangni", "nihao", "gangcai", "zhongguoren", "womendajia", "haoburongyi", "jintiantianqizhenhao", "zhongguogongchandang"]:
    r = viterbi(test)
    print(f"{test} -> {r[:3]}")
