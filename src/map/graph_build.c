#include "map/graph_build.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pbf/pbf_stream.h"
#include "pbf/pbf_block.h"
#include "map/road_tags.h"
#include "map/binfmt.h"
#include "utils/strpool.h"
#include "utils/geo.h"

/* ------------------------------------------------------------------ */
/* 变长数组                                                             */
/* ------------------------------------------------------------------ */
typedef struct { char *p; size_t n, cap, esz; } Buf;

static int buf_reserve(Buf *b, size_t add)
{
    size_t need = b->n + add;
    if (need <= b->cap) return 0;
    size_t ncap = b->cap ? b->cap : 64;
    while (ncap < need) ncap *= 2;
    char *np = realloc(b->p, ncap * b->esz);
    if (!np) return -1;
    b->p = np; b->cap = ncap;
    return 0;
}

static void buf_free(Buf *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }

/* ------------------------------------------------------------------ */
/* 建图上下文                                                           */
/* ------------------------------------------------------------------ */
typedef struct {
    int64_t  osm_id;
    uint32_t ref_off, ref_cnt;
    uint8_t  cls, oneway, no_access;
    uint16_t flags;
    uint16_t speed, lanes;
    uint32_t name_id;
} WayRec;

typedef struct {
    int64_t from_way, via_node, to_way;
    int64_t via_way;   /* via=way 限制里 via 的 way id；0 = via 是节点 */
    uint8_t kind;      /* 0 禁止 1 必须 */
    uint8_t dirmask;   /* 1 左 2 直 4 右 8 掉头 */
} PendRestr;

/* 线要素（步行道/铁路/水系线）与面要素共用 ref 存储：
 * ref_off/ref_cnt 指向 g->refs（与 WayRec 同池），落盘前统一转成
 * polyrefs.bin 的 u32 内部下标。 */
typedef struct {
    int64_t  osm_id;
    uint32_t ref_off, ref_cnt;
    uint8_t  cls;
    uint8_t  flags;
    uint32_t name_id;
} LineRec;                      /* 24B */

typedef struct {
    int64_t  osm_id;
    uint32_t ref_off, ref_cnt;
    uint8_t  kind;
} AreaRec;                      /* 20B */

typedef struct {
    Buf ways;       /* WayRec */
    Buf refs;       /* int64_t */
    Buf restr;      /* PendRestr */
    Buf lines;      /* LineRec */
    Buf areas;      /* AreaRec */

    /* 第 1 遍之后：排序去重的 node id */
    int64_t *ids;        size_t n_ids;
    int32_t *lat, *lon;  int64_t *osm_id;   /* 与 ids 同下标 */

    /* 边（建好 CSR 之前的临时数组） */
    Buf e_from, e_to, e_dist, e_name, e_flags, e_cls, e_speed, e_lanes, e_way;

    /* 落盘后的 CSR */
    uint32_t *node_edge_begin;   /* n_ids + 1 */
    uint16_t *node_edge_count;
    uint32_t *node_class_mask;
    uint8_t  *node_flags;
    uint32_t *csr_of;            /* 原边序 → CSR 边序 */

    StrPool  *names;

    /* 转向限制落盘 */
    Buf out_restr;               /* 16 字节一条 */

    GraphBuildStats st;
    int64_t bb[4];               /* lat_min lon_min lat_max lon_max */
    int     have_bb;
} Graph;

static void bb_update(Graph *g, int32_t lat, int32_t lon)
{
    if (!g->have_bb) {
        g->bb[0] = g->bb[2] = lat; g->bb[1] = g->bb[3] = lon; g->have_bb = 1;
        return;
    }
    if (lat < g->bb[0]) g->bb[0] = lat;
    if (lat > g->bb[2]) g->bb[2] = lat;
    if (lon < g->bb[1]) g->bb[1] = lon;
    if (lon > g->bb[3]) g->bb[3] = lon;
}

/* ------------------------------------------------------------------ */
/* 第 1 遍：way + restriction                                          */
/* ------------------------------------------------------------------ */


static const char *str_fn(uint32_t sid, void *ud)
{
    return pbf_str((PbfBlock *)ud, sid);
}

static int cmp_i64(const void *a, const void *b)
{
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return (x > y) - (x < y);
}

static size_t lower_bound_i64(const int64_t *a, size_t n, int64_t key)
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (a[mid] < key) lo = mid + 1; else hi = mid;
    }
    return lo;
}

static uint8_t restr_dir_from_value(const char *v)
{
    uint8_t m = 0;
    if (strstr(v, "left"))     m |= 1;
    if (strstr(v, "straight")) m |= 2;
    if (strstr(v, "right"))    m |= 4;
    if (strstr(v, "u_turn") || strstr(v, "uturn")) m |= 8;
    return m;
}

