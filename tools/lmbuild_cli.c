/* lmbuild_cli —— 地标下界表的构建器（PLAN.md §48 / §49）
 *
 * 跑 N 次 lm_dijkstra（**距离模式**，单位米），把全图 5228 万个 (省,节点) 的
 * d(L→x) 按经纬度落进网格，每格只存 **最小值**（uint16，100 m 单位），写进
 * <图目录>/lm.bin。
 *
 * 下界推导（只需要"从地标出发"的一张表，实测单侧 == 双侧，见 §49.4）：
 *
 *     d(u→t) ≥ d(L→t) − d(L→u)
 *     其中 d(L→t) ≥ D(cel_t, L)                       （格内最小值，天然是下界）
 *           d(L→u) ≤ D(cel_u, L) + diam + 100 m       （格内任一点到 u 的上界）
 *     ⇒ bound = S(cel_t) − S(cel_u) − penalty_m
 *
 * ★ 取整一律向下：S = floor(最小值/100)。u 侧要下界 ⇒ 存小了才安全；
 *   t 侧那 100 m 已经算在 penalty_m 里。
 *
 * 用法：
 *   ./lmbuild_cli --graph=products/graph [--out=products/graph/lm.bin]
 *                 [--cell=3000]       格边长（米，纬度方向）
 *                 [--penalty=N]       罚项（米），默认 = 格对角 + 100
 *                 [--lm=lat,lon]      可重复，不给就用内置的 10 个
 *                 [--check=olat,olon,dlat,dlon]  建完顺手打印该 O-D 的表下界
 *                 [--stats]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "map/mapset.h"
#include "map/autobuild.h"
#include "route/route.h"
#include "route/lmtable.h"
#include "utils/plat.h"

#define M_PER_E7  0.0111195        /* 1e-7 度纬度 = 多少米 */
#define MAX_LM    64
#define MAX_CHK   64

static uint64_t mono_ms(void)
{
    return nm_now_ms();                /* 平台层：clock_gettime / QPC */
}

static const char *arg_val(const char *a, const char *prefix)
{
    size_t n = strlen(prefix);
    return strncmp(a, prefix, n) == 0 ? a + n : NULL;
}

/* 内置地标（§50 定 10 个；§52 扩到 30 个；列表里备到 50 个供 §52.6 对照）
 *
 * ★ 列表必须写在源码里，不能只靠命令行 `--lm=` 传：lm.bin 的 header 里**不存**
 *   地标坐标，一旦命令行参数没留档，那份表就再也重建不出来，直接破坏
 *   "products/ 全删后由 sync_maps.sh 确定性重建"这条约定（§52.1 真踩过）。
 *
 * 顺序有意义：`--nlm=N` 取**前 N 个**，所以任意前缀都是一个自洽的配置，
 *   10/20/30/50 各档的可比性来自"同一个有序列表"。
 *   - 第 1~10 位 = §50 发货的那 10 个，逐位未动 ⇒ `--nlm=10` 与旧表一致；
 *   - 第 11 位起 = 在覆盖区候选城市池上跑**贪心最远点**（每步选"到已选集合
 *     最小距离最大"的城市），让新增地标尽量摊开（生成脚本见 PLAN.md §52.1）。
 * ★ 因此**只能在末尾追加**，绝不能重排 —— 重排会让所有已发货的表失去复现性。
 */
