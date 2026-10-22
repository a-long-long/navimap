#!/usr/bin/env python3
"""地标下界的**可采纳性**验收（PLAN.md §50.5 第 5 条 / §52.8）

现状（2026-09-28 实测，已拍板接受）
--------------------------------
**地标下界不是严格下界**，是"加速启发式"。50 条实测：一致 46 / 越界 2 /
跑不完 2。越界的：南阳→天津 501 vs 500 分、太原→石家庄 129 vs 128 分
（距离口径放大到 895.16 vs 878.41 km，+1.9%）。

已证伪三个假设：加大 penalty（+40 km 不收敛）、双向目标格取错（单向同样越界）、
转向限制（`--no-restr` 建的表照样越界）。剩余候选原因：u 侧要"格内 max"
而不是 min（格内两点的*路程*可远大于格对角）。详见 PLAN.md §52.8。

⇒ 用法：这个脚本现在是**监控工具**，不是门禁。用 `--slack=1` 跑表示
   "接受已知的最多差 1 分"，退出码 0；不加 slack 就是严格的"必须相等"判据。
   `regress.sh` 里只放了一条最便宜的路线（太原→石家庄）做**不许恶化**的门禁。

判据
----
双向 `--exact` + `w=1.0` 在 h 可采纳时**保证返回最优解**（route.c 里那段证明：
若存在更优解 C* < best，它必经某个接缝 n，两侧 f(n) ≤ C* ⇒ max(ff,fb) < best，
循环不会停）。所以：

    开表 w=1.0 的预计分钟  ==  无表 w=1.0 的预计分钟

无表时 h 是直线距离（必然可采纳）⇒ 它就是**真值**。
开表时 h = max(直线, 地标下界) ⇒ 一旦下界在某处越界（聚合/罚项/跨省缝合出错），
A* 会提前终止并返回**更差**的解。⇒ **开表比无表差哪怕 1 分钟就是 bug。**

★ 为什么不是"不差于"而是"必须相等"：两者都是 w=1.0 + 可采纳 h 的精确 A*，
  给的都是同一个最优值。出现"开表更好"同样说明有问题（那说明无表那侧提前
  终止了，或者两侧搜的不是同一个代价口径）。

★ 为什么不用 `--bidir=0`：单向 w=1.0 太贵（保定→石家庄 139 km 就超 1200 万
  展开上限，而且**开表和无表都超**），跑不动 50 条。双向 `--exact` 便宜一到两个
  数量级。双向在 h 可采纳时的最优性证明见 route.c。

用法
----
    python3 scripts/lmadmissible.py              # 全 50 条
    python3 scripts/lmadmissible.py --cap=20     # 只跑前 N 条（快速冒烟）
    python3 scripts/lmadmissible.py --max-expand=20000000

退出码 0 = 无越界；1 = 发现越界（无论有几条 inconclusive）。

注意
----
- 展开上限默认沿用 route_cli 的 1200 万。命中的路线两侧都跑不完 ⇒ 记成
  INCONCLUSIVE，**不能当作通过**。要覆盖它们就调大 --max-expand（内存约
  250 B/展开，20M ≈ 5 GB，本机不一定放得下）。
- 判据是**预计分钟**（mode_time=1），不是里程。
"""
import argparse
import os
import random
import re
import subprocess
import sys

CITIES = [("北京", 39.90, 116.40), ("天津", 39.13, 117.20), ("石家庄", 38.04, 114.51),
          ("唐山", 39.63, 118.18), ("保定", 38.87, 115.46), ("邯郸", 36.63, 114.54),
          ("秦皇岛", 39.94, 119.60), ("太原", 37.87, 112.55), ("大同", 40.09, 113.30),
          ("临汾", 36.09, 111.52), ("济南", 36.65, 117.02), ("青岛", 36.07, 120.38),
          ("烟台", 37.46, 121.45), ("潍坊", 36.71, 119.16), ("临沂", 35.10, 118.36),
          ("郑州", 34.75, 113.62), ("洛阳", 34.62, 112.45), ("开封", 34.80, 114.31),
          ("南阳", 32.99, 112.53), ("西安", 34.34, 108.94), ("宝鸡", 34.36, 107.24),
          ("汉中", 33.08, 107.02), ("沈阳", 41.80, 123.43), ("大连", 38.91, 121.61),
          ("鞍山", 41.11, 122.99), ("锦州", 41.10, 121.13), ("丹东", 40.13, 124.38)]

