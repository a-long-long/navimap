#include "map/mapset.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <inttypes.h>

#include "map/coordkey.h"
#include "utils/geo.h"
#include "utils/plat.h"

/* 显示层分 tile 的前向声明（定义在 src_close 之后） */
static int  disp_open(MapSrc *s);
static void src_free_disp(MapSrc *s);
static int  src_load_disp_tiles(MapSrc *s, const int32_t eb[4]);

/* ------------------------------------------------------------------ */
/* 单省加载                                                              */
/* ------------------------------------------------------------------ */

static int manifest_bbox(const char *dir, int32_t out[4])
{
    char p[4096];
    if (nm_path_join(p, sizeof(p), dir, "manifest.txt") != 0) return -1;
    FILE *f = fopen(p, "r");
    if (!f) return -1;
    char line[512];
    int got = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "bbox_e7", 7) == 0) {
            int v[4];
            if (sscanf(line, "bbox_e7 %d %d %d %d", &v[0], &v[1], &v[2], &v[3]) == 4) {
                memcpy(out, v, sizeof(v));
                got = 1;
            }
            break;
        }
    }
    fclose(f);
    return got ? 0 : -1;
}

static void src_close(MapSrc *s)
{
    nmbin_close(&s->nodes);
    nmbin_close(&s->edges);
    if (s->has_restr) nmbin_close(&s->restrictions);
    if (s->has_features) {
        nmbin_close(&s->lines);
        nmbin_close(&s->areas);
        nmbin_close(&s->polyrefs);
    }
    if (s->has_tiles) {
        nmbin_close(&s->tiles);
        nmbin_close(&s->edgelist);
    }
    name_pool_close(&s->np);
    nmbin_close(&s->lanes);
    name_pool_close(&s->lnp);
    src_free_disp(s);     /* 回收 tile 拼接缓冲（普通省空操作） */
    free(s->area_bbox);
    free(s->line_bbox);
    memset(s, 0, sizeof(*s));
}

/* ------------------------------------------------------------------ */
/* 显示层分 tile：运行时按视口只加载相交 tile，拼接进 areas/lines/polyrefs */
/* ------------------------------------------------------------------ */

/* 解析 disp.idx 的网格参数。成功（该省有 disp.idx 且格式合法）返回 0，
 * 并把 tile_e7/lat0/lon0/ncols/nrows 存进 s、置 has_disp_tiles=1；
 * 失败（无 disp.idx 或解析错误）返回 -1，调用方回退到整省 nmbin。 */
static int disp_open(MapSrc *s)
{
    char p[4096];
    snprintf(p, sizeof(p), "%s/disp.idx", s->dir);
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    int got = 0;
    char line[256];
    int64_t tile_e7 = 0, lat0 = 0, lon0 = 0;
    long ncols = 0, nrows = 0;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "tile_e7 %" PRId64, &tile_e7) == 1) continue;
        if (sscanf(line, "lat0 %" PRId64, &lat0) == 1) continue;
        if (sscanf(line, "lon0 %" PRId64, &lon0) == 1) continue;
        if (sscanf(line, "ncols %ld", &ncols) == 1) continue;
        if (sscanf(line, "nrows %ld", &nrows) == 1) continue;
        if (strncmp(line, "ntiles", 6) == 0) { got = 1; break; }
    }
    fclose(f);
    if (!got || ncols <= 0 || nrows <= 0 || tile_e7 <= 0) return -1;
    s->has_disp_tiles = 1;
    s->disp_tile_e7 = tile_e7;
    s->disp_lat0    = lat0;
    s->disp_lon0    = lon0;
    s->disp_ncols   = (int)ncols;
    s->disp_nrows   = (int)nrows;
    return 0;
}

/* 释放拼接缓冲（手 malloc 的 dareas/dlines/dpoly，nmbin_close 不会 free）。
 * 对无 tile 的省是空操作（dareas==NULL 且 has_disp_tiles==0），不动 has_features，
 * 以免误清普通省由 nmbin 管理的整省显示层。 */
static void src_free_disp(MapSrc *s)
{
    free(s->dareas); s->dareas = NULL; s->dea = 0;
    free(s->dlines); s->dlines = NULL; s->del = 0;
    free(s->dpoly);  s->dpoly  = NULL; s->depo = 0;
    if (s->has_disp_tiles) {
        s->areas.data = NULL; s->areas.mapped = 0;
        s->lines.data = NULL; s->lines.mapped = 0;
        s->polyrefs.data = NULL; s->polyrefs.mapped = 0;
        s->has_features = 0;
        s->disp_sig = 0;
    }
}

/* 按视口 eb（含边距的 [lat0,lon0,lat1,lon1]）加载相交的显示层 tile，
 * 拼接进 s->areas/lines/polyrefs（mapped=0 的手 malloc 缓冲），
 * 使 paint_areas/paint_lines 零改动（它们直接读这三个缓冲）。
 *
 * 每个 tile 的 polyrefs 是独立段，ref_off 以本 tile 的 polyrefs 为基；
 * 拼接时把 ref_off 重基到整段 dpoly 的 u32 偏移。disp_sig 命中则跳过重建
 * （视口在已加载 tile 集合内不重做，避免每帧 churn）。返回 0 成功 / -1 失败。 */
