/* tutorial/13_gates/main.c — 第 13 课 · 架构纪律与验收门禁
 *
 * 累加内容：为什么这个工程“敢改”——一整套防回归门禁。
 *   1) 分层门禁 check_layers：依赖必须是纯 DAG，反向 #include 立刻被抓。
 *   2) 回归运行器：ASSERT-OK / ASSERT-FAIL 标记 + 退出码 双判据（标记缺失也算 FAIL）。
 *   3) 负向对照：任何判据都必须“能失败”——注入 bug 后断言必须 FAIL，否则判据是假绿。
 *
 * 编译： gcc -std=c11 -O2 -o tut main.c && ./tut ; echo "exit=$?"
 * （本课前几课都出 BMP；本课刻意只产出文本标记 + 退出码，因为真实门禁就是这么工作的。
 *   但为保持“每课出图”，额外画了一张分层 DAG 示意图 gates.bmp。）
 */
#include "../common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* —— 1) 模块分层（与项目终态 DAG 完全一致） ——
 * utils ← pbf ← map ← route ← render ← ui ← ui_qt
 * 允许方向：模块 i 只能 #include 层号 ≤ i 的模块（含自身层）。 */
static const char* LAYERS[7] = {"utils","pbf","map","route","render","ui","ui_qt"};
static int layer_idx(const char* m){
    for (int i = 0; i < 7; i++) if (strcmp(LAYERS[i], m) == 0) return i;
    return -1; /* 横切模块（search/settings）不在此表，放行 */
}

typedef struct { const char* file; int line; const char* srcmod; const char* tgtmod; } Inc;

/* 返回违规数；每处违规打印 file:line 与方向。 */
static int check_layers(const Inc* incs, int n){
    int viol = 0;
    for (int i = 0; i < n; i++){
        int si = layer_idx(incs[i].srcmod);
        int ti = layer_idx(incs[i].tgtmod);
        if (si < 0 || ti < 0) continue;        /* 横切模块，放行 */
        if (ti > si) {                          /* 反向依赖：include 了更高层 */
            printf("  VIOLATION %s:%d  %s 不允许 include %s（反向依赖，破坏 DAG）\n",
                   incs[i].file, incs[i].line, incs[i].srcmod, incs[i].tgtmod);
            viol++;
        }
    }
    return viol;
}

/* —— 2) 回归运行器：标记 + 退出码 双判据 —— */
static int g_fail = 0;
#define CHECK(name, cond) do { \
    if (cond) printf("ASSERT-OK   %s\n", name); \
    else { printf("ASSERT-FAIL %s\n", name); g_fail++; } \
} while (0)

/* —— 3) 被测逻辑：一个会被“改一个点”的阈值 + 依赖它的函数 ——
 * 真实工程对应物：AREA_DOT_PX / ROUTE_HEUR_W / ovl-alpha … */
static int KNOT = 10;
static int classify(int n){ return n < KNOT ? 0 : 1; }        /* 0=低 1=高 */
static int classify_bug(int n){ return n <= KNOT ? 0 : 1; }   /* 注入 bug：< 写成 <= */

/* 画一张“分层 DAG”示意图：7 条彩色横带（下=地基），带间连线表示允许依赖方向。 */
static void draw_dag(uint32_t* buf, int W, int H){
    static const uint32_t C[7] = {
        0xFF3A6EA5,0xFF4F8FBF,0xFF5BAE8C,0xFF8FBF4F,0xFFBF8F4F,0xFFBF5A5A,0xFFA53A3A
    };
    int bh = H / 7;
    for (int i = 0; i < 7; i++)
        tut_fill_rect(buf, W, H, 0, i*bh, W, bh, C[i]);
    for (int i = 0; i < 6; i++){
        int y0 = i*bh + bh/2, y1 = (i+1)*bh + bh/2;
        int x0 = W - 30 - i*20, x1 = 30 + (6-i)*20;
        tut_line(buf, W, H, x0, y0, x1, y1, 0xFF000000);
    }
}

int main(void){
    printf("=== 第 13 课 · 架构纪律与验收门禁 ===\n\n");

    /* DAG 示意图 */
    int W = 420, H = 240;
    static uint32_t buf[420*240];
    tut_cls(buf, W, H, 0xFFF4F1EA);
    draw_dag(buf, W, H);
    tut_write_bmp("gates.bmp", buf, W, H);

    /* ---- 分层门禁 ---- */
    printf("\n[分层门禁 check_layers]\n");
    Inc good[] = {
        {"map/graph_build.c", 12, "map",  "utils"},
        {"route/route.c",      40, "route","map"},
        {"ui/window.c",        11, "ui",   "render"},
        {"ui_qt/qt_main.cpp",  68, "ui_qt","ui"},
    };
    CHECK("clean deps: 0 violations",
          check_layers(good, (int)(sizeof good / sizeof good[0])) == 0);
    Inc bad[] = { {"map/graph_build.c", 7, "map", "ui"} };  /* map 反向 include ui */
    CHECK("reverse dep caught: 1 violation", check_layers(bad, 1) == 1);

    /* ---- 回归判据 + 负向对照 ---- */
    printf("\n[回归判据 + 负向对照]\n");
    CHECK("classify 单调: classify(0)<=classify(99)", classify(0) <= classify(99));
    CHECK("classify(0)==0 (默认 KNOT=10)", classify(0) == 0);
    CHECK("classify(99)==1", classify(99) == 1);
    CHECK("classify(KNOT)==1 (边界上界归高类)", classify(KNOT) == 1);
    CHECK("负向对照(@good): classify(KNOT-1)==0 成立", classify(KNOT-1) == 0);
    /* 同样的断言，对“注入 bug 的坏实现”必须 FAIL —— 证明判据有判别力 */
    CHECK("负向对照(@bug): 坏实现使 classify(KNOT)==1 失败（判据有判别力）",
          !(classify_bug(KNOT) == 1));

    printf("\n=== REGRESS-RESULT: %s ===\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