static int pass1(Graph *g, const char *path)
{
    PbfStream *s = pbf_stream_open(path);
    if (!s) return -1;

    const char *type; const uint8_t *buf; size_t len;
    PbfStatus rc;
    while ((rc = pbf_stream_next(s, &type, &buf, &len)) == PBF_OK) {
        if (!type || strcmp(type, "OSMData") != 0) continue;
        PbfBlock *blk = NULL;
        if (pbf_unpack_block(buf, len, &blk) != 0) { pbf_stream_close(s); return -1; }

        for (size_t gi = 0; gi < blk->pb->n_primitivegroup; gi++) {
            PrimitiveGroup *pg = blk->pb->primitivegroup[gi];
            if (!pg) continue;

            /* ---- ways ---- */
            for (size_t i = 0; i < pg->n_ways; i++) {
                Way *w = pg->ways[i];
                if (!w) continue;
                g->st.ways_total++;

                RoadTags t;
                road_tags_parse(w->keys, w->n_keys, w->vals, w->n_vals,
                                str_fn, blk, &t);
                if (t.unknown_value) {
                    g->st.rt_unknown_ways++;
                    if (t.whitelisted) g->st.rt_unknown_nondrivable++;
                    else if (t.drivable) g->st.rt_unknown_drivable++;
                }

                /* ---- 分流：路由路 / 线要素 / 面要素 / 丢弃 ---- */
                if (!t.drivable && !t.show && !t.area_kind) {
                    g->st.ways_skip++;
                    continue;
                }
                if (w->n_refs < 2) { g->st.ways_skip++; g->st.ways_too_short++; continue; }

                uint32_t off = (uint32_t)(g->refs.n / sizeof(int64_t));
                if (buf_reserve(&g->refs, w->n_refs * sizeof(int64_t)) != 0) {
                    pbf_block_free(blk); pbf_stream_close(s); return -1;
                }
                int64_t *rp = (int64_t *)g->refs.p;
                int64_t acc = 0;
                for (size_t k = 0; k < w->n_refs; k++) {
                    acc += w->refs[k];
                    rp[g->refs.n / sizeof(int64_t)] = acc;
                    g->refs.n += sizeof(int64_t);
                }

                if (t.drivable) {
                    WayRec wr;
                    memset(&wr, 0, sizeof(wr));
                    wr.osm_id  = w->id;
                    wr.ref_off = off;
                    wr.ref_cnt = (uint32_t)w->n_refs;
                    wr.cls     = t.road_class;
                    wr.oneway  = t.oneway;
                    wr.no_access = t.no_access;
                    wr.speed   = t.speed_kmh;
                    wr.lanes   = t.lanes;
                    wr.flags   = 0;
                    if (t.roundabout) wr.flags |= EF_ROUNDABOUT;
                    if (t.bridge)     wr.flags |= EF_BRIDGE;
                    if (t.tunnel)     wr.flags |= EF_TUNNEL;
                    if (t.toll)       wr.flags |= EF_TOLL;
                    if (t.ferry)      wr.flags |= EF_FERRY;
                    if (t.unpaved)    wr.flags |= EF_UNPAVED;
                    if (t.no_access)  wr.flags |= EF_NO_ACCESS;
                    wr.name_id = t.name ? sp_intern(g->names, t.name, t.name_len) : 0;

                    if (buf_reserve(&g->ways, sizeof(WayRec)) != 0) {
                        pbf_block_free(blk); pbf_stream_close(s); return -1;
                    }
                    memcpy(g->ways.p + g->ways.n, &wr, sizeof(WayRec));
                    g->ways.n += sizeof(WayRec);
                    g->st.ways_kept++;
                } else if (t.show) {
                    /* 线要素：步行道/铁路/水系线，进 lines.bin，不建边 */
                    LineRec lr;
                    memset(&lr, 0, sizeof(lr));
                    lr.osm_id  = w->id;
                    lr.ref_off = off;
                    lr.ref_cnt = (uint32_t)w->n_refs;
                    lr.cls     = t.road_class;
                    lr.flags   = 0;
                    if (t.bridge) lr.flags |= EF_BRIDGE;
                    if (t.tunnel) lr.flags |= EF_TUNNEL;
                    lr.name_id = t.name ? sp_intern(g->names, t.name, t.name_len) : 0;
                    if (buf_reserve(&g->lines, sizeof(LineRec)) != 0) {
                        pbf_block_free(blk); pbf_stream_close(s); return -1;
                    }
                    memcpy(g->lines.p + g->lines.n, &lr, sizeof(LineRec));
                    g->lines.n += sizeof(LineRec);
                    g->st.lines_kept++;
                } else {
                    /* 面要素候选：只有闭合 way 才是面 */
                    uint32_t cnt = (uint32_t)w->n_refs;
                    const int64_t *r = (const int64_t *)g->refs.p;
                    if (cnt >= 4 && r[off] == r[off + cnt - 1]) {
                        AreaRec ar;
                        memset(&ar, 0, sizeof(ar));
                        ar.osm_id  = w->id;
                        ar.ref_off = off;
                        ar.ref_cnt = cnt;
                        ar.kind    = t.area_kind;
                        if (buf_reserve(&g->areas, sizeof(AreaRec)) != 0) {
                            pbf_block_free(blk); pbf_stream_close(s); return -1;
                        }
                        memcpy(g->areas.p + g->areas.n, &ar, sizeof(AreaRec));
                        g->areas.n += sizeof(AreaRec);
                        g->st.areas_kept++;
                    } else {
                        g->st.areas_skip_open++;
                    }
                }
            }

            /* ---- relations（只要转向限制）---- */
            for (size_t i = 0; i < pg->n_relations; i++) {
                Relation *r = pg->relations[i];
                if (!r) continue;
                if (getenv("NAVMAP_DEBUG")) {
                    size_t dn = r->n_keys < r->n_vals ? r->n_keys : r->n_vals;
                    for (size_t k = 0; k < dn; k++) {
                        const char *kk = pbf_str(blk, r->keys[k]);
                        const char *vv = pbf_str(blk, r->vals[k]);
                        if (kk && strcmp(kk, "type") == 0)
                            fprintf(stderr, "DBG relation type=%s\n", vv);
                    }
                }
                int is_restr = 0;
                const char *rv = NULL;
                size_t n = r->n_keys < r->n_vals ? r->n_keys : r->n_vals;
                for (size_t k = 0; k < n; k++) {
                    const char *kk = pbf_str(blk, r->keys[k]);
                    const char *vv = pbf_str(blk, r->vals[k]);
                    if (!kk || !vv) continue;
                    if (strcmp(kk, "type") == 0 && strcmp(vv, "restriction") == 0) is_restr = 1;
                    else if (strcmp(kk, "restriction") == 0) rv = vv;
                }
                if (!is_restr) continue;
                g->st.restr_scanned++;

                int64_t from_way = 0, to_way = 0, via_node = 0, via_way = 0;
                int have_from = 0, have_to = 0, have_via = 0;
                size_t nm = r->n_memids;
                if (nm > r->n_roles_sid) nm = r->n_roles_sid;
                if (nm > r->n_types) nm = r->n_types;
                int64_t acc = 0;
                for (size_t k = 0; k < nm; k++) {
                    acc += r->memids[k];
                    const char *role = pbf_str(blk, r->roles_sid[k]);
                    if (!role) continue;
                    if (strcmp(role, "from") == 0) { from_way = acc; have_from = 1; }
                    else if (strcmp(role, "to") == 0) { to_way = acc; have_to = 1; }
                    else if (strcmp(role, "via") == 0) {
                        if (r->types[k] == RELATION__MEMBER_TYPE__NODE) { via_node = acc; have_via = 1; }
                        else { via_way = acc; have_via = 1; }
                    }
                }
                if (!have_from || !have_to || !have_via) { g->st.restr_skip_incomplete++; continue; }

                PendRestr pr;
                memset(&pr, 0, sizeof(pr));
                pr.from_way = from_way; pr.to_way = to_way;
                pr.via_node = via_node; pr.via_way = via_way;
                pr.kind = (rv && strncmp(rv, "only_", 5) == 0) ? 1 : 0;
                pr.dirmask = rv ? restr_dir_from_value(rv) : 0;
                if (buf_reserve(&g->restr, sizeof(PendRestr)) != 0) {
                    pbf_block_free(blk); pbf_stream_close(s); return -1;
                }
                memcpy(g->restr.p + g->restr.n, &pr, sizeof(PendRestr));
                g->restr.n += sizeof(PendRestr);
            }
        }
        pbf_block_free(blk);
    }
    pbf_stream_close(s);
    return rc == PBF_ERR ? -1 : 0;
}