static int src_load_disp_tiles(MapSrc *s, const int32_t eb[4])
{
    if (!s->has_disp_tiles) return -1;

    int64_t te7 = s->disp_tile_e7, la0 = s->disp_lat0, lo0 = s->disp_lon0;
    int ncols = s->disp_ncols, nrows = s->disp_nrows;

    /* 第一遍：读 disp.idx，收集与 eb 相交的 tile (c,r)，并算集合签名 */
    int cap = 16, nneed = 0;
    int32_t *need = malloc((size_t)cap * 2 * sizeof(int32_t));
    if (!need) return -1;
    uint64_t sig = 0;

    char idxp[4096];
    snprintf(idxp, sizeof(idxp), "%s/disp.idx", s->dir);
    FILE *f = fopen(idxp, "rb");
    if (!f) { free(need); src_free_disp(s); return -1; }
    char line[256];
    int in_list = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "ntiles", 6) == 0) { in_list = 1; continue; }
        if (!in_list) continue;
        long c, r;
        if (sscanf(line, "%ld %ld", &c, &r) != 2) continue;
        if (c < 0 || r < 0 || c >= ncols || r >= nrows) continue;
        /* tile (c,r)：列=纬度、行=经度（与 write_disp_tiles 一致） */
        int32_t tla0 = (int32_t)(la0 + (int64_t)c * te7);
        int32_t tla1 = (int32_t)(la0 + ((int64_t)c + 1) * te7);
        int32_t tlo0 = (int32_t)(lo0 + (int64_t)r * te7);
        int32_t tlo1 = (int32_t)(lo0 + ((int64_t)r + 1) * te7);
        if (tla1 < eb[0] || tlo1 < eb[1] || tla0 > eb[2] || tlo0 > eb[3]) continue;
        if (nneed >= cap) {
            cap *= 2;
            int32_t *t = realloc(need, (size_t)cap * 2 * sizeof(int32_t));
            if (!t) { fclose(f); free(need); src_free_disp(s); return -1; }
            need = t;
        }
        need[2 * nneed] = (int32_t)c; need[2 * nneed + 1] = (int32_t)r; nneed++;
        sig ^= (((uint64_t)(unsigned)(c * 0x9E3779B1u)) << 1) ^
               ((uint64_t)(unsigned)(r * 0x85EBCA77u));
    }
    fclose(f);
    sig = (sig << 32) | (uint64_t)(unsigned)nneed;

    if (nneed == 0) {
        /* 视口在省内但该处无要素：清空缓冲，保持 has_features=0 自然跳过 */
        if (s->disp_sig != (int64_t)sig) { src_free_disp(s); s->disp_sig = (int64_t)sig; }
        free(need);
        return 0;
    }
    if (s->disp_sig == (int64_t)sig && s->dareas) {
        free(need);          /* 同一 tile 集合，缓冲仍有效，跳过重建 */
        return 0;
    }

    /* 逐 tile 打开→复制→关闭（同时最多 3 个文件 mmap，绕开 Windows 的映射句柄上限；
     * 之前一次性打开全部 tile 会在 ~168 个后耗尽句柄）。拼接缓冲随增长动态 realloc。 */
    uint8_t *da = NULL, *dl = NULL, *dp = NULL;
    size_t cap_a = 0, cap_l = 0, cap_p = 0;
    uint64_t na = 0, nl = 0, np = 0;   /* 拼接后的记录条数（最终 rec_count） */
    int fail = 0;

    for (int k = 0; k < nneed; k++) {
        int32_t c = need[2 * k], r = need[2 * k + 1];
        char pa[4096], pl[4096], pp[4096];
        snprintf(pa, sizeof(pa), "%s/t_%ld_%ld.areas", s->dir, (long)c, (long)r);
        snprintf(pl, sizeof(pl), "%s/t_%ld_%ld.lines", s->dir, (long)c, (long)r);
        snprintf(pp, sizeof(pp), "%s/t_%ld_%ld.polyrefs", s->dir, (long)c, (long)r);
        NMBinFile A, L, P;
        memset(&A, 0, sizeof(A)); memset(&L, 0, sizeof(L)); memset(&P, 0, sizeof(P));
        if (nmbin_open(pa, &A) != 0 || nmbin_open(pl, &L) != 0 || nmbin_open(pp, &P) != 0) {
            nmbin_close(&A); nmbin_close(&L); nmbin_close(&P);
            fail++; continue;
        }
        uint64_t ka = A.hdr.rec_count, kl = L.hdr.rec_count, kp = P.hdr.rec_count;

        /* polyrefs：追加到 dp（本 tile 落在偏移 np 处，areas/lines 据此重基 ref_off） */
        if (kp) {
            size_t need_p = (size_t)(np + kp) * 4;
            if (need_p > cap_p) {
                cap_p = need_p * 2 + 4096;
                uint8_t *t = realloc(dp, cap_p);
                if (!t) { nmbin_close(&A); nmbin_close(&L); nmbin_close(&P); goto oom; }
                dp = t;
            }
            memcpy(dp + (size_t)np * 4, P.data, (size_t)kp * 4);
        }
        /* areas：复制并把 ref_off 重基到整段 dp（np = 本 tile polyrefs 起点） */
        if (ka) {
            size_t need_a = (size_t)(na + ka) * NM_AREA_BYTES;
            if (need_a > cap_a) {
                cap_a = need_a * 2 + 4096;
                uint8_t *t = realloc(da, cap_a);
                if (!t) { nmbin_close(&A); nmbin_close(&L); nmbin_close(&P); goto oom; }
                da = t;
            }
            uint8_t *out = da + (size_t)na * NM_AREA_BYTES;
            for (uint64_t j = 0; j < ka; j++) {
                const uint8_t *rec = A.data + j * NM_AREA_BYTES;
                memcpy(out, rec, NM_AREA_BYTES);
                uint32_t ro = nm_get_u32(rec + AF_REF_OFF);
                nm_put_u32(out + AF_REF_OFF, (uint32_t)((uint64_t)np + ro));
                out += NM_AREA_BYTES;
            }
        }
        /* lines：同 areas */
        if (kl) {
            size_t need_l = (size_t)(nl + kl) * NM_LINE_BYTES;
            if (need_l > cap_l) {
                cap_l = need_l * 2 + 4096;
                uint8_t *t = realloc(dl, cap_l);
                if (!t) { nmbin_close(&A); nmbin_close(&L); nmbin_close(&P); goto oom; }
                dl = t;
            }
            uint8_t *out = dl + (size_t)nl * NM_LINE_BYTES;
            for (uint64_t j = 0; j < kl; j++) {
                const uint8_t *rec = L.data + j * NM_LINE_BYTES;
                memcpy(out, rec, NM_LINE_BYTES);
                uint32_t ro = nm_get_u32(rec + LF_REF_OFF);
                nm_put_u32(out + LF_REF_OFF, (uint32_t)((uint64_t)np + ro));
                out += NM_LINE_BYTES;
            }
        }
        nmbin_close(&A); nmbin_close(&L); nmbin_close(&P);
        na += ka; nl += kl; np += kp;
    }
    free(need);

    if (fail == nneed) {            /* 全打不开：保留旧缓冲，下次再试，不闪退 */
        free(da); free(dl); free(dp);
        s->disp_sig = (int64_t)sig;
        return -1;
    }

    /* 释放旧拼接缓冲 + 懒填 bbox 缓存（多边形集合变了必须重建） */
    free(s->dareas); free(s->dlines); free(s->dpoly);
    free(s->area_bbox); s->area_bbox = NULL;
    free(s->line_bbox); s->line_bbox = NULL;

    s->dareas = da; s->dea = (size_t)na * NM_AREA_BYTES;
    s->dlines = dl; s->del = (size_t)nl * NM_LINE_BYTES;
    s->dpoly  = dp; s->depo = (size_t)np * 4;

    s->areas.data = da; s->areas.hdr.rec_count = na;
    s->areas.hdr.rec_size = NM_AREA_BYTES; s->areas.mapped = 0; s->areas.data_len = s->dea;
    s->lines.data = dl; s->lines.hdr.rec_count = nl;
    s->lines.hdr.rec_size = NM_LINE_BYTES; s->lines.mapped = 0; s->lines.data_len = s->del;
    s->polyrefs.data = dp; s->polyrefs.hdr.rec_count = np;
    s->polyrefs.hdr.rec_size = 4; s->polyrefs.mapped = 0; s->polyrefs.data_len = s->depo;
    s->has_features = (na + nl > 0) ? 1 : 0;
    s->disp_sig = (int64_t)sig;
    return 0;