static const double k_lm[][2] = {
    { 39.90, 116.40 },   /*  1 北京 */
    { 40.09, 113.30 },   /*  2 大同 */
    { 41.80, 123.43 },   /*  3 沈阳 */
    { 38.91, 121.61 },   /*  4 大连 */
    { 36.07, 120.38 },   /*  5 青岛 */
    { 34.75, 113.63 },   /*  6 郑州 */
    { 34.34, 108.94 },   /*  7 西安 */
    { 32.99, 112.53 },   /*  8 南阳 */
    { 37.87, 112.55 },   /*  9 太原 */
    { 35.10, 118.36 },   /* 10 临沂 */
    { 36.60, 109.49 },   /* 11 延安 */
    { 41.57, 120.45 },   /* 12 朝阳 */
    { 37.74, 115.68 },   /* 13 衡水 */
    { 33.07, 107.02 },   /* 14 汉中 */
    { 37.43, 118.67 },   /* 15 东营 */
    { 34.77, 111.19 },   /* 16 三门峡 */
    { 40.12, 124.37 },   /* 17 丹东 */
    { 39.94, 119.60 },   /* 18 秦皇岛 */
    { 38.29, 109.74 },   /* 19 榆林 */
    { 34.41, 115.65 },   /* 20 商丘 */
    { 32.69, 109.03 },   /* 21 安康 */
    { 40.95, 117.94 },   /* 22 承德 */
    { 32.15, 114.09 },   /* 23 信阳 */
    { 36.65, 117.00 },   /* 24 济南 */
    { 36.20, 113.12 },   /* 25 长治 */
    { 37.51, 122.12 },   /* 26 威海 */
    { 40.67, 122.23 },   /* 27 营口 */
    { 40.77, 114.88 },   /* 28 张家口 */
    { 34.36, 107.24 },   /* 29 宝鸡 */
    { 36.09, 111.52 },   /* 30 临汾 */
    /* ---- 31~50：同法继续贪心（§52.6 试 50 地标用）。★ 前 30 位不动 ⇒
       `--nlm=30` 的地标集指纹与产物仍然逐位不变 ---- */
    { 37.52, 111.14 },   /* 31 吕梁 */
    { 38.37, 117.87 },   /* 32 黄骅 */
    { 37.83, 120.76 },   /* 33 蓬莱 */
    { 39.69, 123.02 },   /* 34 庄河 */
    { 33.87, 110.09 },   /* 35 商洛 */
    { 36.70, 114.20 },   /* 36 武安 */
    { 40.62, 120.71 },   /* 37 兴城 */
    { 33.77, 113.19 },   /* 38 平顶山 */
    { 38.52, 114.99 },   /* 39 定州 */
    { 35.48, 110.44 },   /* 40 韩城 */
    { 34.86, 117.32 },   /* 41 枣庄 */
    { 39.06, 115.86 },   /* 42 容城 */
    { 36.46, 115.99 },   /* 43 聊城 */
    { 37.86, 113.58 },   /* 44 阳泉 */
    { 33.94, 116.44 },   /* 45 永城 */
    { 39.62, 122.00 },   /* 46 瓦房店 */
    { 40.20, 117.97 },   /* 47 遵化 */
    { 33.92, 106.52 },   /* 48 凤县 */
    { 34.05, 111.05 },   /* 49 卢氏 */
    { 40.73, 124.79 },   /* 50 宽甸 */
};
#define N_ALL_LM    ((int)(sizeof(k_lm) / sizeof(k_lm[0])))
/* 不带 --nlm 时用的地标个数 = **发货配置**（§52.6：50 个实测是"展开少 10%、
   总耗时打平"，多出来的 20 个对 43/50 条路线的结果毫无影响 ⇒ 30 是拐点）。
   列表里备到 50 个只是留给对照实验，别让"手跑一次 lmbuild_cli"意外改变配置。 */
#define N_DEFAULT_LM 30

/* 地标集指纹：FNV-1a 64 位，喂的是各坐标的 e7 整数（不是 double 的字节，
 * 否则 39.9 与 39.90000000000001 会算出两个指纹）。
 * 为什么需要它：lm.bin 的 header 里不存地标坐标，而 sync_maps.sh 原先的指纹
 * 只含**路网**数据 ⇒ 改了源码里的地标列表，指纹不变，重建被 SKIP，
 * 旧表静默留在原地（与 §51.5 同源的"静默过期"）。 */