/* ------------------------------------------------------------------ */
/* 排序去重 → 内部下标                                                  */
/* ------------------------------------------------------------------ */
static int build_index(Graph *g)
{
    size_t nref = g->refs.n / sizeof(int64_t);
    g->ids = malloc((nref ? nref : 1) * sizeof(int64_t));
    if (!g->ids) return -1;
    memcpy(g->ids, g->refs.p, nref * sizeof(int64_t));
    qsort(g->ids, nref, sizeof(int64_t), cmp_i64);

    size_t w = 0;
    for (size_t i = 0; i < nref; i++) {
        if (i == 0 || g->ids[i] != g->ids[i - 1]) g->ids[w++] = g->ids[i];
    }
    g->n_ids = w;

    /* 计数数组必须显式清零：本机上大块走 mmap 时是零页、走主堆时是脏内存，
       靠"没 memset 却一直是对的"会在数据量变小时突然崩。 */
    g->lat = calloc(g->n_ids ? g->n_ids : 1, sizeof(int32_t));
    g->lon = calloc(g->n_ids ? g->n_ids : 1, sizeof(int32_t));
    g->osm_id = calloc(g->n_ids ? g->n_ids : 1, sizeof(int64_t));
    if (!g->lat || !g->lon || !g->osm_id) return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* 第 2 遍：回填坐标                                                    */
/* ------------------------------------------------------------------ */
static int pass2(Graph *g, const char *path)
{
    PbfStream *s = pbf_stream_open(path);
    if (!s) return -1;

    const char *type; const uint8_t *buf; size_t len;
    PbfStatus rc;
    while ((rc = pbf_stream_next(s, &type, &buf, &len)) == PBF_OK) {
        if (!type || strcmp(type, "OSMData") != 0) continue;
        PbfBlock *blk = NULL;
        if (pbf_unpack_block(buf, len, &blk) != 0) { pbf_stream_close(s); return -1; }

        for (size_t gi = 0; gi < blk->pb->n_primitivegroup; gi++) {
            PrimitiveGroup *pg = blk->pb->primitivegroup[gi];
            if (!pg || !pg->dense) continue;
            DenseNodes *dn = pg->dense;
            if (dn->n_lat != dn->n_id || dn->n_lon != dn->n_id) {
                pbf_block_free(blk); pbf_stream_close(s);
                fprintf(stderr, "DenseNodes 三个数组长度不一致\n");
                return -1;
            }
            int64_t acc_id = 0, acc_lat = 0, acc_lon = 0;
            for (size_t i = 0; i < dn->n_id; i++) {
                acc_id  += dn->id[i];
                acc_lat += dn->lat[i];
                acc_lon += dn->lon[i];
                size_t pos = lower_bound_i64(g->ids, g->n_ids, acc_id);
                if (pos >= g->n_ids || g->ids[pos] != acc_id) continue;
                int32_t la = (int32_t)pbf_to_e7(blk, acc_lat, blk->pb->lat_offset);
                int32_t lo = (int32_t)pbf_to_e7(blk, acc_lon, blk->pb->lon_offset);
                g->lat[pos]    = la;
                g->lon[pos]    = lo;
                g->osm_id[pos] = acc_id;
                bb_update(g, la, lo);
            }
        }
        pbf_block_free(blk);
    }
    pbf_stream_close(s);
    return rc == PBF_ERR ? -1 : 0;
}

/* ------------------------------------------------------------------ */
/* 压实：把"way 引用了、但文件里没有坐标"的节点整条剔除                  */
/* ------------------------------------------------------------------ */
static void compact_nodes(Graph *g)
{
    size_t w = 0;
    for (size_t i = 0; i < g->n_ids; i++) {
        if (g->osm_id[i] == 0) continue;   /* 第 2 遍没回填上 = 文件里没这个节点 */
        g->ids[w]    = g->ids[i];
        g->lat[w]    = g->lat[i];
        g->lon[w]    = g->lon[i];
        g->osm_id[w] = g->osm_id[i];
        w++;
    }
    g->st.dead_nodes = g->n_ids - w;
    g->n_ids = w;
    /* ids 仍然有序，lower_bound 照常可用；边里凡是引用被剔除节点的，
       建边时会命中"找不到"分支被丢掉。 */
}

/* ------------------------------------------------------------------ */
/* 建边 + CSR                                                          */
/* ------------------------------------------------------------------ */
static int build_edges(Graph *g)
{
    const int64_t *rp = (const int64_t *)g->refs.p;

    g->e_from.esz = 4; g->e_to.esz = 4; g->e_dist.esz = 4; g->e_name.esz = 4;
    g->e_flags.esz = 2; g->e_cls.esz = 1; g->e_speed.esz = 1; g->e_lanes.esz = 2;
    g->e_way.esz = 8;

    size_t nways = g->ways.n / sizeof(WayRec);
    const WayRec *wp = (const WayRec *)g->ways.p;

    for (size_t wi = 0; wi < nways; wi++) {
        const WayRec *w = &wp[wi];
        for (uint32_t k = 0; k + 1 < w->ref_cnt; k++) {
            int64_t a = rp[w->ref_off + k];
            int64_t b = rp[w->ref_off + k + 1];
            size_t pa = lower_bound_i64(g->ids, g->n_ids, a);
            size_t pb = lower_bound_i64(g->ids, g->n_ids, b);
            if (pa >= g->n_ids || g->ids[pa] != a ||
                pb >= g->n_ids || g->ids[pb] != b) {
                g->st.dropped_missing_node++;
                continue;
            }
            /* 坐标没回填上（文件里根本没有这个节点）也要跳过 */
            if (g->osm_id[pa] == 0 || g->osm_id[pb] == 0) {
                g->st.dropped_missing_node++;
                continue;
            }
            double d = nm_fast_dist_m(g->lat[pa], g->lon[pa], g->lat[pb], g->lon[pb]);
            uint32_t dcm = (uint32_t)(d * 100.0 + 0.5);
            if (dcm == 0) dcm = 1;

            struct { size_t from, to; uint16_t flags; } dirs[2];
            int ndir = 1;
            dirs[0].from = pa; dirs[0].to = pb; dirs[0].flags = w->flags;
            if (w->oneway == 0) {
                dirs[1].from = pb; dirs[1].to = pa;
                dirs[1].flags = w->flags | EF_REVERSE;
                ndir = 2;
            } else if (w->oneway == 2) {
                /* oneway=-1：只允许按节点顺序逆向走。
                   这里**不能**打 EF_REVERSE —— 它是这条路唯一的一条边，
                   打了会被渲染当成"双向路的回程边"跳掉，整条路就画不出来了。 */
                dirs[0].from = pb; dirs[0].to = pa; dirs[0].flags = w->flags;
            }

            for (int di = 0; di < ndir; di++) {
                uint32_t f = dirs[di].from, t = dirs[di].to;
                if (buf_reserve(&g->e_from, 4) || buf_reserve(&g->e_to, 4) ||
                    buf_reserve(&g->e_dist, 4) || buf_reserve(&g->e_name, 4) ||
                    buf_reserve(&g->e_flags, 2) || buf_reserve(&g->e_cls, 1) ||
                    buf_reserve(&g->e_speed, 1) || buf_reserve(&g->e_lanes, 2) ||
                    buf_reserve(&g->e_way, 8)) return -1;

                uint32_t fi = (uint32_t)g->e_from.n / 4;
                memcpy(g->e_from.p + g->e_from.n, &f, 4); g->e_from.n += 4;
                memcpy(g->e_to.p   + g->e_to.n,   &t, 4); g->e_to.n   += 4;
                memcpy(g->e_dist.p + g->e_dist.n, &dcm, 4); g->e_dist.n += 4;
                memcpy(g->e_name.p + g->e_name.n, &w->name_id, 4); g->e_name.n += 4;
                memcpy(g->e_flags.p + g->e_flags.n, &dirs[di].flags, 2); g->e_flags.n += 2;
                uint8_t cl = w->cls, sp = (uint8_t)(w->speed > 255 ? 255 : w->speed);
                memcpy(g->e_cls.p   + g->e_cls.n,   &cl, 1); g->e_cls.n   += 1;
                memcpy(g->e_speed.p + g->e_speed.n, &sp, 1); g->e_speed.n += 1;
                memcpy(g->e_lanes.p + g->e_lanes.n, &w->lanes, 2); g->e_lanes.n += 2;
                memcpy(g->e_way.p   + g->e_way.n,   &w->osm_id, 8); g->e_way.n += 8;
                (void)fi;
            }
        }
    }

    size_t ne = g->e_to.n / 4;
    g->st.edges = ne;
    g->st.nodes = g->n_ids;

    /* ---- CSR：按 from 做计数排序 ---- */
    uint32_t *cnt = calloc(g->n_ids + 1, sizeof(uint32_t));
    uint32_t *begin = malloc((g->n_ids + 2) * sizeof(uint32_t));
    uint32_t *pos = malloc((g->n_ids + 1) * sizeof(uint32_t));
    g->node_class_mask = calloc(g->n_ids ? g->n_ids : 1, sizeof(uint32_t));
    g->node_edge_begin = calloc(g->n_ids + 1, sizeof(uint32_t));
    g->node_edge_count = calloc(g->n_ids ? g->n_ids : 1, sizeof(uint16_t));
    g->node_flags      = calloc(g->n_ids ? g->n_ids : 1, sizeof(uint8_t));
    g->csr_of          = malloc((ne ? ne : 1) * sizeof(uint32_t));
    if (!cnt || !begin || !pos || !g->node_class_mask || !g->node_edge_begin ||
        !g->node_edge_count || !g->node_flags || !g->csr_of) {
        free(cnt); free(begin); free(pos); return -1;
    }

    const uint32_t *ef = (const uint32_t *)g->e_from.p;
    const uint8_t  *ec = (const uint8_t *)g->e_cls.p;
    for (size_t i = 0; i < ne; i++) {
        cnt[ef[i]]++;
        g->node_class_mask[ef[i]] |= (1u << (ec[i] & 7));
    }
    uint64_t acc = 0;
    for (size_t i = 0; i < g->n_ids; i++) {
        begin[i] = (uint32_t)acc;
        pos[i]   = (uint32_t)acc;
        acc += cnt[i];
    }
    begin[g->n_ids] = (uint32_t)acc;
    for (size_t i = 0; i < ne; i++) g->csr_of[i] = pos[ef[i]]++;

    /* 度数 ≥3 记为路口，渲染/路由都用它 */
    for (size_t i = 0; i < g->n_ids; i++) {
        if (cnt[i] >= 3) g->node_flags[i] |= 1u;
        g->node_edge_begin[i] = begin[i];
        g->node_edge_count[i] = cnt[i] > 65535u ? 65535u : (uint16_t)cnt[i];
    }
    g->node_edge_begin[g->n_ids] = begin[g->n_ids];

    free(cnt); free(begin); free(pos);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 转向限制：把 (from_way, via_node, to_way) 定位到具体的有向边        */
/* ------------------------------------------------------------------ */
typedef struct {
    size_t   key;        /* via 节点下标 */
    int64_t  way;        /* 所属 way 的 osm id */
    int      incoming;   /* 1 = 指向 via，0 = 从 via 出发 */
    uint32_t edge;       /* CSR 边下标 */
    size_t   other;      /* 另一端节点下标（via=way 定位 e_via 时要认准终点） */
} EdgeCand;

/* way osm id → 它在 refs 池里的节点序列（via=way 要靠它求 A/B 交点） */
typedef struct { int64_t id; uint32_t off, cnt; } WayIdx;

static int cmp_wayidx(const void *a, const void *b)
{
    const WayIdx *x = a, *y = b;
    return (x->id > y->id) - (x->id < y->id);
}

static const WayIdx *wayidx_find(const WayIdx *w, size_t n, int64_t id)
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (w[mid].id == id) return &w[mid];
        if (w[mid].id < id) lo = mid + 1; else hi = mid;
    }
    return NULL;
}