oom:
    free(da); free(dl); free(dp); free(need);
    s->disp_sig = (int64_t)sig;
    return -1;
}

/* 只打开文件、不碰身份（name/dir/bbox/dup_of/loaded 由调用方保持）。
 * 重载走这个，身份不变；首次打开由 src_open 先置身份再调它。 */
static int src_open_files(MapSrc *s)
{
    s->dmap_inflight = 0;   /* 重载 = 重新尝试解码（清掉失败标记） */
    s->dmap_failed   = 0;
    char p[4096];
    char err[256];

    /* 必需文件：nodes/edges。加载即校验版本与自洽性——这是此前一直欠着的
     * 加载端防线：此前 app_init 只看 magic，格式升级或半截文件会静默画错图。 */
    snprintf(p, sizeof(p), "%s/nodes.bin", s->dir);
    if (nmbin_open(p, &s->nodes) != 0) {
        fprintf(stderr, "图集: %s 打不开 %s，跳过\n", s->name, p);
        return -1;
    }
    if (nmbin_check(&s->nodes, err, sizeof(err)) != 0) {
        fprintf(stderr, "图集: %s nodes.bin 校验失败（%s），跳过该省\n", s->name, err);
        goto fail;
    }
    snprintf(p, sizeof(p), "%s/edges.bin", s->dir);
    if (nmbin_open(p, &s->edges) != 0) {
        fprintf(stderr, "图集: %s 打不开 %s，跳过\n", s->name, p);
        goto fail;
    }
    if (nmbin_check(&s->edges, err, sizeof(err)) != 0) {
        fprintf(stderr, "图集: %s edges.bin 校验失败（%s），跳过该省\n", s->name, err);
        goto fail;
    }

    /* 转向限制可选：路由用，没有就当省内没有任何禁转 */
    snprintf(p, sizeof(p), "%s/restrictions.bin", s->dir);
    s->has_restr = (nmbin_open(p, &s->restrictions) == 0);
    if (s->has_restr && nmbin_check(&s->restrictions, err, sizeof(err)) != 0) {
        fprintf(stderr, "图集: %s restrictions.bin 校验失败（%s），按无限制处理\n",
                s->name, err);
        nmbin_close(&s->restrictions);
        s->has_restr = 0;
    }

    /* 线/面要素可选：三者齐备才算有。
     * ★ PBF 模式下**不从 nmbin 打开**这三个文件（它们可能根本没落盘），
     *   改由 mapset_ensure_display 从 PBF 运行时解码。
     * ★ 有显示层分 tile（disp.idx）的省也**不从 nmbin 打开整省**这三个文件，
     *   改为 mapset_set_view 按视口只加载相交 tile 并拼接（见 src_load_disp_tiles），
     *   避免为屏外一条边解码/映射整省。 */
    if (s->pbf_display_mode) {
        s->has_features = 0;      /* 等解码后再置 1 */
    } else if (disp_open(s) == 0) {
        s->has_features = 0;      /* 等 set_view 按视口加载 tile 后再置 1 */
    } else {
        s->has_features = 1;
        snprintf(p, sizeof(p), "%s/lines.bin", s->dir);
        if (nmbin_open(p, &s->lines) != 0) s->has_features = 0;
        if (s->has_features) {
            snprintf(p, sizeof(p), "%s/areas.bin", s->dir);
            if (nmbin_open(p, &s->areas) != 0) s->has_features = 0;
        }
        if (s->has_features) {
            snprintf(p, sizeof(p), "%s/polyrefs.bin", s->dir);
            if (nmbin_open(p, &s->polyrefs) != 0) s->has_features = 0;
        }
        if (!s->has_features)
            fprintf(stderr, "图集: %s 无 lines/areas/polyrefs，只显示路网\n", s->name);
    }

    /* 空间索引可选：两者齐备才算有 */
    s->has_tiles = 1;
    snprintf(p, sizeof(p), "%s/tiles.bin", s->dir);
    if (nmbin_open(p, &s->tiles) != 0) s->has_tiles = 0;
    if (s->has_tiles) {
        snprintf(p, sizeof(p), "%s/edgelist.bin", s->dir);
        if (nmbin_open(p, &s->edgelist) != 0) s->has_tiles = 0;
    }

    /* 路名池可选 */
    snprintf(p, sizeof(p), "%s/names.bin", s->dir);
    if (name_pool_open(&s->np, p) != 0)
        fprintf(stderr, "图集: %s 无路名池（不画路名标注）\n", s->name);

    /* 车道指示可选：lanes.bin + lane_names.bin 齐备才有 */
    s->has_lanes = 1;
    snprintf(p, sizeof(p), "%s/lanes.bin", s->dir);
    if (nmbin_open(p, &s->lanes) != 0 || nmbin_check(&s->lanes, err, sizeof(err)) != 0)
        s->has_lanes = 0;
    if (s->has_lanes) {
        snprintf(p, sizeof(p), "%s/lane_names.bin", s->dir);
        if (name_pool_open(&s->lnp, p) != 0) s->has_lanes = 0;
    }

    /* 视口裁剪用的 bbox。没有 manifest（旧目录）就置全域，宁可多画。 */
    if (manifest_bbox(s->dir, s->bbox) != 0) {
        s->bbox[0] = -900000000; s->bbox[1] = -1800000000;
        s->bbox[2] =  900000000; s->bbox[3] =  1800000000;
    }
    return 0;

fail:
    /* nmbin_close / name_pool_close 对全零结构都是安全空操作，
       逐个 close 一遍即可，不用记录哪些开过 */
    nmbin_close(&s->nodes);
    nmbin_close(&s->edges);
    nmbin_close(&s->restrictions);
    nmbin_close(&s->lines);
    nmbin_close(&s->areas);
    nmbin_close(&s->polyrefs);
    nmbin_close(&s->tiles);
    nmbin_close(&s->edgelist);
    name_pool_close(&s->np);
    nmbin_close(&s->lanes);
    name_pool_close(&s->lnp);
    return -1;
}

/* 首次打开：先清身份再置 name/dir，再开文件。 */
static int src_open(MapSrc *s, const char *dir, const char *name)
{
    memset(s, 0, sizeof(*s));
    s->dup_of = -1;          /* 0 是合法的"被 srcs[0] 包含"，所以空值用 -1 */
    snprintf(s->name, sizeof(s->name), "%s", name);
    snprintf(s->dir, sizeof(s->dir), "%s", dir);
    return src_open_files(s);
}

