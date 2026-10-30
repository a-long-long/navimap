/* skedaddle_restr_probe —— via=way 转向限制的形态探针。
 *
 * 存在理由：graph_build 直接跳过所有 via=way 的限制（北京 182 条 / 约 28%），
 * 但要决定"怎么展开"得先知道数据长什么样——via way 有多长、两端能否定位到
 * 具体节点、公共节点是不是唯一。凭想象设计展开方案会做错。
 *
 * 两遍扫描：
 *   第 1 遍：扫 relations，挑出 via 成员是 way 的 restriction，记下 from/via/to
 *   第 2 遍：扫 ways，把涉及到的 way 的节点引用抓下来（只要这几百条）
 * 然后统计：via way 长度分布、from∩via 与 via∩to 的公共节点个数。
 *
 * 用法：./skedaddle_restr_probe data/beijing.osm.pbf [--dump=N]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "pbf/pbf_stream.h"
#include "pbf/pbf_block.h"
#include "utils/mem.h"

typedef struct {
    int64_t  from_way, via_way, to_way;
    char     rv[32];        /* restriction 取值，如 "no_left_turn" */
    int      n_from_via;    /* from 与 via 的公共节点数 */
    int      n_via_to;      /* via 与 to 的公共节点数 */
    size_t   via_len;       /* via way 的节点数 */
    int64_t  join_a, join_b;/* 唯一公共节点时记下它 */
} Restr;

typedef struct {
    int64_t  id;
    int64_t *refs;
    size_t   n;
    size_t   cap;
} WayRefs;

static WayRefs *g_wr = NULL;
static size_t   g_nwr = 0, g_capwr = 0;

static int cmp_wr(const void *a, const void *b)
{
    const WayRefs *x = a, *y = b;
    return (x->id > y->id) - (x->id < y->id);
}

static WayRefs *wr_find(int64_t id)
{
    size_t lo = 0, hi = g_nwr;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (g_wr[mid].id == id) return &g_wr[mid];
        if (g_wr[mid].id < id) lo = mid + 1; else hi = mid;
    }
    return NULL;
}