static uint64_t lmlist_fp(const double lm[][2], int nlm)
{
    uint64_t h = 1469598103934665603ULL;
    for (int i = 0; i < nlm; i++) {
        int32_t v[2];
        v[0] = (int32_t)llround(lm[i][0] * 1e7);
        v[1] = (int32_t)llround(lm[i][1] * 1e7);
        const unsigned char *p = (const unsigned char *)v;
        for (size_t j = 0; j < sizeof(v); j++) {
            h ^= p[j];
            h *= 1099511628211ULL;
        }
    }
    return h;
}

static int parse_ll(const char *s, double *lat, double *lon)
{
    char *sp = strchr(s, ',');
    if (!sp) return -1;
    char buf[128];
    size_t n = (size_t)(sp - s);
    if (n == 0 || n >= sizeof(buf)) return -1;
    memcpy(buf, s, n); buf[n] = '\0';
    *lat = atof(buf);
    *lon = atof(sp + 1);
    return 0;
}

static int parse_ll4(const char *s, double v[4])
{
    const char *p = s;
    for (int i = 0; i < 4; i++) {
        char buf[128];
        if (i < 3) {
            const char *sp = strchr(p, ',');
            if (!sp) return -1;
            size_t n = (size_t)(sp - p);
            if (n == 0 || n >= sizeof(buf)) return -1;
            memcpy(buf, p, n); buf[n] = '\0';
            v[i] = atof(buf);
            p = sp + 1;
        } else {
            if (*p == '\0') return -1;
            v[i] = atof(p);
        }
    }
    return 0;
}

/* 把一个地标的全图距离聚合成一格一个 uint16 */
/* Router 在 route.h 里是不透明类型 ⇒ 图集从外面传进来 */
/* per-prov 的 [可达, 总数, 出度0]，用来盯"够不到的节点是不是数据本身的碎片" */
static uint64_t g_reach[64], g_total[64], g_deg0[64];

static void aggregate(const MapSet *ms, const LmDist *d, LmTable *t, uint32_t k,
                      uint64_t *n_hit, uint64_t *n_out)
{
    uint64_t hit = 0, out = 0;
    for (int p = 0; p < ms->n && p < 64; p++) {
        const MapSrc *s = &ms->srcs[p];
        /* 副本省：lm_dijkstra 不给它赋值（既不在转移里也不展开），全是 UNREACHABLE */
        if (s->dup_of >= 0) continue;
        uint64_t ncnt = s->nodes.hdr.rec_count;
        const uint8_t *base = s->nodes.data;
        const uint32_t *cost = d->cost + d->off[p];
        uint64_t reach = 0, deg0 = 0;
        for (uint64_t i = 0; i < ncnt; i++) {
            const uint8_t *nr = base + i * NM_NODE_BYTES;
            if (nm_get_u16(nr + NF_EDGE_COUNT) == 0) deg0++;
            uint32_t c = cost[i];
            if (c == LM_UNREACHABLE) continue;
            reach++;
            size_t ci = lm_cell_of(t, nm_get_i32(nr + NF_LAT), nm_get_i32(nr + NF_LON));
            if (ci == (size_t)-1) { out++; continue; }
            uint32_t m100 = c / 100000u;        /* (米×1000) → 米 → /100 */
            if (m100 > LM_MAXVAL) m100 = LM_MAXVAL;
            uint16_t *slot = &t->v[ci * t->h.nlm + k];
            if (m100 < (uint32_t)*slot) *slot = (uint16_t)m100;
            hit++;
        }
        g_reach[p] = reach;
        g_total[p] = ncnt;
        g_deg0[p]  = deg0;
    }
    if (n_hit) *n_hit = hit;
    if (n_out) *n_out = out;
}