/* 卸载：关文件 + 释放缓存，但保留身份（name/dir/bbox/dup_of/loaded）。
 * 与 src_close（整体释放，mapset_free 用）不同，这里省份槽位继续存在，
 * 只是文件不再 mmap——下次 set_view 命中视口时 src_open_files 重新打开。 */
static void src_unload(MapSrc *s)
{
    if (s->dmap_inflight) return;   /* 后台解码中：保留 nodes 映射，待解码完再卸 */
    nmbin_close(&s->nodes);
    nmbin_close(&s->edges);
    if (s->has_restr) nmbin_close(&s->restrictions);
    /* ★ PBF 解码出来的显示层是 malloc 缓冲，nmbin_close 只管 mmap，
     * 所以要单独 pbfmap_free（否则泄漏）。 */
    if (s->dmap_from_pbf) {
        pbfmap_free(&s->dmap);
        s->dmap_from_pbf = 0;
    }
    /* ★ 显示层分 tile 的拼接缓冲也是手 malloc（mapped=0），nmbin_close 不会释放，
     * 这里先回收；普通省 has_disp_tiles=0，src_free_disp 是空操作。 */
    src_free_disp(s);
    if (s->has_features) {
        nmbin_close(&s->lines);
        nmbin_close(&s->areas);
        nmbin_close(&s->polyrefs);
    }
    if (s->has_tiles) {
        nmbin_close(&s->tiles);
        nmbin_close(&s->edgelist);
    }
    name_pool_close(&s->np);
    nmbin_close(&s->lanes);
    name_pool_close(&s->lnp);
    s->has_lanes = 0;
    free(s->area_bbox);
    free(s->line_bbox);
    memset(&s->nodes, 0, sizeof(s->nodes));
    memset(&s->edges, 0, sizeof(s->edges));
    memset(&s->restrictions, 0, sizeof(s->restrictions));
    memset(&s->lines, 0, sizeof(s->lines));
    memset(&s->areas, 0, sizeof(s->areas));
    memset(&s->polyrefs, 0, sizeof(s->polyrefs));
    memset(&s->tiles, 0, sizeof(s->tiles));
    memset(&s->edgelist, 0, sizeof(s->edgelist));
    memset(&s->np, 0, sizeof(s->np));
    s->has_restr = 0;
    s->has_features = 0;
    s->has_tiles = 0;
    s->area_bbox = NULL;
    s->line_bbox = NULL;
    /* name/dir/bbox/dup_of 保留 */
}

/* ------------------------------------------------------------------ */
/* 图集                                                                  */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* 数据包含关系检测                                                      */
/*                                                                      */
/* 各省 pbf 是同一份 OSM 数据按多边形裁出来的，地形上被包围的行政区会被      */
/* 邻省整块包含：实测北京 ⊂ 河北 100%、天津 ⊂ 河北 100%（osmid 两两求交，   */
/* 且"只加载河北"与"只加载北京"在同位置渲染出的 BMP md5 完全相同）。        */
/*                                                                      */
/* 这对路由是灾难：每个路口在两省各有一个副本，搜索状态翻倍，且同一个瓦片     */
/* 永远有 2 个省覆盖 → 每次展开都要重建缝合索引。所以加载时就把这件事认出来。*/
/*                                                                      */
/* 判据（三条都要满足，宁可漏判也别误判——误判会真的丢路）：                 */
/*   1. bbox 完全落在对方 bbox 内；2. 节点数不多于对方；                   */
/*   3. 抽样 1000 个节点，≥99% 能在对方的空间索引里找到同坐标节点。         */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* edgelist 条目读取（v1 / v2 通吃）                                    */
/*                                                                      */
/* v1（8B）只有 {node_idx, edge_idx}，取坐标要再去 nodes.bin 随机读；    */
/* v2（28B）自带两端坐标 + 节点下标。这里统一出口，下面三条路径（重复省   */
/* 检测 / 归属查询 / 瓦片遍历）都不用关心目录是哪一版。                  */
/* ------------------------------------------------------------------ */

static inline int xl_is_v2(const MapSrc *s)
{
    return s->edgelist.hdr.rec_size == NM_XL2_BYTES;
}

/* 条目 pos 的 from 端坐标键；下标越界返回 ~0（调用方据 ck 比较自然判掉） */
static uint64_t xl_from_key(const MapSrc *s, uint64_t pos)
{
    const uint8_t *xl = s->edgelist.data + pos * s->edgelist.hdr.rec_size;
    if (xl_is_v2(s))
        return coord_key_e7(nm_get_i32(xl + XL2_FROM_LAT),
                            nm_get_i32(xl + XL2_FROM_LON));
    uint32_t from = nm_get_u32(xl + XL_NODE);
    if ((uint64_t)from >= s->nodes.hdr.rec_count) return ~0ULL;
    const uint8_t *nr = s->nodes.data + (uint64_t)from * NM_NODE_BYTES;
    return coord_key_e7(nm_get_i32(nr + NF_LAT), nm_get_i32(nr + NF_LON));
}

/* 条目 pos 的 {from 节点下标, CSR 边下标}；越界返回 0 */
static int xl_entry(const MapSrc *s, uint64_t pos, uint32_t *from, uint32_t *eidx)
{
    const uint8_t *xl = s->edgelist.data + pos * s->edgelist.hdr.rec_size;
    if (xl_is_v2(s)) {
        *from = nm_get_u32(xl + XL2_NODE);
        *eidx = nm_get_u32(xl + XL2_EDGE);
    } else {
        *from = nm_get_u32(xl + XL_NODE);
        *eidx = nm_get_u32(xl + XL_EDGE);
    }
    return ((uint64_t)*from < s->nodes.hdr.rec_count &&
            (uint64_t)*eidx < s->edges.hdr.rec_count);
}

static int tile_has_coord(const MapSrc *s, int tx, int ty, uint64_t ck)
{
    if (!s->has_tiles) return 0;
    const uint8_t *td = s->tiles.data;
    uint32_t minx = nm_get_u32(td + TF_EDGE_OFF);
    uint32_t miny = nm_get_u32(td + TF_EDGE_CNT);
    uint32_t nx   = nm_get_u32(td + NM_TILE_REC_BYTES + TF_EDGE_OFF);
    uint32_t ny   = nm_get_u32(td + NM_TILE_REC_BYTES + TF_EDGE_CNT);
    if (tx < (int)minx || ty < (int)miny ||
        tx >= (int)(minx + nx) || ty >= (int)(miny + ny))
        return 0;
    if ((uint64_t)nx * ny > s->tiles.hdr.rec_count - 2) return 0;
    size_t t = ((size_t)(ty - miny) * nx) + (tx - minx);
    const uint8_t *dir = td + (2 + t) * NM_TILE_REC_BYTES;
    uint32_t off = nm_get_u32(dir + TF_EDGE_OFF);
    uint32_t cnt = nm_get_u32(dir + TF_EDGE_CNT);
    uint64_t ne = s->edgelist.hdr.rec_count;
    for (uint32_t k = 0; k < cnt; k++) {
        uint64_t pos = (uint64_t)off + k;
        if (pos >= ne) break;
        if (xl_from_key(s, pos) == ck) return 1;
    }
    return 0;
}