static size_t node_idx_of(const Graph *g, int64_t osm_id)
{
    size_t p = lower_bound_i64(g->ids, g->n_ids, osm_id);
    if (p < g->n_ids && g->ids[p] == osm_id) return p;
    return SIZE_MAX;
}

/* 两条 way 的公共节点。返回 1=唯一并写入 *out，0=没有，-1=有多个不同交点。
   way 自交会让同一个节点 id 在一条 way 里出现两次，这里按"不同的内部下标"
   判唯一，所以自交不算歧义。 */
static int common_node(const Graph *g, const WayIdx *fa, const WayIdx *fb,
                       size_t *out)
{
    const int64_t *rp = (const int64_t *)g->refs.p;
    int found = 0;
    size_t val = 0;
    for (uint32_t i = 0; i < fa->cnt; i++) {
        int64_t id = rp[(size_t)fa->off + i];
        for (uint32_t j = 0; j < fb->cnt; j++) {
            if (rp[(size_t)fb->off + j] != id) continue;
            size_t ix = node_idx_of(g, id);
            if (ix == SIZE_MAX) continue;
            if (!found) { val = ix; found = 1; }
            else if (val != ix) return -1;
            break;
        }
    }
    if (!found) return 0;
    *out = val;
    return 1;
}

/* 在候选边里挑唯一的一条：node 处、方向 incoming、属于 way。
   need_other 时还要求另一端 == other（via way 必须正好从 A 跨到 B，
   多段的 via way 在这里会落空，被保守跳过）。 */
static int pick_edge(const EdgeCand *cp, size_t nc, size_t node, int incoming,
                     int64_t way, int need_other, size_t other, uint32_t *out)
{
    int found = 0;
    uint32_t val = 0;
    for (size_t k = 0; k < nc; k++) {
        if (cp[k].key != node || cp[k].incoming != incoming || cp[k].way != way)
            continue;
        if (need_other && cp[k].other != other) continue;
        if (!found) { val = cp[k].edge; found = 1; }
        else if (val != cp[k].edge) return -1;      /* 多条：不敢猜，跳过 */
    }
    if (!found) return 0;
    *out = val;
    return 1;
}

static int resolve_restrictions(Graph *g)
{
    size_t nr = g->restr.n / sizeof(PendRestr);
    if (nr == 0) return 0;
    const PendRestr *pr = (const PendRestr *)g->restr.p;

    /* way osm id → 节点序列：via=way 求 A/B 交点要用 */
    size_t nw = g->ways.n / sizeof(WayRec);
    const WayRec *wr = (const WayRec *)g->ways.p;
    WayIdx *wi = malloc((nw ? nw : 1) * sizeof(WayIdx));
    if (!wi) return -1;
    for (size_t i = 0; i < nw; i++) {
        wi[i].id = wr[i].osm_id; wi[i].off = wr[i].ref_off; wi[i].cnt = wr[i].ref_cnt;
    }
    qsort(wi, nw, sizeof(WayIdx), cmp_wayidx);

    /* via=way 的交点：先算出来，才能把 A/B 也加进候选边的节点集合 */
    size_t *ab = malloc(nr * 2 * sizeof(size_t));   /* [2i]=A [2i+1]=B */
    char   *ab_ok = calloc(nr, 1);
    if (!ab || !ab_ok) { free(wi); free(ab); free(ab_ok); return -1; }
    for (size_t i = 0; i < nr; i++) {
        if (!pr[i].via_way) continue;
        const WayIdx *f = wayidx_find(wi, nw, pr[i].from_way);
        const WayIdx *v = wayidx_find(wi, nw, pr[i].via_way);
        const WayIdx *t = wayidx_find(wi, nw, pr[i].to_way);
        if (!f || !v || !t) continue;
        size_t a = 0, b = 0;
        if (common_node(g, f, v, &a) != 1) continue;
        if (common_node(g, v, t, &b) != 1) continue;
        if (a == b) continue;                       /* 退化成单点，交给别处 */
        ab[2 * i] = a; ab[2 * i + 1] = b; ab_ok[i] = 1;
    }

    /* via 节点集合：转成内部下标后再排序去重（下面要和边数组里的内部下标比）。
       via=way 要收 A 和 B 两个，所以最多 2*nr 个。 */
    int64_t *vias = malloc((nr ? nr * 2 : 1) * sizeof(int64_t));
    size_t   nv = 0;
    if (!vias) { free(wi); free(ab); free(ab_ok); return -1; }
    for (size_t i = 0; i < nr; i++) {
        if (pr[i].via_way) {
            if (!ab_ok[i]) continue;
            vias[nv++] = (int64_t)ab[2 * i];
            vias[nv++] = (int64_t)ab[2 * i + 1];
        } else {
            size_t p = lower_bound_i64(g->ids, g->n_ids, pr[i].via_node);
            if (p < g->n_ids && g->ids[p] == pr[i].via_node) vias[nv++] = (int64_t)p;
        }
    }
    qsort(vias, nv, sizeof(int64_t), cmp_i64);
    size_t w = 0;
    for (size_t i = 0; i < nv; i++) if (i == 0 || vias[i] != vias[i - 1]) vias[w++] = vias[i];
    nv = w;

    /* 候选边：via 节点的入边要全表扫（CSR 只给得出边） */
    Buf cands; memset(&cands, 0, sizeof(cands)); cands.esz = sizeof(EdgeCand);
    const uint32_t *ef = (const uint32_t *)g->e_from.p;
    const uint32_t *et = (const uint32_t *)g->e_to.p;
    const int64_t  *ew = (const int64_t *)g->e_way.p;
    size_t ne = g->e_to.n / 4;

    for (size_t i = 0; i < ne; i++) {
        size_t p = lower_bound_i64(vias, nv, (int64_t)et[i]);
        if (p < nv && vias[p] == (int64_t)et[i]) {
            EdgeCand c = { (size_t)et[i], ew[i], 1, g->csr_of[i], (size_t)ef[i] };
            if (buf_reserve(&cands, sizeof(EdgeCand))) goto oom;
            memcpy(cands.p + cands.n, &c, sizeof(EdgeCand)); cands.n += sizeof(EdgeCand);
        }
        size_t q = lower_bound_i64(vias, nv, (int64_t)ef[i]);
        if (q < nv && vias[q] == (int64_t)ef[i]) {
            EdgeCand c = { (size_t)ef[i], ew[i], 0, g->csr_of[i], (size_t)et[i] };
            if (buf_reserve(&cands, sizeof(EdgeCand))) goto oom;
            memcpy(cands.p + cands.n, &c, sizeof(EdgeCand)); cands.n += sizeof(EdgeCand);
        }
    }

    g->out_restr.esz = NM_RESTR_BYTES;
    const EdgeCand *cp = (const EdgeCand *)cands.p;
    size_t nc = cands.n / sizeof(EdgeCand);

    for (size_t i = 0; i < nr; i++) {
        uint8_t rec[NM_RESTR_BYTES];
        memset(rec, 0, sizeof(rec));
        rec[RF_DIRMASK] = pr[i].dirmask;

        if (pr[i].via_way) {
            /* ---- via=way：三边序列 e_from → e_via → e_to ----
               记录挂在 B（via∩to），from 记 e_via，prev 记 e_from。
               挂 B 而不是 A，是因为转向发生在 B；from 之所以不能省，是同一对
               via/to 对不同来路约束不同（北京实测同一条 via 同时有
               no_u_turn 和 no_left_turn，来路不同）。 */
            if (!ab_ok[i]) { g->st.restr_skip_via_way++; continue; }
            size_t a = ab[2 * i], b = ab[2 * i + 1];
            uint32_t e_from = 0, e_via = 0, e_to = 0;
            if (pick_edge(cp, nc, a, 1, pr[i].from_way, 0, 0, &e_from) != 1 ||
                pick_edge(cp, nc, a, 0, pr[i].via_way,  1, b, &e_via)  != 1 ||
                pick_edge(cp, nc, b, 0, pr[i].to_way,   0, 0, &e_to)   != 1) {
                g->st.restr_skip_via_way++; continue;
            }
            nm_put_u32(rec + RF_FROM_EDGE, e_via);
            nm_put_u32(rec + RF_TO_EDGE,   e_to);
            nm_put_u32(rec + RF_VIA_NODE,  (uint32_t)b);
            nm_put_u32(rec + RF_PREV_EDGE, e_from);
            rec[RF_KIND] = 2;
            g->st.restr_via_way_written++;
        } else {
            size_t via_idx = lower_bound_i64(g->ids, g->n_ids, pr[i].via_node);
            if (via_idx >= g->n_ids || g->ids[via_idx] != pr[i].via_node) {
                g->st.restr_skip_unresolved++; continue;
            }
            uint32_t fe = 0, te = 0;
            if (pick_edge(cp, nc, via_idx, 1, pr[i].from_way, 0, 0, &fe) != 1 ||
                pick_edge(cp, nc, via_idx, 0, pr[i].to_way,   0, 0, &te) != 1) {
                g->st.restr_skip_unresolved++; continue;
            }
            nm_put_u32(rec + RF_FROM_EDGE, fe);
            nm_put_u32(rec + RF_TO_EDGE,   te);
            nm_put_u32(rec + RF_VIA_NODE,  (uint32_t)via_idx);
            rec[RF_KIND] = pr[i].kind;
        }

        if (buf_reserve(&g->out_restr, NM_RESTR_BYTES)) goto oom;
        memcpy(g->out_restr.p + g->out_restr.n, rec, NM_RESTR_BYTES);
        g->out_restr.n += NM_RESTR_BYTES;
        g->st.restr_written++;
    }

    free(vias); free(wi); free(ab); free(ab_ok);
    buf_free(&cands);
    return 0;

oom:
    free(vias); free(wi); free(ab); free(ab_ok);
    buf_free(&cands);
    return -1;
}