static WayRefs *wr_add(int64_t id)
{
    if (g_nwr == g_capwr) {
        size_t nc = g_capwr ? g_capwr * 2 : 256;
        WayRefs *p = realloc(g_wr, nc * sizeof(WayRefs));
        if (!p) return NULL;
        g_wr = p; g_capwr = nc;
    }
    WayRefs *w = &g_wr[g_nwr++];
    memset(w, 0, sizeof(*w));
    w->id = id;
    return w;
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    int dump = 0;

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--dump=", 7) == 0) dump = atoi(argv[i] + 7);
        else if (argv[i][0] != '-') path = argv[i];
    }
    if (!path) { fprintf(stderr, "用法: skedaddle_restr_probe <file.osm.pbf> [--dump=N]\n"); return 2; }

    /* ---------- 第 1 遍：relations ---------- */
    Restr *rs = NULL; size_t nr = 0, capr = 0;
    PbfStream *s = pbf_stream_open(path);
    if (!s) { fprintf(stderr, "打不开 %s\n", path); return 1; }

    const char *type; const uint8_t *buf; size_t len;
    while (pbf_stream_next(s, &type, &buf, &len) == PBF_OK) {
        if (!type || strcmp(type, "OSMData") != 0) continue;
        PbfBlock *blk = NULL;
        if (pbf_unpack_block(buf, len, &blk) != 0) break;

        for (size_t gi = 0; gi < blk->pb->n_primitivegroup; gi++) {
            PrimitiveGroup *pg = blk->pb->primitivegroup[gi];
            if (!pg) continue;
            for (size_t i = 0; i < pg->n_relations; i++) {
                Relation *r = pg->relations[i];
                if (!r) continue;
                int is_restr = 0; const char *rv = NULL;
                size_t nkv = r->n_keys < r->n_vals ? r->n_keys : r->n_vals;
                for (size_t k = 0; k < nkv; k++) {
                    const char *kk = pbf_str(blk, r->keys[k]);
                    const char *vv = pbf_str(blk, r->vals[k]);
                    if (!kk || !vv) continue;
                    if (strcmp(kk, "type") == 0 && strcmp(vv, "restriction") == 0) is_restr = 1;
                    else if (strcmp(kk, "restriction") == 0) rv = vv;
                }
                if (!is_restr) continue;

                int64_t fw = 0, tw = 0, vw = 0;
                int have_f = 0, have_t = 0, have_vw = 0, via_node = 0;
                size_t nm = r->n_memids;
                if (nm > r->n_roles_sid) nm = r->n_roles_sid;
                if (nm > r->n_types) nm = r->n_types;
                int64_t acc = 0;
                for (size_t k = 0; k < nm; k++) {
                    acc += r->memids[k];
                    const char *role = pbf_str(blk, r->roles_sid[k]);
                    if (!role) continue;
                    if (strcmp(role, "from") == 0)      { fw = acc; have_f = 1; }
                    else if (strcmp(role, "to") == 0)   { tw = acc; have_t = 1; }
                    else if (strcmp(role, "via") == 0) {
                        if (r->types[k] == RELATION__MEMBER_TYPE__NODE) via_node = 1;
                        else { vw = acc; have_vw = 1; }
                    }
                }
                if (via_node || !have_vw || !have_f || !have_t) continue;

                if (nr == capr) {
                    size_t nc = capr ? capr * 2 : 256;
                    Restr *p = realloc(rs, nc * sizeof(Restr));
                    if (!p) { pbf_block_free(blk); return 1; }
                    rs = p; capr = nc;
                }
                Restr *e = &rs[nr++];
                memset(e, 0, sizeof(*e));
                e->from_way = fw; e->via_way = vw; e->to_way = tw;
                snprintf(e->rv, sizeof(e->rv), "%s", rv ? rv : "");
                wr_add(fw); wr_add(vw); wr_add(tw);
            }
        }
        pbf_block_free(blk);
    }
    pbf_stream_close(s);

    printf("via=way 限制        %zu 条\n", nr);
    if (nr == 0) return 0;

    /* ---------- 第 2 遍：抓这些 way 的节点引用 ---------- */
    qsort(g_wr, g_nwr, sizeof(WayRefs), cmp_wr);
    s = pbf_stream_open(path);
    if (!s) return 1;
    while (pbf_stream_next(s, &type, &buf, &len) == PBF_OK) {
        if (!type || strcmp(type, "OSMData") != 0) continue;
        PbfBlock *blk = NULL;
        if (pbf_unpack_block(buf, len, &blk) != 0) break;
        for (size_t gi = 0; gi < blk->pb->n_primitivegroup; gi++) {
            PrimitiveGroup *pg = blk->pb->primitivegroup[gi];
            if (!pg) continue;
            for (size_t i = 0; i < pg->n_ways; i++) {
                Way *w = pg->ways[i];
                if (!w) continue;
                WayRefs *wr = wr_find(w->id);
                if (!wr) continue;
                wr->n = w->n_refs;
                wr->refs = malloc(w->n_refs * sizeof(int64_t));
                if (!wr->refs) { wr->n = 0; continue; }
                wr->cap = w->n_refs;
                int64_t acc = 0;
                for (size_t k = 0; k < w->n_refs; k++) {
                    acc += w->refs[k];
                    wr->refs[k] = acc;
                }
            }
        }
        pbf_block_free(blk);
    }
    pbf_stream_close(s);

    /* ---------- 分析 ---------- */
    size_t n_ok = 0, n_nofrom = 0, n_novia = 0, n_noto = 0;
    size_t n_ambig = 0, n_zero = 0;
    size_t len1 = 0, len2 = 0, len3p = 0, lenmax = 0;
    size_t n_nojoin_a = 0, n_nojoin_b = 0;

    for (size_t i = 0; i < nr; i++) {
        Restr *e = &rs[i];
        WayRefs *f = wr_find(e->from_way), *v = wr_find(e->via_way), *t = wr_find(e->to_way);
        if (!f || !f->n) { n_nofrom++; continue; }
        if (!v || !v->n) { n_novia++;  continue; }
        if (!t || !t->n) { n_noto++;   continue; }
        e->via_len = v->n;
        if (v->n > lenmax) lenmax = v->n;
        if (v->n <= 1) len1++; else if (v->n == 2) len2++; else len3p++;

        /* 公共节点 */
        int64_t ja[8]; int nja = 0;
        for (size_t a = 0; a < f->n; a++)
            for (size_t b = 0; b < v->n && nja < 8; b++)
                if (f->refs[a] == v->refs[b]) { ja[nja++] = f->refs[a]; break; }
        int64_t jb[8]; int njb = 0;
        for (size_t a = 0; a < v->n; a++)
            for (size_t b = 0; b < t->n && njb < 8; b++)
                if (v->refs[a] == t->refs[b]) { jb[njb++] = v->refs[a]; break; }
        e->n_from_via = nja; e->n_via_to = njb;

        if (nja == 0) { n_nojoin_a++; continue; }
        if (njb == 0) { n_nojoin_b++; continue; }
        if (nja > 1 || njb > 1) { n_ambig++; continue; }
        if (ja[0] == jb[0]) { n_zero++; continue; }   /* A==B：退化成单点限制 */
        e->join_a = ja[0]; e->join_b = jb[0];
        n_ok++;
    }

    printf("  缺 from way       %zu\n", n_nofrom);
    printf("  缺 via way        %zu\n", n_novia);
    printf("  缺 to way         %zu\n", n_noto);
    printf("  from∩via 无交点   %zu\n", n_nojoin_a);
    printf("  via∩to 无交点     %zu\n", n_nojoin_b);
    printf("  交点不唯一        %zu\n", n_ambig);
    printf("  A==B（退化单点）  %zu\n", n_zero);
    printf("  ★ 可精确定位      %zu 条（%.1f%%）\n", n_ok, 100.0 * n_ok / nr);
    printf("via way 节点数      1个=%zu  2个=%zu  ≥3个=%zu  最长=%zu\n",
           len1, len2, len3p, lenmax);

    if (dump) {
        printf("\n样例（前 %d 条可定位的）：\n", dump);
        size_t shown = 0;
        for (size_t i = 0; i < nr && shown < (size_t)dump; i++) {
            if (!rs[i].join_a || !rs[i].join_b) continue;
            printf("  %-16s from=%" PRId64 " via=%" PRId64 "(%" PRIu64 "节点) to=%" PRId64
                   "  A=%" PRId64 " B=%" PRId64 "\n",
                   rs[i].rv, rs[i].from_way, rs[i].via_way,
                   (uint64_t)rs[i].via_len, rs[i].to_way, rs[i].join_a, rs[i].join_b);
            shown++;
        }
    }

    for (size_t i = 0; i < g_nwr; i++) free(g_wr[i].refs);
    free(g_wr); free(rs);
    return 0;
}