int mapset_detail_prov(const MapSet *ms, int32_t lat_e7, int32_t lon_e7,
                       int fallback)
{
    double wx, wy;
    nm_mercator((double)lat_e7 / 1e7, (double)lon_e7 / 1e7, &wx, &wy);
    double Z = (double)(1u << NM_TILE_Z);
    int tx = (int)(wx * Z), ty = (int)(wy * Z);
    uint64_t ck = coord_key_e7(lat_e7, lon_e7);

    for (int i = 0; i < ms->n; i++) {
        const MapSrc *s = &ms->srcs[i];
        if (!s->loaded) continue;              /* 该省已卸载（视口外），跳过 */
        if (s->dup_of < 0) continue;            /* 只看被包含的小省 */
        if (lat_e7 < s->bbox[0] || lon_e7 < s->bbox[1] ||
            lat_e7 > s->bbox[2] || lon_e7 > s->bbox[3])
            continue;
        if (tile_has_coord(s, tx, ty, ck)) return i;
    }
    return fallback;
}

#define DUP_SAMPLE 1000

static void detect_duplicates(MapSet *ms)
{
    for (int i = 0; i < ms->n; i++) ms->srcs[i].dup_of = -1;

    for (int i = 0; i < ms->n; i++) {
        MapSrc *a = &ms->srcs[i];
        if (!a->has_tiles) continue;
        uint64_t na = a->nodes.hdr.rec_count;
        if (na == 0) continue;
        for (int j = 0; j < ms->n; j++) {
            if (i == j) continue;
            MapSrc *b = &ms->srcs[j];
            if (!b->has_tiles) continue;
            uint64_t nb = b->nodes.hdr.rec_count;
            if (nb < na) continue;
            if (a->bbox[0] < b->bbox[0] || a->bbox[1] < b->bbox[1] ||
                a->bbox[2] > b->bbox[2] || a->bbox[3] > b->bbox[3])
                continue;                       /* bbox 不包含 */

            /* ★ 采样必须从 edgelist 取，不能从 nodes 取：瓦片索引里登记的
               是每条边的 from 节点，只作为 to 出现的节点（单行道终点等）
               根本不在索引里，拿它们采样会把命中率拉到 60% 上下，误判为
               "不包含"。路由真正关心的是有出边的路口，正好是 edgelist。 */
            uint64_t nea = a->edgelist.hdr.rec_count;
            if (nea == 0) continue;
            uint64_t stride = nea / DUP_SAMPLE;
            if (stride == 0) stride = 1;
            uint64_t tested = 0, hit = 0;
            for (uint64_t n = 0; n < nea && tested < DUP_SAMPLE; n += stride) {
                uint64_t ck = xl_from_key(a, n);
                if (ck == ~0ULL) continue;
                int32_t la = coord_key_lat(ck), lo = coord_key_lon(ck);
                double wx, wy;
                nm_mercator((double)la / 1e7, (double)lo / 1e7, &wx, &wy);
                double Z = (double)(1u << NM_TILE_Z);
                int tx = (int)(wx * Z), ty = (int)(wy * Z);
                tested++;
                if (tile_has_coord(b, tx, ty, ck)) hit++;
            }
            if (tested >= 100 && hit * 100 >= tested * 99) {
                a->dup_of = j;
                printf("图集: %s 的数据被 %s 完全包含（抽样 %llu/%llu 命中），"
                       "路由缝合时跳过其副本\n",
                       a->name, b->name,
                       (unsigned long long)hit, (unsigned long long)tested);
                break;
            }
        }
    }
}