/* ------------------------------------------------------------------ */
/* 落盘                                                               */
/* ------------------------------------------------------------------ */
static int write_nodes(Graph *g, const char *path, const char *src)
{
    NMBinWriter *w = nmbin_create(path, NMBIN_KIND_NODES, NM_NODE_BYTES, src);
    if (!w) return -1;
    nmbin_add_field(w, "lat_e7",      NF_LAT,        NMT_I32, 4);
    nmbin_add_field(w, "lon_e7",      NF_LON,        NMT_I32, 4);
    nmbin_add_field(w, "edge_begin",  NF_EDGE_BEGIN, NMT_U32, 4);
    nmbin_add_field(w, "edge_count",  NF_EDGE_COUNT, NMT_U16, 2);
    nmbin_add_field(w, "node_flags",  NF_NODE_FLAGS, NMT_U16, 2);
    nmbin_add_field(w, "osm_id",      NF_OSM_ID,     NMT_U64, 8);
    nmbin_add_field(w, "class_mask",  NF_CLASS_MASK, NMT_U32, 4);

    uint8_t rec[NM_NODE_BYTES];
    const size_t CHUNK = 8192;
    uint8_t *bulk = malloc(CHUNK * NM_NODE_BYTES);
    if (!bulk) { nmbin_abort(w); return -1; }
    size_t i = 0;
    while (i < g->n_ids) {
        size_t n = g->n_ids - i < CHUNK ? g->n_ids - i : CHUNK;
        for (size_t k = 0; k < n; k++) {
            memset(rec, 0, sizeof(rec));
            nm_put_i32(rec + NF_LAT,        g->lat[i + k]);
            nm_put_i32(rec + NF_LON,        g->lon[i + k]);
            nm_put_u32(rec + NF_EDGE_BEGIN, g->node_edge_begin[i + k]);
            nm_put_u16(rec + NF_EDGE_COUNT, g->node_edge_count[i + k]);
            nm_put_u16(rec + NF_NODE_FLAGS, (uint16_t)g->node_flags[i + k]);
            nm_put_u64(rec + NF_OSM_ID,     (uint64_t)g->osm_id[i + k]);
            nm_put_u32(rec + NF_CLASS_MASK, g->node_class_mask[i + k]);
            memcpy(bulk + k * NM_NODE_BYTES, rec, NM_NODE_BYTES);
        }
        if (nmbin_write_records(w, bulk, n) != 0) { free(bulk); nmbin_abort(w); return -1; }
        i += n;
    }
    free(bulk);
    return nmbin_finalize(w);
}

static int write_edges(Graph *g, const char *path, const char *src)
{
    NMBinWriter *w = nmbin_create(path, NMBIN_KIND_EDGES, NM_EDGE_BYTES, src);
    if (!w) return -1;
    nmbin_add_field(w, "to",       EF_TO,      NMT_U32, 4);
    nmbin_add_field(w, "dist_cm",  EF_DIST_CM, NMT_U32, 4);
    nmbin_add_field(w, "name_id",  EF_NAME_ID, NMT_U32, 4);
    nmbin_add_field(w, "flags",    EF_FLAGS,   NMT_U16, 2);
    nmbin_add_field(w, "class",    EF_CLASS,   NMT_U8,  1);
    nmbin_add_field(w, "speed",    EF_SPEED,   NMT_U8,  1);
    nmbin_add_field(w, "lanes",    EF_LANES,   NMT_U16, 2);

    size_t ne = g->e_to.n / 4;
    const uint32_t *eto = (const uint32_t *)g->e_to.p;
    const uint32_t *eds = (const uint32_t *)g->e_dist.p;
    const uint32_t *enm = (const uint32_t *)g->e_name.p;
    const uint16_t *efl = (const uint16_t *)g->e_flags.p;
    const uint8_t  *ecl = (const uint8_t *)g->e_cls.p;
    const uint8_t  *esp = (const uint8_t *)g->e_speed.p;
    const uint16_t *eln = (const uint16_t *)g->e_lanes.p;

    const size_t CHUNK = 8192;
    uint8_t *bulk = malloc(CHUNK * NM_EDGE_BYTES);
    if (!bulk) { nmbin_abort(w); return -1; }

    /* 按 CSR 顺序写：edge 下标 = CSR 位置 */
    uint8_t *csr_sorted = malloc((ne ? ne : 1) * NM_EDGE_BYTES);
    if (!csr_sorted) { free(bulk); nmbin_abort(w); return -1; }
    uint8_t rec[NM_EDGE_BYTES];
    for (size_t i = 0; i < ne; i++) {
        memset(rec, 0, sizeof(rec));
        nm_put_u32(rec + EF_TO,      eto[i]);
        nm_put_u32(rec + EF_DIST_CM, eds[i]);
        nm_put_u32(rec + EF_NAME_ID, enm[i]);
        nm_put_u16(rec + EF_FLAGS,   efl[i]);
        rec[EF_CLASS] = ecl[i];
        rec[EF_SPEED] = esp[i];
        nm_put_u16(rec + EF_LANES,   eln[i]);
        memcpy(csr_sorted + g->csr_of[i] * NM_EDGE_BYTES, rec, NM_EDGE_BYTES);
    }
    /* 边下标变了，restrictions 里的 csr_of 已经指向新下标，无需修正 */

    size_t off = 0;
    while (off < ne) {
        size_t n = ne - off < CHUNK ? ne - off : CHUNK;
        if (nmbin_write_records(w, csr_sorted + off * NM_EDGE_BYTES, n) != 0) {
            free(bulk); free(csr_sorted); nmbin_abort(w); return -1;
        }
        off += n;
    }
    free(bulk); free(csr_sorted);
    return nmbin_finalize(w);
}