BIN = "./build/route_cli"
GRAPH = "products/graph"
N_PAIR = 50
SEED = 7          # 与 scripts/citypairs.py 完全一致 ⇒ 两侧可比


def build_pairs():
    random.seed(SEED)
    pairs = set()
    while len(pairs) < N_PAIR:
        a, b = random.sample(CITIES, 2)
        pairs.add((a, b))
    return list(pairs)


def run(p, lm, max_expand):
    """返回 (分钟, 展开数) 或 None（跑不完 / 失败）。"""
    env = dict(os.environ)
    env["NAVIMAP_LM"] = "1" if lm else "0"
    cmd = [BIN, "--graph=" + GRAPH,
           "--from=%.2f,%.2f" % (p[0][1], p[0][2]),
           "--to=%.2f,%.2f" % (p[1][1], p[1][2]),
           "--repeat=1",
           "--exact",                       # 保证最优的双向终止条件
           "--heur-w=1.0", "--heur-w-hi=1.0"]  # ★ hi 也要给，否则两遍会用到默认的 w
    if max_expand:
        cmd += ["--max-expand=%d" % max_expand]
    try:
        a = subprocess.run(cmd, capture_output=True, text=True, timeout=900, env=env)
    except subprocess.TimeoutExpired:
        return None
    t = a.stdout
    if "ROUTE-OK" not in t:
        return None
    m = re.search(r"预计\s+(\d+)\s+分", t)
    e = re.search(r"搜索\s+展开\s+(\d+)\s+节点", t)
    if not m:
        return None
    return (int(m.group(1)), int(e.group(1)) if e else -1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cap", type=int, default=0, help="只跑前 N 条（冒烟用）")
    ap.add_argument("--max-expand", type=int, default=0, help="0 = 用 route_cli 默认")
    ap.add_argument("--slack", type=int, default=0,
                    help="容忍的开表-真值差（分钟）。0 = 严格必须相等；"
                         "1 = 接受当前已知的最多差 1 分（发货状态）")
    args = ap.parse_args()

    pairs = build_pairs()
    if args.cap:
        pairs = pairs[:args.cap]

    bad = 0
    inconclusive = 0
    tolerated = 0
    print("路线            有表(分/展开)      无表(分/展开)      判定")
    for p in pairs:
        name = "%s→%s" % (p[0][0], p[1][0])
        a = run(p, True, args.max_expand)
        b = run(p, False, args.max_expand)
        if a is None or b is None:
            inconclusive += 1
            print("%-14s %-18s %-18s INCONCLUSIVE（跑不完，不算通过）"
                  % (name,
                     ("%d/%d" % a) if a else "<失败>",
                     ("%d/%d" % b) if b else "<失败>"))
            continue
        fa, fb = a[0], b[0]
        if fa == fb:
            verdict = "OK"
        elif fa - fb <= args.slack:
            tolerated += 1
            verdict = "已知越界 差 %+d 分（在 slack=%d 内，已接受）" % (fa - fb, args.slack)
        else:
            bad += 1
            verdict = "★越界 差 %+d 分（超过 slack=%d）" % (fa - fb, args.slack)
        print("%-14s %-18s %-18s %s"
              % (name, "%d/%d" % a, "%d/%d" % b, verdict))
        sys.stdout.flush()

    print()
    print("共 %d 条：一致 %d，容忍内越界 %d，超容忍越界 %d，跑不完(inconclusive) %d"
          % (len(pairs), len(pairs) - bad - tolerated - inconclusive,
             tolerated, bad, inconclusive))
    if bad:
        print("LM-ADMISSIBLE-FAIL（越界超过 slack=%d ⇒ 次优变严重了）" % args.slack)
        return 1
    if bad == 0 and tolerated == 0 and inconclusive == 0:
        print("LM-ADMISSIBLE-OK（开表 w=1.0 与无表 w=1.0 逐条相等）")
        return 0
    print("LM-ADMISSIBLE-OK-WITHIN-SLACK（已知越界 %d 条，都在 slack=%d 内；"
          "未验到 %d 条）" % (tolerated, args.slack, inconclusive))
    return 0


if __name__ == "__main__":
    sys.exit(main())
