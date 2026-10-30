/* tutorial/05_graph/main.c — 第 5 课 · 建图：PBF → 路网（CSR）
 *
 * 累加内容：从"原始 OSM 要素"到"可导航图"。本课用一小撮手写节点/way 演示建图全过程：
 *   ① 节点原始 id → 0..n-1 下标映射；② 每条 way 相邻节点对 → 双向边；③ 边代价 = haversine 米。
 * 最终存成 CSR（压缩稀疏行：offset + edge list），这是 A* 高效遍历的内存布局。
 *
 * 编译： gcc -std=c11 -O2 -o tut main.c && ./tut
 */
#include "../common.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define W 320
#define H 240
static uint32_t buf[W * H];

static double hav_m(double la1, double lo1, double la2, double lo2) {
    double R = 6371008.8, p1 = la1 * M_PI / 180, p2 = la2 * M_PI / 180;
    double dp = (la2 - la1) * M_PI / 180, dl = (lo2 - lo1) * M_PI / 180;
    double a = sin(dp/2)*sin(dp/2) + cos(p1)*cos(p2)*sin(dl/2)*sin(dl/2);
    return 2 * R * asin(sqrt(a));
}

/* 原始数据（模拟从 PBF 抽出的"最小导航集"） */
static struct { int id; double lat, lon; } NODES[] = {
    {7001, 39.9000, 116.4000},
    {7002, 39.9010, 116.4010},
    {7003, 39.9020, 116.4020},
    {7004, 39.9005, 116.4035},
    {7005, 39.8990, 116.4025},
};
#define NN (int)(sizeof(NODES)/sizeof(NODES[0]))

static int way0[] = {7001, 7002, 7003, 7004};
static int way1[] = {7001, 7005, 7004};
static int *ways[] = {way0, way1};
static int  waylen[] = {4, 3};
#define NW (int)(sizeof(ways)/sizeof(ways[0]))

int main(void) {
    tut_cls(buf, W, H, 0xFFF4F1EA);

    /* ① id → 下标映射 */
    int idx[10000]; memset(idx, -1, sizeof idx);
    for (int i = 0; i < NN; i++) idx[NODES[i].id] = i;

    /* ② 收集所有（有向）边 → ③ 按源点归组填 CSR（压缩稀疏行） */
    int *off = (int *)calloc(NN + 1, sizeof(int));
    int *to  = (int *)malloc(sizeof(int) * 64);
    double *cost = (double *)malloc(sizeof(double) * 64);
    int *eu = (int *)malloc(sizeof(int) * 64);   /* 源 */
    int *ev = (int *)malloc(sizeof(int) * 64);   /* 汇 */
    double *ec = (double *)malloc(sizeof(double) * 64);
    int m = 0;
    for (int w = 0; w < NW; w++) {
        int len = waylen[w];
        for (int k = 0; k < len - 1; k++) {
            int a = idx[ways[w][k]], b = idx[ways[w][k+1]];
            double d = hav_m(NODES[a].lat, NODES[a].lon, NODES[b].lat, NODES[b].lon);
            eu[m] = a; ev[m] = b; ec[m] = d; m++;   /* 正向 */
            eu[m] = b; ev[m] = a; ec[m] = d; m++;   /* 反向（无向路双向通） */
        }
    }
    for (int i = 0; i < m; i++) off[eu[i] + 1]++;        /* 出度计数 */
    for (int i = 0; i < NN; i++) off[i + 1] += off[i];  /* 前缀和 → CSR 偏移 */
    int *cur = (int *)malloc(sizeof(int) * NN);
    for (int i = 0; i < NN; i++) cur[i] = off[i];
    for (int i = 0; i < m; i++) { int s = cur[eu[i]]++; to[s] = ev[i]; cost[s] = ec[i]; }

    /* 打印邻接表 */
    printf("CSR 路网：%d 节点 / %d 边\n", NN, m / 2);
    for (int i = 0; i < NN; i++) {
        printf("  node[%d](%d):", i, NODES[i].id);
        for (int e = off[i]; e < off[i+1]; e++) printf(" →%d(%.0fm)", to[e], cost[e]);
        printf("\n");
    }

    /* 画：节点 + 边（bbox 映射到画布） */
    double lat0 = 39.8980, lat1 = 39.9030, lon0 = 116.3990, lon1 = 116.4045;
    int px[NN], py[NN];
    for (int i = 0; i < NN; i++) {
        px[i] = (int)((NODES[i].lon - lon0) / (lon1 - lon0) * (W - 1));
        py[i] = (int)((lat1 - NODES[i].lat) / (lat1 - lat0) * (H - 1));
    }
    for (int i = 0; i < m; i += 2) {           /* 每条无向边只画一次（正向，跳过反向） */
        tut_line(buf, W, H, px[eu[i]], py[eu[i]], px[ev[i]], py[ev[i]], 0xFFB03A1F);
    }
    for (int i = 0; i < NN; i++) tut_fill_rect(buf, W, H, px[i]-3, py[i]-3, 7, 7, 0xFF1F6FB0);

    if (tut_write_bmp("out.bmp", buf, W, H) != 0) { fprintf(stderr, "write failed\n"); return 1; }
    printf("OK: wrote out.bmp — 5 节点路网（蓝=节点，红=边）\n");
    return 0;
}