static int write_osmid(Graph *g, const char *path, const char *src)
{
    NMBinWriter *w = nmbin_create(path, NMBIN_KIND_OSMID, NM_OSMID_BYTES, src);
    if (!w) return -1;
    nmbin_add_field(w, "osm_id", 0, NMT_I64, 8);
    /* 已经是 int64 数组，但落盘统一走小端助手，避免直接 fwrite 结构体 */
    const size_t CHUNK = 8192;
    uint8_t *bulk = malloc(CHUNK * NM_OSMID_BYTES);
    if (!bulk) { nmbin_abort(w); return -1; }
    size_t i = 0;
    while (i < g->n_ids) {
        size_t n = g->n_ids - i < CHUNK ? g->n_ids - i : CHUNK;
        for (size_t k = 0; k < n; k++) nm_put_u64(bulk + k * 8, (uint64_t)g->osm_id[i + k]);
        if (nmbin_write_records(w, bulk, n) != 0) { free(bulk); nmbin_abort(w); return -1; }
        i += n;
    }
    free(bulk);
    return nmbin_finalize(w);
}

static int write_names(Graph *g, const char *path, const char *src)
{
    uint8_t *blob; size_t blob_len;
    sp_serialize(g->names, &blob, &blob_len);
    if (!blob) return -1;

    /* 名字区是变长的，rec_size 用 1，rec_count = 字节数 */
    NMBinWriter *w = nmbin_create(path, NMBIN_KIND_NAMES, 1, src);
    if (!w) { free(blob); return -1; }
    nmbin_add_field(w, "pool", 0, NMT_U8, 1);
    if (nmbin_write_records(w, blob, blob_len) != 0) { free(blob); nmbin_abort(w); return -1; }
    free(blob);
    return nmbin_finalize(w);
}

static int write_restrictions(Graph *g, const char *path, const char *src)
{
    NMBinWriter *w = nmbin_create(path, NMBIN_KIND_RESTRICTIONS, NM_RESTR_BYTES, src);
    if (!w) return -1;
    nmbin_add_field(w, "from_edge", RF_FROM_EDGE, NMT_U32, 4);
    nmbin_add_field(w, "to_edge",   RF_TO_EDGE,   NMT_U32, 4);
    nmbin_add_field(w, "via_node",  RF_VIA_NODE,  NMT_U32, 4);
    nmbin_add_field(w, "kind",      RF_KIND,      NMT_U8,  1);
    nmbin_add_field(w, "dirmask",   RF_DIRMASK,   NMT_U8,  1);
    size_t n = g->out_restr.n / NM_RESTR_BYTES;
    if (n && nmbin_write_records(w, g->out_restr.p, n) != 0) { nmbin_abort(w); return -1; }
    return nmbin_finalize(w);
}

/* ------------------------------------------------------------------ */
/* 线/面要素落盘：refs (osm id) → polyrefs (内部下标)                    */
/* ------------------------------------------------------------------ */
#define OUTF2(p, dir, name) (snprintf(p, sizeof(p), "%s/%s", dir, name), (const char *)(p))

