/* tutorial/14_capstone/main.c — 综合项目：改一个点 + 加一条判据
 *
 * 本目录演示完整的“改 → 测 → 守”闭环，用一个玩具例子，但方法 1:1 对应真实工程：
 *   改一个点：把分类阈值 THRESH 从 10 改成 20
 *             （真实工程类比：AREA_DOT_PX 4→6 / ROUTE_HEUR_W 1.4→1.25 / --ovl-alpha 改值）。
 *   加一条判据：新增 ASSERT(classify(15)==0)，把“15 归低类”这个由 THRESH=20 才成立的不变量锁住。
 *   负向对照：若有人把 THRESH 改回 10，classify(15) 会变 1，这条新判据立刻 FAIL
 *             → 证明它真的守住了改动（判据有判别力，不是假绿）。
 *
 * 编译： gcc -std=c11 -O2 -o tut main.c && ./tut ; echo "exit=$?"
 */
#include "../common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* —— 本次“改的一个点”：阈值 —— */
static int THRESH = 20;   /* 原值 10，本次改为 20 */

static int classify(int n){ return n < THRESH ? 0 : 1; }        /* 0=低 1=高 */
static int classify_with(int t, int n){ return n < t ? 0 : 1; }  /* 参数化版：用于负向对照 */

static int g_fail = 0;
#define CHECK(name, cond) do { \
    if (cond) printf("ASSERT-OK   %s\n", name); \
    else { printf("ASSERT-FAIL %s\n", name); g_fail++; } \
} while (0)

/* 画两张阈值条：上=THRESH=10（改动前），下=THRESH=20（改动后）。
 * 蓝=低类，红=高类；可见改动后“红区”被推到了更右边。 */
static void draw_bars(uint32_t* buf, int W, int H){
    tut_cls(buf, W, H, 0xFFF4F1EA);
    int tb = H/5, bb = H*4/5, bh = H/5;
    for (int x = 0; x < W; x++){
        int n = (int)(x * 30.0 / W);
        uint32_t c10 = classify_with(10, n) ? 0xFFB03A1F : 0xFF1F6FB0;
        uint32_t c20 = classify_with(20, n) ? 0xFFB03A1F : 0xFF1F6FB0;
        tut_fill_rect(buf, W, H, x, tb, 1, bh, c10);
        tut_fill_rect(buf, W, H, x, bb, 1, bh, c20);
    }
}

int main(void){
    printf("=== 综合项目 · 改一个点 + 加一条判据 ===\n\n");
    printf("本次改动：THRESH 10 -> %d（玩具版；真实工程类比见 README）\n\n", THRESH);

    /* 原有判据（改动前就有的，仍然全绿） */
    printf("[原有判据]\n");
    CHECK("classify 单调", classify(0) <= classify(99));
    CHECK("classify(0)==0", classify(0) == 0);
    CHECK("classify(99)==1", classify(99) == 1);

    /* —— 加的一条判据：锁住“15 归低类”（只有 THRESH>=16 才成立） —— */
    printf("\n[新增判据（守改动）]\n");
    CHECK("classify(15)==0（THRESH=20 才成立）", classify(15) == 0);

    /* 负向对照：若 THRESH 被改回 10，classify(15) 变 1 → 新判据必须 FAIL */
    int regress_caught = !(classify_with(10, 15) == 0);
    CHECK("负向对照：THRESH 回退到 10 时新判据 FAIL（守住改动）", regress_caught);

    /* 出图：把“改动前 vs 改动后”画出来 */
    int W = 420, H = 240;
    static uint32_t buf[420*240];
    draw_bars(buf, W, H);
    tut_write_bmp("capstone.bmp", buf, W, H);

    printf("\n=== REGRESS-RESULT: %s ===\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
