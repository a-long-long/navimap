/* tutorial/10_routing/main.c — 第 10 课 · 路由算法（双向加权 A* 的单体版）
 *
 * 累加内容：跨省双向加权 A* 的核心思想，用一个小图演示。
 *   - 状态主键 = coord_key_e7（坐标打包），不是 (省,下标)——跨省接边不需要映射表。
 *   - 启发式 h(u) = haversine(u,goal) * w；w 是旋钮：w 越大越贪心、展开越少但可能次优。
 *   - 用线性扫描的 open 集（节点少，足够演示）；真实工程用 4 叉最小堆 PQueue。
 *
 * 编译： gcc -std=c11 -O2 -o tut main.c && ./tut
 */
#include "../common.h"
#include <stdint.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define W 320
#define H 240
static uint32_t buf[W * H];

#define N 8
static double LAT[N]  = {39.90, 39.91, 39.92, 39.93, 39.90, 39.91, 39.92, 39.93};
static double LON[N]  = {116.40,116.40,116.40,116.40,116.44,116.44,116.44,116.44};

static double hav(double la1, double lo1, double la2, double lo2) {
    double R = 6371008.8, p1 = la1 * M_PI / 180, p2 = la2 * M_PI / 180;
    double dp = (la2 - la1) * M_PI / 180, dl = (lo2 - lo1) * M_PI / 180;
    double a = sin(dp/2)*sin(dp/2) + cos(p1)*cos(p2)*sin(dl/2)*sin(dl/2);
    return 2 * R * asin(sqrt(a));
}

/* 坐标即主键：OsmAnd 风格 id = (x31<<31)|y31，跨省接边零映射表 */
static uint64_t coord_key_e7(double lat, double lon) {
    int64_t x = (int64_t)((lon + 180.0) / 360.0 * (1LL << 31));
    int64_t y = (int64_t)((lat + 90.0)  / 180.0 * (1LL << 31));
    return ((uint64_t)x << 31) | (uint64_t)y;
}

/* —— 建 CSR（复用第 5 课的"先收集后归组"写法） —— */
static int EU[64], EV[64]; double EC[64]; int M = 0;
static int OFF[N + 1], TO[64], CUR[N];
static void build_graph(void) {
    /* 网格边：横向 + 纵向 + 一条斜对角（制造多解） */
    int edges[][2] = {{0,1},{1,2},{2,3},{4,5},{5,6},{6,7},{0,4},{1,5},{2,6},{3,7},{1,6}};
    int ne = (int)(sizeof(edges)/sizeof(edges[0]));
    M = 0;
    for (int i = 0; i < ne; i++) {
        int a = edges[i][0], b = edges[i][1];
        double d = hav(LAT[a], LON[a], LAT[b], LON[b]);
        EU[M]=a; EV[M]=b; EC[M]=d; M++;
        EU[M]=b; EV[M]=a; EC[M]=d; M++;
    }
    memset(OFF, 0, sizeof OFF);
    for (int i = 0; i < M; i++) OFF[EU[i] + 1]++;
    for (int i = 0; i < N; i++) OFF[i+1] += OFF[i];
    for (int i = 0; i < N; i++) CUR[i] = OFF[i];
    for (int i = 0; i < M; i++) { int s = CUR[EU[i]]++; TO[s] = EV[i]; }
}

static const double INF = 1e18;
static double g_goal(int *prev, int goal);   /* 回溯 prev 重算总代价（定义见文件末尾） */
/* 单向加权 A*（双向版是两套此结构对向跑，本课演示单侧核心） */
static int astar(int start, int goal, double w, int *prev, int *expanded) {
    static double g[N], f[N]; static char closed[N], inopen[N];
    for (int i = 0; i < N; i++) { g[i] = INF; f[i] = INF; closed[i] = 0; inopen[i] = 0; prev[i] = -1; }
    g[start] = 0; f[start] = hav(LAT[start], LON[start], LAT[goal], LON[goal]) * w; inopen[start] = 1;
    *expanded = 0;
    while (1) {
        /* 线性扫描取 open 中 f 最小 */
        int u = -1; double best = INF;
        for (int i = 0; i < N; i++) if (inopen[i] && f[i] < best) { best = f[i]; u = i; }
        if (u == -1) return 0;
        inopen[u] = 0; closed[u] = 1; (*expanded)++;
        if (u == goal) return 1;
        for (int e = OFF[u]; e < OFF[u+1]; e++) {
            int v = TO[e]; if (closed[v]) continue;
            double t = g[u] + EC[e];
            if (t < g[v]) {
                g[v] = t; prev[v] = u;
                f[v] = g[v] + hav(LAT[v], LON[v], LAT[goal], LON[goal]) * w;
                inopen[v] = 1;
            }
        }
    }
}

int main(void) {
    build_graph();
    int start = 0, goal = 7;
    printf("coord_key: start=%llx goal=%llx（坐标即主键，跨省零映射表）\n",
           (unsigned long long)coord_key_e7(LAT[start], LON[start]),
           (unsigned long long)coord_key_e7(LAT[goal], LON[goal]));

    int prev[N];
    for (double w = 1.0; w <= 2.0001; w += 1.0) {
        int exp; int ok = astar(start, goal, w, prev, &exp);
        printf("\nw=%.1f %s  展开=%d 代价=%.0f m\n", w, ok ? "找到" : "失败", exp, ok ? g_goal(prev,goal) : 0);
        if (ok) {
            int p = goal; printf("  路径: %d", p);
            while (prev[p] != -1) { p = prev[p]; printf(" <- %d", p); }
            printf("\n");
        }
    }
    /* 用 w=1.4 画路径 */
    int prev2[N]; int exp2; astar(start, goal, 1.4, prev2, &exp2);
    tut_cls(buf, W, H, 0xFFF4F1EA);
    double lat0=39.895,lat1=39.935,lon0=116.395,lon1=116.445;
    for (int i = 0; i < N; i++) {
        int x=(int)((LON[i]-lon0)/(lon1-lon0)*(W-1)), y=(int)((lat1-LAT[i])/(lat1-lat0)*(H-1));
        tut_fill_rect(buf,W,H,x-3,y-3,7,7,0xFF1F6FB0);
    }
    int p = goal;
    while (prev2[p] != -1) {
        int q = prev2[p];
        int x0=(int)((LON[p]-lon0)/(lon1-lon0)*(W-1)), y0=(int)((lat1-LAT[p])/(lat1-lat0)*(H-1));
        int x1=(int)((LON[q]-lon0)/(lon1-lon0)*(W-1)), y1=(int)((lat1-LAT[q])/(lat1-lat0)*(H-1));
        tut_line(buf,W,H,x0,y0,x1,y1,0xFFB03A1F);
        p = q;
    }
    if (tut_write_bmp("out.bmp", buf, W, H) != 0) { fprintf(stderr, "write failed\n"); return 1; }
    printf("OK: wrote out.bmp — 蓝=节点，红=w=1.4 路径\n");
    return 0;
}

/* 取 goal 的总代价（astar 里 g[goal] 在返回后不可见，这里用 prev 回溯重算） */
static double g_goal(int *prev, int goal) {
    double tot = 0; int p = goal;
    while (prev[p] != -1) { int q = prev[p]; for (int e = OFF[q]; e < OFF[q+1]; e++) if (TO[e]==p){tot+=EC[e];break;} p = q; }
    return tot;
}