static int write_lines_areas(Graph *g, const char *dir, const char *src)
{
    size_t nlines = g->lines.n / sizeof(LineRec);
    size_t nareas = g->areas.n / sizeof(AreaRec);
    LineRec *lr = (LineRec *)g->lines.p;
    AreaRec  *ar = (AreaRec *)g->areas.p;
    const int64_t *rp = (const int64_t *)g->refs.p;

    Buf pr; memset(&pr, 0, sizeof(pr)); pr.esz = 4;   /* u32 内部下标 */
    uint32_t cursor = 0;
    for (size_t i = 0; i < nlines; i++) {
        uint32_t off = cursor;
        int dropped = 0;
        for (uint32_t k = 0; k < lr[i].ref_cnt; k++) {
            int64_t id = rp[lr[i].ref_off + k];
            size_t p = lower_bound_i64(g->ids, g->n_ids, id);
            if (p >= g->n_ids || g->ids[p] != id) { dropped = 1; break; }
            if (buf_reserve(&pr, 4) != 0) { buf_free(&pr); return -1; }
            uint32_t idx = (uint32_t)p;
            memcpy(pr.p + pr.n, &idx, 4); pr.n += 4;
            cursor++;
        }
        if (dropped) {
            /* 回退这一条已写的下标 */
            pr.n = (size_t)off * 4; cursor = off;
            g->st.linearea_dropped_missing++;
            lr[i].ref_cnt = 0;      /* 标记丢弃，落盘时跳过 */
            continue;
        }
        lr[i].ref_off = off;
    }
    for (size_t i = 0; i < nareas; i++) {
        uint32_t off = cursor;
        int dropped = 0;
        for (uint32_t k = 0; k < ar[i].ref_cnt; k++) {
            int64_t id = rp[ar[i].ref_off + k];
            size_t p = lower_bound_i64(g->ids, g->n_ids, id);
            if (p >= g->n_ids || g->ids[p] != id) { dropped = 1; break; }
            if (buf_reserve(&pr, 4) != 0) { buf_free(&pr); return -1; }
            uint32_t idx = (uint32_t)p;
            memcpy(pr.p + pr.n, &idx, 4); pr.n += 4;
            cursor++;
        }
        if (dropped) {
            pr.n = (size_t)off * 4; cursor = off;
            g->st.linearea_dropped_missing++;
            ar[i].ref_cnt = 0;
            continue;
        }
        ar[i].ref_off = off;
    }

    /* ---- polyrefs.bin ---- */
    char path[4096];
    NMBinWriter *w = nmbin_create(OUTF2(path, dir, "polyrefs.bin"),
                                  NMBIN_KIND_POLYREFS, 4, src);
    if (!w) { buf_free(&pr); return -1; }
    nmbin_add_field(w, "node_idx", 0, NMT_U32, 4);
    size_t np = pr.n / 4;
    if (np && nmbin_write_records(w, pr.p, np) != 0) {
        nmbin_abort(w); buf_free(&pr); return -1;
    }
    if (nmbin_finalize(w) != 0) { buf_free(&pr); return -1; }

    /* ---- lines.bin（跳过 ref_cnt=0 的丢弃条目）----
       丢弃条目在中间的话，剩余条目的 ref_off 已经各自正确，
       只是记录之间有洞，直接顺序写有效条目即可。 */
    w = nmbin_create(OUTF2(path, dir, "lines.bin"), NMBIN_KIND_LINES,
                     NM_LINE_BYTES, src);
    if (!w) { buf_free(&pr); return -1; }
    nmbin_add_field(w, "osm_id",  LF_OSM_ID,  NMT_U64, 8);
    nmbin_add_field(w, "ref_off", LF_REF_OFF, NMT_U32, 4);
    nmbin_add_field(w, "ref_cnt", LF_REF_CNT, NMT_U32, 4);
    nmbin_add_field(w, "class",   LF_CLASS,   NMT_U8,  1);
    nmbin_add_field(w, "flags",   LF_FLAGS,   NMT_U8,  1);
    nmbin_add_field(w, "name_id", LF_NAME_ID, NMT_U32, 4);
    for (size_t i = 0; i < nlines; i++) {
        if (lr[i].ref_cnt == 0) continue;
        uint8_t rec[NM_LINE_BYTES];
        memset(rec, 0, sizeof(rec));
        nm_put_u64(rec + LF_OSM_ID,  (uint64_t)lr[i].osm_id);
        nm_put_u32(rec + LF_REF_OFF, lr[i].ref_off);
        nm_put_u32(rec + LF_REF_CNT, lr[i].ref_cnt);
        rec[LF_CLASS]  = lr[i].cls;
        rec[LF_FLAGS]  = lr[i].flags;
        nm_put_u32(rec + LF_NAME_ID, lr[i].name_id);
        if (nmbin_write_records(w, rec, 1) != 0) {
            nmbin_abort(w); buf_free(&pr); return -1;
        }
    }
    if (nmbin_finalize(w) != 0) { buf_free(&pr); return -1; }

    /* ---- areas.bin ---- */
    w = nmbin_create(OUTF2(path, dir, "areas.bin"), NMBIN_KIND_AREAS,
                     NM_AREA_BYTES, src);
    if (!w) { buf_free(&pr); return -1; }
    nmbin_add_field(w, "osm_id",  AF_OSM_ID,  NMT_U64, 8);
    nmbin_add_field(w, "ref_off", AF_REF_OFF, NMT_U32, 4);
    nmbin_add_field(w, "ref_cnt", AF_REF_CNT, NMT_U32, 4);
    nmbin_add_field(w, "kind",    AF_KIND,    NMT_U8,  1);
    for (size_t i = 0; i < nareas; i++) {
        if (ar[i].ref_cnt == 0) continue;
        uint8_t rec[NM_AREA_BYTES];
        memset(rec, 0, sizeof(rec));
        nm_put_u64(rec + AF_OSM_ID,  (uint64_t)ar[i].osm_id);
        nm_put_u32(rec + AF_REF_OFF, ar[i].ref_off);
        nm_put_u32(rec + AF_REF_CNT, ar[i].ref_cnt);
        rec[AF_KIND] = ar[i].kind;
        if (nmbin_write_records(w, rec, 1) != 0) {
            nmbin_abort(w); buf_free(&pr); return -1;
        }
    }
    if (nmbin_finalize(w) != 0) { buf_free(&pr); return -1; }

    buf_free(&pr);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 空间索引：edges.bin 保持 CSR 顺序不动，生成 tile → (from,edge) 目录    */
/* ------------------------------------------------------------------ */
static int write_spatial_index(Graph *g, const char *dir, const char *src)
{
    size_t ne = g->e_to.n / 4;
    if (!ne) return 0;

    const uint32_t *ef = (const uint32_t *)g->e_from.p;
    const uint32_t *csr = g->csr_of;

    /* 每条边的 from/to 两端 tile 坐标（Web Mercator × 2^Z 取整） */
    uint32_t *tx0 = malloc(ne * sizeof(uint32_t)), *ty0 = malloc(ne * sizeof(uint32_t));
    uint32_t *tx1 = malloc(ne * sizeof(uint32_t)), *ty1 = malloc(ne * sizeof(uint32_t));
    if (!tx0 || !ty0 || !tx1 || !ty1) {
        free(tx0); free(ty0); free(tx1); free(ty1); return -1;
    }

    uint32_t minx = UINT32_MAX, maxx = 0, miny = UINT32_MAX, maxy = 0;
    for (size_t i = 0; i < ne; i++) {
        double wx, wy;
        nm_mercator(g->lat[ef[i]] / 1e7, g->lon[ef[i]] / 1e7, &wx, &wy);
        tx0[i] = (uint32_t)(wx * (1u << NM_TILE_Z));
        ty0[i] = (uint32_t)(wy * (1u << NM_TILE_Z));
        /* Buf.p 是 char*，而 nm_get_u32 收的是 uint8_t*：char 的符号性由平台定，
           不加转换会触发 -Wpointer-sign */
        uint32_t to = nm_get_u32((const uint8_t *)g->e_to.p + i * 4);
        nm_mercator(g->lat[to] / 1e7, g->lon[to] / 1e7, &wx, &wy);
        tx1[i] = (uint32_t)(wx * (1u << NM_TILE_Z));
        ty1[i] = (uint32_t)(wy * (1u << NM_TILE_Z));
        /* from 和 to 端都要进 bbox：死端节点只作为 to 出现，它的 tile
           可能超出"所有 from 端"的范围，不纳入会导致下面越界写 */
        if (tx0[i] < minx) minx = tx0[i];
        if (tx0[i] > maxx) maxx = tx0[i];
        if (ty0[i] < miny) miny = ty0[i];
        if (ty0[i] > maxy) maxy = ty0[i];
        if (tx1[i] < minx) minx = tx1[i];
        if (tx1[i] > maxx) maxx = tx1[i];
        if (ty1[i] < miny) miny = ty1[i];
        if (ty1[i] > maxy) maxy = ty1[i];
    }
    uint32_t nx = maxx - minx + 1, ny = maxy - miny + 1;
    size_t ntile = (size_t)nx * ny;
    if (ntile > (1u << 26)) {   /* 防御：异常 bbox 放弃索引（不影响正确性） */
        free(tx0); free(ty0); free(tx1); free(ty1);
        return 0;
    }

    /* 计数排序：每条边加入 from/to 两端 tile bbox 覆盖的所有 tile（跨度上限 8，
       长边（高速长直段）才能在视口只覆盖远处 tile 时也画出来）。
       注意：一条边会出现在多个 tile，edgelist 总条数 Σcnt > 边数，
       所以 xl 必须在计数之后按总和分配，不能按 ne。 */
    uint32_t *cnt = calloc(ntile, sizeof(uint32_t));
    uint32_t *off = malloc(ntile * sizeof(uint32_t));
    if (!cnt || !off) {
        free(cnt); free(off);
        free(tx0); free(ty0); free(tx1); free(ty1);
        return -1;
    }

    /* 跨度上限（tile 数）：z14 约 2.4km/格，8 格 ≈ 19km，超过的极端长边
       只保证前 8 格可见。两端四向 clamp 进 [min..max]。 */
    #define EDGE_TILE_RANGE(i, ax, bx, cy, dy) do { \
        uint32_t lx_ = tx0[i] < tx1[i] ? tx0[i] : tx1[i]; \
        uint32_t hx_ = tx0[i] > tx1[i] ? tx0[i] : tx1[i]; \
        uint32_t ly_ = ty0[i] < ty1[i] ? ty0[i] : ty1[i]; \
        uint32_t hy_ = ty0[i] > ty1[i] ? ty0[i] : ty1[i]; \
        (ax) = lx_ < minx ? minx : lx_; \
        (bx) = hx_ > maxx ? maxx : hx_; \
        (cy) = ly_ < miny ? miny : ly_; \
        (dy) = hy_ > maxy ? maxy : hy_; \
        if ((bx) > (ax) + 7) (bx) = (ax) + 7; \
        if ((dy) > (cy) + 7) (dy) = (cy) + 7; \
    } while (0)

    for (size_t i = 0; i < ne; i++) {
        uint32_t ax, bx, cy, dy;
        EDGE_TILE_RANGE(i, ax, bx, cy, dy);
        for (uint32_t y = cy; y <= dy; y++)
            for (uint32_t x = ax; x <= bx; x++)
                cnt[((size_t)(y - miny) * nx) + (x - minx)]++;
    }
    uint64_t acc = 0;
    for (size_t t = 0; t < ntile; t++) { off[t] = (uint32_t)acc; acc += cnt[t]; }

    /* v2：条目自带坐标/等级（渲染时只读顺序数据，见 binfmt.h 的说明）。
       总条数 = Σcnt，一条边进几个 tile 就有几条，所以这里比边数大。 */
    uint8_t *xl = malloc((size_t)acc * NM_XL2_BYTES);
    if (!xl) {
        free(cnt); free(off);
        free(tx0); free(ty0); free(tx1); free(ty1);
        return -1;
    }
    /* 保存起始偏移副本（填充时 off 会被消耗） */
    uint32_t *off0 = malloc(ntile * sizeof(uint32_t));
    if (!off0) {
        free(cnt); free(off); free(xl);
        free(tx0); free(ty0); free(tx1); free(ty1);
        return -1;
    }
    memcpy(off0, off, ntile * sizeof(uint32_t));

    for (size_t i = 0; i < ne; i++) {
        uint32_t ax, bx, cy, dy;
        EDGE_TILE_RANGE(i, ax, bx, cy, dy);
        for (uint32_t y = cy; y <= dy; y++)
            for (uint32_t x = ax; x <= bx; x++) {
                size_t t = ((size_t)(y - miny) * nx) + (x - minx);
                uint8_t *rec = xl + (size_t)off[t] * NM_XL2_BYTES;
                uint32_t to = nm_get_u32((const uint8_t *)g->e_to.p + i * 4);
                nm_put_i32(rec + XL2_FROM_LAT, g->lat[ef[i]]);
                nm_put_i32(rec + XL2_FROM_LON, g->lon[ef[i]]);
                nm_put_i32(rec + XL2_TO_LAT,   g->lat[to]);
                nm_put_i32(rec + XL2_TO_LON,   g->lon[to]);
                nm_put_u16(rec + XL2_FLAGS,
                           nm_get_u16((const uint8_t *)g->e_flags.p + i * 2));
                rec[XL2_CLS] = ((const uint8_t *)g->e_cls.p)[i];
                rec[XL2_PAD] = 0;
                nm_put_u32(rec + XL2_EDGE, csr[i]);
                nm_put_u32(rec + XL2_NODE, ef[i]);
                off[t]++;
            }
    }

    char path[4096];
    int rc = 0;
    /* edgelist.bin */
    NMBinWriter *w = nmbin_create(OUTF2(path, dir, "edgelist.bin"),
                                  NMBIN_KIND_EDGELIST, NM_XL2_BYTES, src);
    if (!w) rc = -1;
    if (!rc) {
        nmbin_add_field(w, "from_lat", XL2_FROM_LAT, NMT_I32, 4);
        nmbin_add_field(w, "from_lon", XL2_FROM_LON, NMT_I32, 4);
        nmbin_add_field(w, "to_lat",   XL2_TO_LAT,   NMT_I32, 4);
        nmbin_add_field(w, "to_lon",   XL2_TO_LON,   NMT_I32, 4);
        nmbin_add_field(w, "flags",    XL2_FLAGS,    NMT_U16, 2);
        nmbin_add_field(w, "cls",      XL2_CLS,      NMT_U8,  1);
        nmbin_add_field(w, "edge_idx", XL2_EDGE,     NMT_U32, 4);
        nmbin_add_field(w, "node_idx", XL2_NODE,     NMT_U32, 4);
        if (nmbin_write_records(w, xl, acc) != 0) rc = -1;
        else if (nmbin_finalize(w) != 0) rc = -1;
    }
    /* tiles.bin：rec_count = ntile + 2，前 2 条记录是网格元数据
       {x0,y0} 和 {nx,ny}（复用 rec 布局，避免破坏 rec_size 校验） */
    if (!rc) {
        w = nmbin_create(OUTF2(path, dir, "tiles.bin"),
                         NMBIN_KIND_TILES, NM_TILE_REC_BYTES, src);
        if (!w) rc = -1;
        else {
            nmbin_add_field(w, "edge_off", TF_EDGE_OFF, NMT_U32, 4);
            nmbin_add_field(w, "edge_cnt", TF_EDGE_CNT, NMT_U32, 4);
            uint8_t meta0[NM_TILE_REC_BYTES], meta1[NM_TILE_REC_BYTES];
            nm_put_u32(meta0 + TF_EDGE_OFF, minx);
            nm_put_u32(meta0 + TF_EDGE_CNT, miny);
            nm_put_u32(meta1 + TF_EDGE_OFF, nx);
            nm_put_u32(meta1 + TF_EDGE_CNT, ny);
            if (nmbin_write_records(w, meta0, 1) != 0 ||
                nmbin_write_records(w, meta1, 1) != 0) rc = -1;
            for (size_t t = 0; t < ntile && !rc; t++) {
                uint8_t rec[NM_TILE_REC_BYTES];
                nm_put_u32(rec + TF_EDGE_OFF, off0[t]);
                nm_put_u32(rec + TF_EDGE_CNT, cnt[t]);
                if (nmbin_write_records(w, rec, 1) != 0) rc = -1;
            }
            if (!rc && nmbin_finalize(w) != 0) rc = -1;
        }
    }

    g->st.tiles_written = ntile;
    free(cnt); free(off); free(off0); free(xl);
    free(tx0); free(ty0); free(tx1); free(ty1);
    return rc;
    #undef EDGE_TILE_RANGE
}