static int mapset_add(MapSet *ms, const char *dir, const char *name)
{
    if (ms->n == ms->cap) {
        int ncap = ms->cap ? ms->cap * 2 : 8;
        MapSrc *ns = realloc(ms->srcs, (size_t)ncap * sizeof(MapSrc));
        if (!ns) return -1;
        ms->srcs = ns;
        ms->cap = ncap;
    }
    MapSrc *s = &ms->srcs[ms->n];
    if (src_open(s, dir, name) != 0) return -1;
    s->loaded = 1;

    if (ms->n == 0) {
        memcpy(ms->bbox, s->bbox, sizeof(ms->bbox));
    } else {
        if (s->bbox[0] < ms->bbox[0]) ms->bbox[0] = s->bbox[0];
        if (s->bbox[1] < ms->bbox[1]) ms->bbox[1] = s->bbox[1];
        if (s->bbox[2] > ms->bbox[2]) ms->bbox[2] = s->bbox[2];
        if (s->bbox[3] > ms->bbox[3]) ms->bbox[3] = s->bbox[3];
    }
    ms->n++;
    printf("图集: + %-12s 节点 %-10llu 边 %-10llu\n",
           s->name,
           (unsigned long long)s->nodes.hdr.rec_count,
           (unsigned long long)s->edges.hdr.rec_count);
    return 0;
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

int mapset_load(MapSet *ms, const char *graph_dir)
{
    memset(ms, 0, sizeof(*ms));

    /* PBF 显示层后台解码：建锁 + 初始空闲任务槽（job_prov=-1 表示无在途）。 */
    ms->dmap_mtx = nm_mutex_create();
    ms->job_prov = -1;

    /* 单省目录：下面直接有 nodes.bin */
    char p[4096];
    if (nm_path_join(p, sizeof(p), graph_dir, "nodes.bin") == 0 &&
        nm_file_exists(p)) {
        const char *nm = nm_base_name(graph_dir);
        if (mapset_add(ms, graph_dir, nm) != 0) {
            fprintf(stderr, "图集: %s 加载失败\n", graph_dir);
            return -1;
        }
        return 0;
    }

    /* 父目录：扫描含 manifest.txt 的子目录。目录枚举顺序由文件系统决定，
       先收集名字再排序，保证加载顺序（以及日志）可复现。 */
    char **names = NULL;
    int nn = 0, cap = 0;
    NmDir *d = nm_dir_open(graph_dir);
    if (!d) {
        fprintf(stderr, "图集: 既不是省目录也没有 nodes.bin：%s\n", graph_dir);
        return -1;
    }
    const char *de;
    while ((de = nm_dir_next(d)) != NULL) {
        if (de[0] == '.') continue;
        /* 跳过 sync_maps 的过渡目录：.tmp.* 是转换中的半成品，.old.* 是已换下、
           删除失败时留下的旧产物。两者都含 manifest.txt，不排除就会被当成
           一个"省"加载进来 —— 同一份数据装两遍（内存翻倍 + 渲染重复）。 */
        if (strstr(de, ".tmp.")) continue;
        if (strstr(de, ".old.")) continue;
        if (nm_path_join(p, sizeof(p), graph_dir, de) != 0) continue;
        if (!nm_is_dir(p)) continue;
        char mf[4096];
        if (nm_path_join(mf, sizeof(mf), p, "manifest.txt") != 0) continue;
        if (!nm_file_exists(mf)) continue;    /* 没有产物指纹的不算省 */
        if (nn == cap) {
            cap = cap ? cap * 2 : 16;
            char **t = realloc(names, (size_t)cap * sizeof(char *));
            if (!t) break;
            names = t;
        }
        names[nn++] = nm_strdup(de);
    }
    nm_dir_close(d);

    if (nn == 0) {
        fprintf(stderr, "图集: %s 下没有含 manifest.txt 的省目录\n", graph_dir);
        return -1;
    }
    qsort(names, (size_t)nn, sizeof(char *), cmp_str);

    int ok = 0;
    for (int i = 0; i < nn; i++) {
        if (nm_path_join(p, sizeof(p), graph_dir, names[i]) == 0 &&
            mapset_add(ms, p, names[i]) == 0) ok++;
        free(names[i]);
    }
    free(names);

    if (ok == 0) {
        fprintf(stderr, "图集: %s 下一个省都没加载成功\n", graph_dir);
        return -1;
    }
    printf("图集: 共 %d 个省\n", ms->n);
    detect_duplicates(ms);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 按需加载（进省才开、离省卸载）                                        */
/*                                                                      */
/* 省份下标固定（槽位 == 省 id），卸载只关文件、保留身份，因此路由/转向里  */
/* 写进结果的 prov 下标始终有效。视口驱动的 mapset_set_view 每帧由渲染循环  */
/* 调用：把与视口（含边距）相交的省份打开、其余卸载。迭代函数跳过          */
/* !loaded，卸载的省份不会被画、也不会被查询命中。                        */
/* ------------------------------------------------------------------ */

static int bbox_overlap(const int32_t a[4], const int32_t b[4])
{
    return !(a[2] < b[0] || a[3] < b[1] || a[0] > b[2] || a[1] > b[3]);
}

/* 后台解码线程的"发布 / 派发 / worker"（定义见文件末尾 PBF 显示层节）。 */
static void mapset_publish_display(MapSet *ms);
static void mapset_dispatch_display(MapSet *ms, int prov);
static void dmap_worker(void *arg);

void mapset_set_view(MapSet *ms, const int32_t vb[4])
{
    if (!ms || ms->n == 0) return;
    mapset_publish_display(ms);   /* 把后台线程已解完的省挂上显示层 */
    /* 边距：视口四周各扩 50%，让即将滚入的相邻省提前打开，也覆盖吸附/导航预览 */
    int32_t m  = (vb[2] - vb[0]) / 2;
    int32_t mn = (vb[3] - vb[1]) / 2;
    int32_t eb[4] = { vb[0] - m, vb[1] - mn, vb[2] + m, vb[3] + mn };

    for (int i = 0; i < ms->n; i++) {
        MapSrc *s = &ms->srcs[i];
        int ov = bbox_overlap(s->bbox, eb);
        if (ov && !s->loaded) {
            s->loaded = (src_open_files(s) == 0);
        } else if (!ov && s->loaded) {
            src_unload(s);
            s->loaded = 0;
        }
        /* PBF 显示层：视口进省 ⇒ 后台线程异步解码（见 dmap_worker）。
         * 不再在主线程同步解码（旧逻辑会阻塞渲染 0.6~4.4s）。解码完成前
         * 该省 has_features=0，渲染自然跳过（空白），就绪后下一帧经 publish
         * 挂上、立即显示——也就是"渐进式加载"，零卡顿。
         * ★ 仅 pbf_display_mode 的省走这条；显示层分 tile 的省（has_disp_tiles）
         * pbf_display_mode=0，走下面的同步按需加载。 */
        if (ov && ms->pbf_dir[0] && s->pbf_display_mode) {
            if (!s->dmap_from_pbf) {
                mapset_dispatch_display(ms, i);   /* 无任务在途才派发，异步 */
            } else {
                s->dmap_used = ++ms->dmap_clock;  /* 已就绪才刷新 LRU */
            }
        }

        /* 显示层分 tile：视口进省 ⇒ 按视口只加载相交 tile 并拼接进
         * areas/lines/polyrefs（同步、仅 mmap 视口内少量 tile，轻量不卡）。
         * 不进省（!ov）已由 src_unload 释放拼接缓冲。失败不致命：
         * 该省 has_features=0，本帧自然不画显示层。 */
        if (ov && s->has_disp_tiles) {
            src_load_disp_tiles(s, eb);
        }
    }

    /* LRU 淘汰：驻留超上限时，把**不在视口内**且最久未用的省显示层丢掉。 */
    if (ms->pbf_dir[0] && ms->dmap_lru_cap > 0) {
        for (;;) {
            int live = 0;
            for (int i = 0; i < ms->n; i++)
                if (ms->srcs[i].dmap_from_pbf) live++;
            if (live <= ms->dmap_lru_cap) break;
            /* 找 LRU 且不在视口内的 */
            int victim = -1;
            uint64_t oldest = UINT64_MAX;
            for (int i = 0; i < ms->n; i++) {
                MapSrc *s = &ms->srcs[i];
                if (!s->dmap_from_pbf) continue;
                if (bbox_overlap(s->bbox, eb)) continue;   /* 视口内，不淘汰 */
                if (s->dmap_used < oldest) { oldest = s->dmap_used; victim = i; }
            }
            if (victim < 0) break;    /* 全在视口内 ⇒ 不淘汰 */
            MapSrc *s = &ms->srcs[victim];
            pbfmap_free(&s->dmap);
            memset(&s->lines, 0, sizeof(s->lines));
            memset(&s->areas, 0, sizeof(s->areas));
            memset(&s->polyrefs, 0, sizeof(s->polyrefs));
            s->has_features = 0;
            s->dmap_from_pbf = 0;
        }
    }
}

void mapset_ensure_loaded(MapSet *ms, int prov)
{
    if (!ms || prov < 0 || prov >= ms->n) return;
    MapSrc *s = &ms->srcs[prov];
    if (!s->loaded)
        s->loaded = (src_open_files(s) == 0);
}

/* ------------------------------------------------------------------ */
/* PBF 显示层：从 PBF 运行时解码 + LRU                                   */
/* ------------------------------------------------------------------ */

int mapset_enable_pbf_display(MapSet *ms, const char *pbf_dir, int lru_cap)
{
    if (!ms || !pbf_dir) return -1;
    if (!nm_is_dir(pbf_dir)) return -1;
    snprintf(ms->pbf_dir, sizeof(ms->pbf_dir), "%s", pbf_dir);
    ms->dmap_lru_cap = lru_cap > 0 ? lru_cap : 3;
    ms->dmap_clock = 0;
    /* 标记所有省走 PBF 显示层（src_open_files 据此不打开 nmbin 的
     * lines/areas/polyrefs）。已打开的省要重开一次才生效。 */
    for (int i = 0; i < ms->n; i++) {
        MapSrc *s = &ms->srcs[i];
        s->pbf_display_mode = 1;
        if (s->loaded) {
            /* 释放 nmbin 的显示层（若有），改由 PBF 提供 */
            if (!s->dmap_from_pbf && s->has_features) {
                nmbin_close(&s->lines);
                nmbin_close(&s->areas);
                nmbin_close(&s->polyrefs);
            }
            s->has_features = 0;
        }
    }
    /* 预热：立即起后台解码线程（线程空转直到派发任务）。省得首进省时才 spawn，
     * 让解码在视口进省的第一帧就已在途，进一步压低感知延迟。 */
    if (ms->dmap_mtx && !ms->dmap_worker_started) {
        nm_thread_spawn(dmap_worker, ms);
        ms->dmap_worker_started = 1;
    }
    return 0;
}

int mapset_ensure_display(MapSet *ms, int prov)
{
    if (!ms || prov < 0 || prov >= ms->n) return -1;
    if (!ms->pbf_dir[0]) return -1;
    MapSrc *s = &ms->srcs[prov];
    if (s->dmap_from_pbf) return 0;      /* 已就绪 */
    if (!s->loaded) return -1;            /* 路由 nodes 没打开，没有下标空间 */

    char pbf[4096];
    char nm[512];
    int k = snprintf(nm, sizeof(nm), "%s.osm.pbf", s->name);
    if (k <= 0 || (size_t)k >= sizeof(nm) ||
        nm_path_join(pbf, sizeof(pbf), ms->pbf_dir, nm) != 0) return -1;
    if (!nm_file_exists(pbf)) return -1;

    PbfMap m;
    if (pbfmap_decode(pbf, &s->nodes, &m, NULL, NULL) != 0) return -1;

    /* 接管缓冲：把 dmap 的三个 NMBinFile 挂到 src 上，渲染端零改动。
     * （内容与 nmbin 落盘文件逐字节相同，8 省实测。） */
    s->lines    = m.lines;
    s->areas    = m.areas;
    s->polyrefs = m.polyrefs;
    s->dmap     = m;
    s->dmap_from_pbf = 1;
    s->has_features = 1;
    s->dmap_used = ++ms->dmap_clock;
    return 0;
}

/* 取某省后台解码进度（0..100）。无在途/未启用时返回 0。
 * 读受 dmap_mtx 保护，与 worker 的进度回调串行，避免撕裂读。 */
int mapset_dmap_progress(const MapSet *ms, int prov)
{
    if (!ms || prov < 0 || prov >= ms->n) return 0;
    const MapSrc *s = &ms->srcs[prov];
    int p = 0;
    if (ms->dmap_mtx) {
        nm_mutex_lock(ms->dmap_mtx);
        p = s->dmap_progress;
        nm_mutex_unlock(ms->dmap_mtx);
    } else {
        p = s->dmap_progress;
    }
    return p;
}

/* ------------------------------------------------------------------ */
/* PBF 显示层：后台解码线程（消除同步解码对渲染的 0.6~4.4s 阻塞）       */
/* ------------------------------------------------------------------ */

/* 把 worker 已解完的省挂上显示层。只在主线程（set_view 顶部）调用。
 * 与 dmap_worker 通过 dmap_mtx 串行化，作业槽的读改写都在锁内完成。 */
static void mapset_publish_display(MapSet *ms)
{
    if (!ms->dmap_mtx) return;
    nm_mutex_lock(ms->dmap_mtx);
    if (!ms->job_done) { nm_mutex_unlock(ms->dmap_mtx); return; }

    int      p   = ms->job_done_prov;
    PbfMap   res = ms->job_result;
    int      ok  = ms->job_ok;
    ms->job_done = 0;
    memset(&ms->job_result, 0, sizeof(PbfMap));   /* 缓冲所有权已交出 */
    nm_mutex_unlock(ms->dmap_mtx);

    if (p < 0 || p >= ms->n) return;
    MapSrc *s = &ms->srcs[p];
    s->dmap_inflight = 0;                          /* 解码结束，解除 pin */
    if (ok) {
        s->lines    = res.lines;
        s->areas    = res.areas;
        s->polyrefs = res.polyrefs;
        s->dmap     = res;
        s->dmap_from_pbf = 1;
        s->has_features  = 1;
        s->dmap_progress = 100;     /* 完成 */
        s->dmap_used = ++ms->dmap_clock;
    } else {
        s->dmap_failed = 1;   /* 解码失败：不再重试，等重载（src_open_files 清标记） */
    }
}

/* 派发一省去后台解码：仅当没有任务在途 / 待发布 / 已失败 / 已就绪时才派发。
 * 派发时置 dmap_inflight=1，让 src_unload 在解码期间保留 nodes 映射
 * （worker 要读 s->nodes 做节点下标查表）。 */
static void mapset_dispatch_display(MapSet *ms, int prov)
{
    if (prov < 0 || prov >= ms->n) return;
    if (!ms->dmap_mtx) { mapset_ensure_display(ms, prov); return; }  /* 无锁则退回同步 */
    MapSrc *s = &ms->srcs[prov];
    if (s->dmap_from_pbf || s->dmap_failed || s->dmap_inflight) return;

    nm_mutex_lock(ms->dmap_mtx);
    if (ms->job_prov >= 0 || ms->job_done) {       /* 已有在途/待发布，本帧不重复派发 */
        nm_mutex_unlock(ms->dmap_mtx);
        return;
    }
    ms->job_prov = prov;
    s->dmap_inflight = 1;
    s->dmap_progress = 0;       /* 解码开始，进度从 0 爬升 */
    if (!ms->dmap_worker_started) {
        nm_thread_spawn(dmap_worker, ms);
        ms->dmap_worker_started = 1;
    }
    nm_mutex_unlock(ms->dmap_mtx);
}

/* 后台解码进度回调上下文：把 pbfmap_decode 回报的 frac 写到对应省的
 * dmap_progress（受 dmap_mtx 保护，与渲染端读取串行）。 */
typedef struct { MapSet *ms; int prov; } DmapCbCtx;

static void dmap_progress_cb(void *ud, double frac)
{
    DmapCbCtx *c = (DmapCbCtx *)ud;
    MapSet *ms = c->ms;
    MapSrc *s = &ms->srcs[c->prov];
    if (ms->dmap_mtx) nm_mutex_lock(ms->dmap_mtx);
    s->dmap_progress = (int)(frac * 100.0 + 0.5);
    if (ms->dmap_mtx) nm_mutex_unlock(ms->dmap_mtx);
}

/* 后台解码线程：轮询任务槽（无 condvar，解码本身 0.6~4.4s，4ms 轮询延迟可忽略），
 * 在锁外做耗时解码，完成后把结果放入 job_result 并置 job_done。pbfmap_decode
 * 仅用局部状态（Buf/StrPool/本地数组），无文件级可变全局 ⇒ 线程安全。 */
static void dmap_worker(void *arg)
{
    MapSet *ms = (MapSet *)arg;
    for (;;) {
        nm_sleep_ms(4);
        nm_mutex_lock(ms->dmap_mtx);
        if (ms->dmap_quit) { nm_mutex_unlock(ms->dmap_mtx); break; }
        int p = ms->job_prov;
        int have = (p >= 0 && !ms->job_done);
        nm_mutex_unlock(ms->dmap_mtx);
        if (!have) continue;

        int    ok = 0;
        PbfMap m; memset(&m, 0, sizeof(m));
        if (p < ms->n && ms->srcs[p].loaded) {
            MapSrc *s = &ms->srcs[p];
            char pbf[4096], nm_[512];
            int k = snprintf(nm_, sizeof(nm_), "%s.osm.pbf", s->name);
            if (k > 0 && (size_t)k < sizeof(nm_) &&
                nm_path_join(pbf, sizeof(pbf), ms->pbf_dir, nm_) == 0 &&
                nm_file_exists(pbf)) {
                DmapCbCtx cbctx = { ms, p };
                ok = (pbfmap_decode(pbf, &s->nodes, &m, dmap_progress_cb,
                                   &cbctx) == 0);
            }
        }

        nm_mutex_lock(ms->dmap_mtx);
        if (ms->dmap_quit) {            /* 让出：放弃半成品，不发布 */
            if (!ok) pbfmap_free(&m);
            nm_mutex_unlock(ms->dmap_mtx);
            break;
        }
        ms->job_result    = m;
        ms->job_ok        = ok;
        ms->job_done      = 1;
        ms->job_done_prov = p;
        ms->job_prov      = -1;         /* 释放任务槽，主线程可派发下一省 */
        nm_mutex_unlock(ms->dmap_mtx);
    }
    ms->dmap_stopped = 1;
}

void mapset_free(MapSet *ms)
{
    if (!ms) return;
    /* 先让 worker 退出，避免它读已释放的 srcs / nodes。detached 线程无法 join，
     * 故置 quit 后轮询 dmap_stopped（上限 ~10s，正常只在数 ms 内停）。 */
    if (ms->dmap_worker_started) {
        ms->dmap_quit = 1;
        for (int i = 0; i < 2000 && !ms->dmap_stopped; i++)
            nm_sleep_ms(5);
    }
    if (ms->dmap_mtx) {
        nm_mutex_destroy(ms->dmap_mtx);
        ms->dmap_mtx = NULL;
    }
    for (int i = 0; i < ms->n; i++)
        src_close(&ms->srcs[i]);
    free(ms->srcs);
    memset(ms, 0, sizeof(*ms));
}

void mapset_totals(const MapSet *ms, uint64_t *nodes, uint64_t *edges)
{
    uint64_t n = 0, e = 0;
    for (int i = 0; i < ms->n; i++) {
        if (!ms->srcs[i].loaded) continue;
        n += ms->srcs[i].nodes.hdr.rec_count;
        e += ms->srcs[i].edges.hdr.rec_count;
    }
    if (nodes) *nodes = n;
    if (edges) *edges = e;
}

const char *mapset_edge_lanes(const MapSet *ms, int prov, uint32_t edge)
{
    if (!ms || prov < 0 || prov >= ms->n) return NULL;
    const MapSrc *s = &ms->srcs[prov];
    if (!s->loaded || !s->has_lanes || !s->lnp.ready || !s->lanes.data) return NULL;
    if ((uint64_t)edge >= s->lanes.hdr.rec_count) return NULL;
    uint32_t sid = nm_get_u32(s->lanes.data + (size_t)edge * 4);
    if (sid == 0 || sid > s->lnp.n) return NULL;       /* 0 = 无车道信息 */
    return s->lnp.blob + s->lnp.off[sid];
}

/* 按 tile 跨省查询。tiles.bin 的 payload 布局见 binfmt.h：
 * rec0={minx,miny}，rec1={nx,ny}，其后每 tile 一条 {edge_off, edge_cnt}，
 * edge_off 指向 edgelist.bin（{node_idx, edge_idx} 对，按 tile 排序）。 */
int mapset_query_tile(const MapSet *ms, int tx, int ty, MapsetTileFn fn, void *ud)
{
    if (!ms || !fn) return 0;
    int total = 0;
    for (int i = 0; i < ms->n; i++) {
        const MapSrc *s = &ms->srcs[i];
        if (!s->loaded || !s->has_tiles) continue;

        const uint8_t *td = s->tiles.data;
        uint32_t minx = nm_get_u32(td + TF_EDGE_OFF);
        uint32_t miny = nm_get_u32(td + TF_EDGE_CNT);
        uint32_t nx   = nm_get_u32(td + NM_TILE_REC_BYTES + TF_EDGE_OFF);
        uint32_t ny   = nm_get_u32(td + NM_TILE_REC_BYTES + TF_EDGE_CNT);
        if (tx < (int)minx || ty < (int)miny ||
            tx >= (int)(minx + nx) || ty >= (int)(miny + ny))
            continue;                       /* 该省没这片 */

        size_t t = ((size_t)(ty - miny) * nx) + (tx - minx);
        uint64_t ntile_dir = s->tiles.hdr.rec_count - 2;
        if ((uint64_t)nx * ny > ntile_dir) continue;
        const uint8_t *dir = td + (2 + t) * NM_TILE_REC_BYTES;
        uint32_t off = nm_get_u32(dir + TF_EDGE_OFF);
        uint32_t cnt = nm_get_u32(dir + TF_EDGE_CNT);

        uint64_t ne = s->edgelist.hdr.rec_count;
        for (uint32_t k = 0; k < cnt; k++) {
            uint64_t pos = (uint64_t)off + k;
            if (pos >= ne) break;
            uint32_t from, eidx;
            if (!xl_entry(s, pos, &from, &eidx)) continue;
            fn(s, from, eidx, ud);
            total++;
        }
    }
    return total;
}
