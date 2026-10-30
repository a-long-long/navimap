# 综合项目 · 改一个点 + 加一条判据

**目标**：独立完成“改一个点 + 加一条判据”，体验完整的“改 → 测 → 守”闭环。这是前 13 课的毕业设计。

**运行**：
```bash
gcc -std=c11 -O2 -o tut main.c && ./tut ; echo "exit=$?"
```

**你会看到**：
- 打印本次改动 `THRESH 10 -> 20`，原有判据全绿，新增判据 `classify(15)==0` 锁住改动。
- 负向对照证明：若 `THRESH` 被改回 10，这条新判据会 FAIL（即它真的守住了改动）。
- 附带 `capstone.bmp`：上=改动前（THRESH=10）、下=改动后（THRESH=20）的阈值条，
  蓝=低类、红=高类，可见“红区”被推到了更右边。

**本目录刻意复刻的真实工程闭环**：

| 玩具例子 | 真实工程类比（任选其一去做） |
|---|---|
| 改 `THRESH` 10→20 | 改 `src/render/paint.h` 的 `AREA_DOT_PX` 4→6；或 `ROUTE_HEUR_W` 1.4→1.25；或 `--ovl-alpha` 改值 |
| 加 `ASSERT(classify(15)==0)` | 加 `regress.sh` 的 `area_lod_tier` 区间断言；或 `ovl_px.py` 半透明比率判据；或 `heap_selftest --expect-fail` |
| 负向对照 `classify_with(10,...)` | 改回原值跑回归，确认新判据 FAIL；或 `voice_neg.sh` / `scripts/voice_neg.sh` |

**交付要求（和真实工程一致）**：
1. 改动走 `scripts/check_layers.sh` 零违规（改的是数值/配置，不应引入反向依赖）。
2. 新增/改动有对应 `regress.sh` 判据，**确定性量优先**于时间（时间判据 flaky，机器负载能放大一个数量级）。
3. 提交前 `./scripts/regress.sh` 全绿（或仅剩已知环境缺失项）。

**练习（动手把玩具变成真的）**：
- 选上表任一真实类比，照“改一个点 + 加一条判据 + 负向对照”三步做一遍：
  1. 改一个数值（如 `AREA_DOT_PX` 4→6）。
  2. 用 `python3 scripts/bmpdiff.py 旧.bmp 新.bmp` 量化“净损失面积”（变背景色像素 − 从背景色变出像素），
     把结果写进 `docs/` 的档位表，并收紧 `regress.sh` 的 `area_lod_tier` 区间。
  3. 负向对照：把值改回 4 跑回归，确认 `area_lod_tier` FAIL，证明判据守住了你的改动。
- 完成后 `git commit` + `git push origin main`（本项目铁律：完成即提交推送）。