int main(int argc, char **argv)
{
    const char *graph = "products/graph";
    const char *out   = NULL;
    double cell_m     = 3000.0;
    double penalty_m  = -1.0;
    double lm[MAX_LM][2];
    int nlm = 0;
    double chk[MAX_CHK][4];
    int nchk = 0;
    int want_stats = 0;
    int no_restr = 0;              /* --no-restr：建表时的全图 Dijkstra 不做转向限制（诊断） */
    int want_nlm = 0;                   /* --nlm=N：只用内置列表的前 N 个 */
    int want_lmfp = 0;                  /* --print-lmfp：只打印地标集指纹就退出 */

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v;
        if ((v = arg_val(a, "--graph=")))        graph = v;
        else if ((v = arg_val(a, "--out=")))     out   = v;
        else if ((v = arg_val(a, "--cell=")))    cell_m = atof(v);
        else if ((v = arg_val(a, "--penalty="))) penalty_m = atof(v);
        else if ((v = arg_val(a, "--nlm=")))     want_nlm = atoi(v);
        else if ((v = arg_val(a, "--lm="))) {
            if (nlm >= MAX_LM) { fprintf(stderr, "--lm 太多（上限 %d）\n", MAX_LM); return 2; }
            if (parse_ll(v, &lm[nlm][0], &lm[nlm][1]) != 0) {
                fprintf(stderr, "--lm 格式应为 lat,lon\n"); return 2;
            }
            nlm++;
        }
        else if ((v = arg_val(a, "--check="))) {
            if (nchk >= MAX_CHK) { fprintf(stderr, "--check 太多\n"); return 2; }
            if (parse_ll4(v, chk[nchk]) != 0) {
                fprintf(stderr, "--check 格式应为 olat,olon,dlat,dlon\n"); return 2;
            }
            nchk++;
        }
        else if (strcmp(a, "--print-lmfp") == 0) want_lmfp = 1;
        else if (strcmp(a, "--no-restr") == 0)    no_restr = 1;
        else if (strcmp(a, "--stats") == 0)      want_stats = 1;
        else { fprintf(stderr, "未知参数 %s\n", a); return 2; }
    }
    if (nlm == 0) {
        nlm = want_nlm > 0 ? want_nlm : N_DEFAULT_LM;
        if (nlm > N_ALL_LM) {
            fprintf(stderr, "--nlm=%d 超过内置地标数 %d\n", nlm, N_ALL_LM);
            return 2;
        }
        for (int i = 0; i < nlm; i++) { lm[i][0] = k_lm[i][0]; lm[i][1] = k_lm[i][1]; }
    } else if (want_nlm > 0) {
        fprintf(stderr, "--nlm 与 --lm 不能同时给\n");
        return 2;
    }
    if (want_lmfp) {
        /* 放在地标集确定之后：命令行 --lm 给的也算数，且不看参数先后顺序 */
        printf("lmfp %016llx\n", (unsigned long long)lmlist_fp(lm, nlm));
        return 0;
    }
    if (!(cell_m >= 100.0)) { fprintf(stderr, "--cell 应 >= 100 米\n"); return 2; }

    MapSet ms;
    AutoBuildOpt ab;
    memset(&ab, 0, sizeof(ab));
    ab.enable = 1;
    if (mapset_load_repair(&ms, graph, &ab, NULL) != 0) return 1;

    /* ★ 距离模式：表存米，用时才按 H_SPEED_KMH 换秒（时间口径下格对角罚项
       得按最慢限速算，会大到不能要 —— 见 lmtable.h 顶部注释） */
    RouteOpts opt = { .mode_time = 0, .bidir = 1, .heur_w = ROUTE_HEUR_W_DEFAULT,
                      .lm_no_restr = no_restr };
    Router *r = router_create(&ms, &opt);
    if (!r) { fprintf(stderr, "路由初始化失败\n"); return 1; }

    /* ---- 网格 ---- */
    uint32_t cell_e7 = (uint32_t)(cell_m / M_PER_E7 + 0.5);
    if (cell_e7 == 0) cell_e7 = 1;
    int64_t lat0 = ms.bbox[0], lon0 = ms.bbox[1], lat1 = ms.bbox[2], lon1 = ms.bbox[3];
    /* 向下取整到格边界，保证 bbox 落在网格内 */
    int64_t g_lat0 = lat0 - ((lat0 % (int64_t)cell_e7) + (int64_t)cell_e7) % (int64_t)cell_e7;
    int64_t g_lon0 = lon0 - ((lon0 % (int64_t)cell_e7) + (int64_t)cell_e7) % (int64_t)cell_e7;
    uint32_t nrow = (uint32_t)(((lat1 - g_lat0) / (int64_t)cell_e7) + 2);
    uint32_t ncol = (uint32_t)(((lon1 - g_lon0) / (int64_t)cell_e7) + 2);

    /* 格对角（上界）：经度方向按 bbox 最南纬度算（cos 最大 ⇒ 米数最多） */
    double lat_rad = (double)lat0 * 1e-7 * 3.14159265358979323846 / 180.0;
    double dlat_m  = (double)cell_e7 * M_PER_E7;
    double dlon_m  = dlat_m * cos(lat_rad);
    double diam    = sqrt(dlat_m * dlat_m + dlon_m * dlon_m);
    uint32_t pen   = (penalty_m >= 0.0) ? (uint32_t)(penalty_m + 0.5)
                                        : (uint32_t)(diam + 100.0 + 0.5);

    printf("网格          %ux%u 格，边长 %.0f m（纬度）/ %.0f m（经度 @%.1f°）\n",
           nrow, ncol, dlat_m, dlon_m, (double)lat0 * 1e-7);
    printf("格对角        %.0f m ⇒ 罚项 %u m\n", diam, pen);
    printf("表体积        %.0f KB（%u 格 × %d 地标 × 2 B）\n",
           (double)nrow * ncol * nlm * 2.0 / 1024.0, nrow * ncol, nlm);

    /* 图指纹：省数 + Σ 各省节点数（**含**副本省 —— 必须与 route.c 校验时
       的算法完全一致，否则每次加载都会误判"表不匹配"而把表丢掉）。 */
    uint64_t total_nodes = 0;
    for (int i = 0; i < ms.n; i++) total_nodes += ms.srcs[i].nodes.hdr.rec_count;
    printf("图指纹        %d 省 / %llu 节点\n", ms.n, (unsigned long long)total_nodes);
    printf("地标集指纹    %016llx（%d 个）\n", (unsigned long long)lmlist_fp(lm, nlm), nlm);

    LmTable t;
    if (lm_table_init(&t, (int32_t)g_lat0, (int32_t)g_lon0, cell_e7,
                      nrow, ncol, (uint32_t)nlm, pen,
                      (uint32_t)ms.n, total_nodes) != 0) {
        fprintf(stderr, "建表失败\n");
        router_destroy(r); mapset_free(&ms); return 1;
    }

    uint64_t t_all0 = mono_ms();
    uint64_t pop_all = 0;
    int bad = 0;
    for (int k = 0; k < nlm; k++) {
        RNode a;
        double da = 0;
        if (router_nearest(r, lm[k][0], lm[k][1], &a, &da) != 0) {
            printf("地标 %d 吸附失败 %.4f,%.4f\n", k, lm[k][0], lm[k][1]);
            bad++;
            continue;
        }
        LmDist d;
        uint64_t npop = 0, npush = 0;
        uint64_t t0 = mono_ms();
        int rc = lm_dijkstra(r, a, &d, &npop, &npush);
        uint64_t t1 = mono_ms();
        if (rc != 0) { printf("地标 %d Dijkstra 返回 %d\n", k, rc); bad++; continue; }
        uint64_t hit = 0, nout = 0;
        aggregate(&ms, &d, &t, (uint32_t)k, &hit, &nout);
        uint64_t t2 = mono_ms();
        lm_dist_free(&d);
        pop_all += npop;
        printf("地标 %2d  %-8s #%-9u 偏差 %5.1f m | Dijkstra %6llu ms 出堆 %9llu "
               "| 聚合 %5llu ms 命中 %9llu 出界 %llu\n",
               k, ms.srcs[a.prov].name, a.node, da,
               (unsigned long long)(t1 - t0), (unsigned long long)npop,
               (unsigned long long)(t2 - t1), (unsigned long long)hit,
               (unsigned long long)nout);
        fflush(stdout);
    }
    uint64_t t_all1 = mono_ms();

    /* 覆盖率：多少格有至少一个地标的值 */
    size_t ncell = t.ncell, nfull = 0, nempty = 0;
    for (size_t i = 0; i < ncell; i++) {
        int any = 0;
        for (uint32_t k = 0; k < t.h.nlm; k++)
            if (t.v[i * t.h.nlm + k] != LM_NONE) { any = 1; break; }
        if (any) nfull++; else nempty++;
    }
    printf("覆盖          %zu / %zu 格有数据（空 %zu，%.1f%%）\n",
           nfull, ncell, nempty, 100.0 * (double)nfull / (double)ncell);
    printf("总计          %llu ms，出堆合计 %llu\n",
           (unsigned long long)(t_all1 - t_all0), (unsigned long long)pop_all);
    printf("各省可达      省            可达        总数      出度0       占比\n");
    for (int p = 0; p < ms.n; p++) {
        if (ms.srcs[p].dup_of >= 0) continue;
        printf("              %-10s %10llu %10llu %10llu   %5.1f%%\n",
               ms.srcs[p].name, (unsigned long long)g_reach[p],
               (unsigned long long)g_total[p], (unsigned long long)g_deg0[p],
               g_total[p] ? 100.0 * (double)g_reach[p] / (double)g_total[p] : 0.0);
    }

    if (out == NULL) {
        char path[4096 + 32];
        snprintf(path, sizeof(path), "%s/lm.bin", graph);
        out = path;
    }
    if (lm_table_save(&t, out) != 0) {
        fprintf(stderr, "写 %s 失败\n", out);
        bad++;
    } else {
        printf("写出          %s\n", out);
    }

    /* ---- 顺手对拍：表下界 vs 需要外部给的真值 ---- */
    for (int i = 0; i < nchk; i++) {
        RNode no, nd;
        double d1 = 0, d2 = 0;
        if (router_nearest(r, chk[i][0], chk[i][1], &no, &d1) != 0 ||
            router_nearest(r, chk[i][2], chk[i][3], &nd, &d2) != 0) {
            printf("CHK %.4f,%.4f → %.4f,%.4f UNREACHABLE\n",
                   chk[i][0], chk[i][1], chk[i][2], chk[i][3]);
            continue;
        }
        int32_t olat = (int32_t)(chk[i][0] * 1e7 + 0.5), olon = (int32_t)(chk[i][1] * 1e7 + 0.5);
        int32_t dlat = (int32_t)(chk[i][2] * 1e7 + 0.5), dlon = (int32_t)(chk[i][3] * 1e7 + 0.5);
        size_t tc = lm_cell_of(&t, dlat, dlon);
        double b = lm_bound_m(&t, tc, olat, olon);
        printf("CHK %.4f,%.4f → %.4f,%.4f  表下界 %.1f km\n",
               chk[i][0], chk[i][1], chk[i][2], chk[i][3],
               b >= 0.0 ? b / 1000.0 : -1.0);
    }

    if (want_stats) {
        printf("峰值内存      %.0f MB\n", nm_peak_rss_mb());
    }

    lm_table_free(&t);
    router_destroy(r);
    mapset_free(&ms);
    printf(bad ? "LM-BUILD-FAIL（%d 个地标失败）\n" : "LM-BUILD-OK\n", bad);
    return bad ? 1 : 0;
}
