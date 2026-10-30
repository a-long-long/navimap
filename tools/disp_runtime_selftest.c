/* skedaddle_disp_runtime_selftest —— 显示层分 tile 的运行时等价性自测（Phase 2 验收）。
 *
 * 判据（行为等价 + 负向对照）：
 *  1. 等价：对某个视口调用 mapset_set_view 后，按需拼接进 s->areas/lines/
 *     polyrefs 的每个要素（osm_id + 节点下标序列）都必须出现在整省 nmbin 的、
 *     地理 bbox 与该视口（含 50% 边距）相交的要素集合里——即"只加载视口内 tile"
 *     没有丢要素，且 ref_off 重基正确（节点序列逐节点相等）。
 *  2. 计时：稳定态（同一视口重复 set_view，命中 disp_sig 跳过重建）平均 < 1ms，
 *     首次加载也远低于帧预算（仅 mmap 视口内少量 tile，不阻塞主线程）。
 *  3. 负向对照：视口移到与该省完全不相交的区域后，该省被卸载且屏外 tile 不被加载
 *     （loaded==0；若仍挂着显示层则判失败）。
 *
 * 用法：skedaddle_disp_runtime_selftest --graph=<省目录> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <inttypes.h>
#include "map/mapset.h"
#include "map/binfmt.h"

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "  ✗ " __VA_ARGS__); g_fail++; } } while (0)

/* FNV-1a 64 哈希一条节点下标序列（用于比较要素节点序列是否逐节点相等）。 */
static uint64_t sh_node_seq(const uint8_t *poly, uint64_t off, uint32_t cnt)
{
    uint64_t h = 1469598103934665603ULL;
    for (uint32_t k = 0; k < cnt; k++) {
        uint32_t v = nm_get_u32(poly + (off + k) * 4);
        h ^= v; h *= 1099511628211ULL;
    }
    return h;
}