/* ------------------------------------------------------------------ */
/* 入口                                                               */
/* ------------------------------------------------------------------ */
static void graph_free(Graph *g)
{
    buf_free(&g->ways); buf_free(&g->refs); buf_free(&g->restr); buf_free(&g->out_restr);
    buf_free(&g->lines); buf_free(&g->areas);
    buf_free(&g->e_from); buf_free(&g->e_to); buf_free(&g->e_dist); buf_free(&g->e_name);
    buf_free(&g->e_flags); buf_free(&g->e_cls); buf_free(&g->e_speed);
    buf_free(&g->e_lanes); buf_free(&g->e_way);
    free(g->ids); free(g->lat); free(g->lon); free(g->osm_id);
    free(g->node_edge_begin); free(g->node_edge_count);
    free(g->node_class_mask); free(g->node_flags); free(g->csr_of);
    sp_destroy(g->names);
}

int graph_build(const char *pbf_path, const char *out_dir, GraphBuildStats *st)
{
    clock_t t0 = clock();
    Graph g;
    memset(&g, 0, sizeof(g));
    g.names = sp_create(1 << 20);
    g.ways.esz = sizeof(WayRec);
    g.refs.esz = sizeof(int64_t);
    g.restr.esz = sizeof(PendRestr);
    g.out_restr.esz = NM_RESTR_BYTES;
    g.lines.esz = sizeof(LineRec);
    g.areas.esz = sizeof(AreaRec);
    if (!g.names) return -1;

    int rc = 0;
    if (pass1(&g, pbf_path) != 0) { rc = -1; goto done; }
    if (build_index(&g) != 0) { rc = -1; goto done; }
    if (pass2(&g, pbf_path) != 0) { rc = -1; goto done; }
    compact_nodes(&g);
    if (build_edges(&g) != 0) { rc = -1; goto done; }
    if (resolve_restrictions(&g) != 0) { rc = -1; goto done; }

    char path[4096];
    const char *src = strrchr(pbf_path, '/') ? strrchr(pbf_path, '/') + 1 : pbf_path;
#define OUTF(p, name) (snprintf(p, sizeof(p), "%s/%s", out_dir, name), (const char *)(p))
    if (write_nodes(&g, OUTF(path, "nodes.bin"), src) != 0) { rc = -1; goto done; }
    if (write_edges(&g, OUTF(path, "edges.bin"), src) != 0) { rc = -1; goto done; }
    if (write_osmid(&g, OUTF(path, "osmid.bin"), src) != 0) { rc = -1; goto done; }
    if (write_names(&g, OUTF(path, "names.bin"), src) != 0) { rc = -1; goto done; }
    if (write_restrictions(&g, OUTF(path, "restrictions.bin"), src) != 0) { rc = -1; goto done; }
    if (write_lines_areas(&g, out_dir, src) != 0) { rc = -1; goto done; }
    /* 空间索引失败不致命：渲染回退全量扫描 */
    if (write_spatial_index(&g, out_dir, src) != 0)
        fprintf(stderr, "警告: 空间索引生成失败，渲染将全量扫描\n");
#undef OUTF

done:
    g.st.names = sp_count(g.names);
    g.st.elapsed_sec = (double)(clock() - t0) / CLOCKS_PER_SEC;
    for (int i = 0; i < 4; i++) g.st.bbox[i] = (int32_t)g.bb[i];
    *st = g.st;
    graph_free(&g);
    return rc;
}

