#!/usr/bin/env python3
"""44 条城市对的算路分布基准（PLAN.md §46.1 的脚本化版本）

用法：
    python3 scripts/citypairs.py                 # 当前默认配置
    python3 scripts/citypairs.py --lm=1 --w=1.2  # 指定地标表开关与 w
    python3 scripts/citypairs.py --tag=基线      # 输出前缀（做 A/B 时好认）

判据口径（★ 别拿耗时当布尔判据，负载能放大一个数量级，见 PLAN.md §47.4）：
  - **展开数**：确定性，主判据
  - **预计分钟**：质量指标（mode_time=1 优化的是时间，不是里程）
  - 耗时只做参考，取 --repeat 里的最后一次

城市表与 random.seed(7) 与 §46.1 完全一致 ⇒ 历次结果可比。
"""
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


def build_pairs():
    random.seed(7)
    pairs = set()
    while len(pairs) < 50:
        a, b = random.sample(CITIES, 2)
        pairs.add((a, b))
    return list(pairs)


def run(p, lm, w, repeat=2):
    env = dict(os.environ)
    env["NAVIMAP_LM"] = "1" if lm else "0"
    cmd = [BIN, "--graph=" + GRAPH,
           "--from=%.2f,%.2f" % (p[0][1], p[0][2]),
           "--to=%.2f,%.2f" % (p[1][1], p[1][2]),
           "--repeat=%d" % repeat]
    if w:
        cmd += ["--heur-w=%s" % w, "--heur-w-hi=%s" % w]
    try:
        a = subprocess.run(cmd, capture_output=True, text=True, timeout=900, env=env)
    except subprocess.TimeoutExpired:
        return None
    t = a.stdout
    if "ROUTE-OK" not in t:
        return None
    m = re.search(r"搜索\s+展开 (\d+) 节点，耗时 (\d+)", t)
    km = re.search(r"总长\s+([\d.]+) km", t)
    mi = re.search(r"预计\s+(\d+) 分", t)
    if not (m and km and mi):
        return None
    return dict(exp=int(m.group(1)), ms=int(m.group(2)),
                km=float(km.group(1)), min=int(mi.group(1)))


def pct(v, q):
    v = sorted(v)
    return v[min(len(v) - 1, int(len(v) * q))]


def main():
    lm = None
    w = None
    tag = ""
    for a in sys.argv[1:]:
        if a.startswith("--lm="):
            lm = a[5:] == "1"
        elif a.startswith("--w="):
            w = a[4:]
        elif a.startswith("--tag="):
            tag = a[6:] + " "
    rows = []
    for p in build_pairs():
        r = run(p, lm, w)
        if not r:
            continue
        rows.append((p[0][0] + "→" + p[1][0], r))
        print("  %s%-12s %9d 展开 %7d ms %8.1f km %5d 分" %
              (tag, rows[-1][0], r["exp"], r["ms"], r["km"], r["min"]), flush=True)
    rows.sort(key=lambda x: -x[1]["exp"])
    print("\n=== 前 8 慢（按展开数）===")
    for k, r in rows[:8]:
        print("  %-12s %9d 展开 %7d ms %8.1f km %5d 分" %
              (k, r["exp"], r["ms"], r["km"], r["min"]))
    ex = [r["exp"] for _, r in rows]
    ms = [r["ms"] for _, r in rows]
    mn = [r["min"] for _, r in rows]
    print("\n有效 %d 条" % len(rows))
    print("  展开  中位 %d  均值 %d  p90 %d  最慢 %d  合计 %d" %
          (pct(ex, .5), sum(ex) / len(ex), pct(ex, .9), max(ex), sum(ex)))
    print("  耗时  中位 %d ms  p90 %d ms  最慢 %d ms  合计 %d ms" %
          (pct(ms, .5), pct(ms, .9), max(ms), sum(ms)))
    print("  质量  预计分钟合计 %d" % sum(mn))
    print("  >1s 的 %d 条，>3s 的 %d 条" %
          (sum(1 for t in ms if t > 1000), sum(1 for t in ms if t > 3000)))


if __name__ == "__main__":
    main()