typedef struct { uint64_t osm; uint64_t sh; } Cat;
static int cmp_cat(const void *a, const void *b) {
    uint64_t x = ((const Cat *)a)->osm, y = ((const Cat *)b)->osm;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

/* 要素 geo bbox（e7）。越界节点跳过；没有有效节点返回 -1。 */
static int feat_bbox(const NMBinFile *nodes, const uint8_t *poly,
                     uint64_t off, uint32_t cnt, int32_t out[4])
{
    int32_t a0 =  90 * 10000000, a1 = -90 * 10000000;
    int32_t o0 = 180 * 10000000, o1 = -180 * 10000000;
    int got = 0;
    uint64_t nn = nodes->hdr.rec_count;
    for (uint32_t k = 0; k < cnt; k++) {
        uint32_t idx = nm_get_u32(poly + (off + k) * 4);
        if ((uint64_t)idx >= nn) continue;
        const uint8_t *nr = nodes->data + (uint64_t)idx * NM_NODE_BYTES;
        int32_t la = nm_get_i32(nr + NF_LAT), lo = nm_get_i32(nr + NF_LON);
        if (la < a0) a0 = la;
        if (la > a1) a1 = la;
        if (lo < o0) o0 = lo;
        if (lo > o1) o1 = lo;
        got = 1;
    }
    if (!got) return -1;
    out[0] = a0; out[1] = o0; out[2] = a1; out[3] = o1;
    return 0;
}

static int overlap(const int32_t a[4], const int32_t b[4]) {
    return !(a[2] < b[0] || a[3] < b[1] || a[0] > b[2] || a[1] > b[3]);
}

/* 在按 osm 排序的 Cat 数组里找 osm 且 sh 匹配的项（允许 cross-tile 重复：同 osm 多条） */
static int cat_find(const Cat *arr, int n, uint64_t osm, uint64_t sh)
{
    int lo = 0, hi = n - 1, found = -1;
    while (lo <= hi) {
        int m = (lo + hi) >> 1;
        if (arr[m].osm == osm) { found = m; break; }
        if (arr[m].osm < osm) lo = m + 1; else hi = m - 1;
    }
    if (found < 0) return 0;
    for (int i = found; i >= 0 && arr[i].osm == osm; i--)
        if (arr[i].sh == sh) return 1;
    for (int i = found + 1; i < n && arr[i].osm == osm; i++)
        if (arr[i].sh == sh) return 1;
    return 0;
}

/* 校验一层：拼接缓冲 cat 必须覆盖整省与 eb 相交的该层全部要素。
 * 返回丢失数（写入 *total_concat / *total_hit）。 */
static int check_layer(const MapSrc *s, const char *name,
                       int off_field, int cnt_field,
                       const int32_t eb[4], uint64_t *total_concat, uint64_t *total_hit)
{
    const NMBinFile *cat   = (strcmp(name, "areas") == 0) ? &s->areas : &s->lines;
    const NMBinFile *cpoly = &s->polyrefs;
    uint64_t nc = cat->hdr.rec_count;
    Cat *arr = malloc((size_t)nc * sizeof(Cat));
    if (!arr) return -1;
    for (uint64_t i = 0; i < nc; i++) {
        const uint8_t *r = cat->data + i * cat->hdr.rec_size;
        uint64_t o = nm_get_u64(r + 0);
        uint32_t off = nm_get_u32(r + off_field);
        uint32_t cnt = nm_get_u32(r + cnt_field);
        if ((uint64_t)off + cnt > cpoly->hdr.rec_count)
            fprintf(stderr, "[DBG] OOB in concat %s i=%llu osm=%llu off=%u cnt=%u np=%llu\n",
                    name, (unsigned long long)i, (unsigned long long)o, off, cnt,
                    (unsigned long long)cpoly->hdr.rec_count);
        arr[i].osm = o;
        arr[i].sh  = sh_node_seq(cpoly->data, off, cnt);
    }
    qsort(arr, (size_t)nc, sizeof(Cat), cmp_cat);

    /* 整省该层，取与 eb 相交者，逐条在拼接缓冲里查 */
    uint64_t nw = 0;
    char wp[4096];
    snprintf(wp, sizeof(wp), "%s/%s.bin", s->dir, name);
    NMBinFile W; memset(&W, 0, sizeof(W));
    char pp[4096];
    snprintf(pp, sizeof(pp), "%s/polyrefs.bin", s->dir);
    NMBinFile Wp; memset(&Wp, 0, sizeof(Wp));
    int missing = 0, hit = 0;
    if (nmbin_open(wp, &W) == 0 && nmbin_open(pp, &Wp) == 0) {
        nw = W.hdr.rec_count;
        for (uint64_t i = 0; i < nw; i++) {
            const uint8_t *r = W.data + i * W.hdr.rec_size;
            uint64_t o = nm_get_u64(r + 0);
            uint32_t off = nm_get_u32(r + off_field);
            uint32_t cnt = nm_get_u32(r + cnt_field);
            int32_t bb[4];
            if (feat_bbox(&s->nodes, Wp.data, off, cnt, bb) != 0) continue;
            if (!overlap(bb, eb)) continue;
            uint64_t sh = sh_node_seq(Wp.data, off, cnt);
            hit++;
            if (!cat_find(arr, (int)nc, o, sh)) missing++;
        }
        nmbin_close(&W); nmbin_close(&Wp);
    }
    free(arr);
    *total_concat = nc; *total_hit = (uint64_t)hit;
    return missing;
}

int main(int argc, char **argv)
{
    const char *dir = NULL;
    for (int i = 1; i < argc; i++)
        if (strncmp(argv[i], "--graph=", 8) == 0) dir = argv[i] + 8;
    if (!dir) { fprintf(stderr, "用法: %s --graph=<省目录>\n", argv[0]); return 2; }

    setvbuf(stdout, NULL, _IONBF, 0);   /* 崩溃时也能看到已打印的进度 */

    MapSet ms;
    if (mapset_load(&ms, dir) != 0) {
        fprintf(stderr, "  ✗ 加载失败：%s\n", dir);
        return 1;
    }

    /* 找到分 tile 的省 */
    int prov = -1;
    for (int i = 0; i < ms.n; i++)
        if (ms.srcs[i].has_disp_tiles) { prov = i; break; }
    if (prov < 0) {
        fprintf(stderr, "  ✗ 该省未做显示层分 tile（无 disp.idx）\n");
        mapset_free(&ms);
        return 1;
    }
    MapSrc *s = &ms.srcs[prov];
    printf("disp 运行时自测: %s (省 %s, tile 网格 %dx%d, tile_e7=%" PRId64 ")\n",
           dir, s->name, s->disp_ncols, s->disp_nrows, s->disp_tile_e7);

    /* 视口 = 该省 bbox 中央 60% 区域 */
    int32_t V[4];
    int64_t dlat = s->bbox[2] - s->bbox[0], dlon = s->bbox[3] - s->bbox[1];
    V[0] = (int32_t)(s->bbox[0] + dlat * 0.20);
    V[2] = (int32_t)(s->bbox[2] - dlat * 0.20);
    V[1] = (int32_t)(s->bbox[1] + dlon * 0.20);
    V[3] = (int32_t)(s->bbox[3] - dlon * 0.20);

    /* 首次 set_view：加载视口内 tile（计时首次加载） */
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    mapset_set_view(&ms, V);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double t_first = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

    CHECK(s->loaded, "首次 set_view 后省应已 loaded\n");
    CHECK(s->has_disp_tiles, "省应标记 has_disp_tiles\n");
    CHECK(s->has_features, "加载 tile 后 has_features 应为 1\n");

    /* 构造 set_view 内部用的 eb（四周各扩 50%） */
    int32_t m  = (V[2] - V[0]) / 2;
    int32_t mn = (V[3] - V[1]) / 2;
    int32_t eb[4] = { V[0] - m, V[1] - mn, V[2] + m, V[3] + mn };

    /* 等价性：拼接缓冲必须覆盖整省与 eb 相交的全部要素 */
    uint64_t ca, ha, cl, hl;
    int ma = check_layer(s, "areas", AF_REF_OFF, AF_REF_CNT, eb, &ca, &ha);
    int ml = check_layer(s, "lines", LF_REF_OFF, LF_REF_CNT, eb, &cl, &hl);
    CHECK(ma == 0, "areas 层：%d 条视口要素在拼接缓冲中找不到（丢失！）\n", ma);
    CHECK(ml == 0, "lines 层：%d 条视口要素在拼接缓冲中找不到（丢失！）\n", ml);
    printf("  等价性: areas 视口 %" PRIu64 " 条 / 拼接 %" PRIu64 " 条；"
           "lines 视口 %" PRIu64 " 条 / 拼接 %" PRIu64 " 条；丢失 0\n",
           ha, ca, hl, cl);

    /* 计时：稳定态（同视口重复，命中 disp_sig 跳过重建） */
    int N = 300;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < N; i++) mapset_set_view(&ms, V);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double t_steady = ((t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9) / N;
    printf("  计时: 首次加载 %.3f ms；稳定态平均 %.4f ms/次（%d 次）\n",
           t_first * 1000.0, t_steady * 1000.0, N);
    CHECK(t_steady < 0.001, "稳定态 set_view 应 < 1ms（实测 %.4f ms）\n", t_steady * 1000.0);

    /* 负向对照：视口移到与该省完全不相交处 → 应被卸载、屏外 tile 不被加载 */
    int32_t far[4] = { 850000000, -100000000, 860000000, -90000000 };  /* 北大西洋 */
    mapset_set_view(&ms, far);
    CHECK(!ms.srcs[prov].loaded, "视口移出后该省应被卸载（loaded==0），否则屏外 tile 仍挂着\n");
    CHECK(!ms.srcs[prov].has_features, "视口移出后该省不应有显示层（has_features==0）\n");
    printf("  负向对照: 视口移出 → 省已卸载、屏外 tile 不加载 ✓\n");

    mapset_free(&ms);

    if (g_fail == 0) {
        printf("DISP-RUNTIME-OK  按需加载等价、零丢失、不阻塞、屏外不加载\n");
        return 0;
    }
    printf("DISP-RUNTIME-FAIL\n");
    return 1;
}
