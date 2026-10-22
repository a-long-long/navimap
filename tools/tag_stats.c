/* tag_stats —— 统计 PBF 里 way 的标签构成，回答"到底丢了什么"
 *
 * 用途：graph_extract 只保留可通行机动车道路，其余 way 一律丢弃。
 *       这个工具把"丢弃的那部分"按 key 和 highway 取值分门别类数出来，
 *       避免"地图信息不全"变成一笔糊涂账。
 *
 * 用法：tag_stats <file.osm.pbf> [--top=N]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "pbf/pbf_stream.h"
#include "pbf/pbf_block.h"

#define MAXK 64

typedef struct { const char *k; uint64_t n; } Ent;

static Ent keys[MAXK];
static int  nkeys;
static Ent hws[MAXK];
static int  nhws;

static void bump(Ent *a, int *n, const char *name)
{
    for (int i = 0; i < *n; i++)
        if (strcmp(a[i].k, name) == 0) { a[i].n++; return; }
    if (*n < MAXK) { a[*n].k = strdup(name); a[*n].n = 1; (*n)++; }
}

static int cmp_ent(const void *a, const void *b)
{
    uint64_t x = ((const Ent *)a)->n, y = ((const Ent *)b)->n;
    return (x < y) - (x > y);
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    int top = 15;
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--top=", 6) == 0) top = atoi(argv[i] + 6);
        else path = argv[i];
    }
    if (!path) { fprintf(stderr, "用法: tag_stats <file.osm.pbf> [--top=N]\n"); return 2; }

    PbfStream *s = pbf_stream_open(path);
    if (!s) { fprintf(stderr, "打不开 %s\n", path); return 1; }

    uint64_t ways = 0, with_hw = 0, closed = 0;
    const char *type; const uint8_t *buf; size_t len;
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
                ways++;
                if (w->n_refs >= 4 && w->refs[0] == w->refs[w->n_refs - 1]) closed++;
                int has_hw = 0;
                for (size_t k = 0; k < w->n_keys && k < w->n_vals; k++) {
                    const char *kk = pbf_str(blk, w->keys[k]);
                    const char *vv = pbf_str(blk, w->vals[k]);
                    if (!kk || !vv) continue;
                    bump(keys, &nkeys, kk);
                    if (strcmp(kk, "highway") == 0) { has_hw = 1; bump(hws, &nhws, vv); }
                }
                if (has_hw) with_hw++;
            }
        }
        pbf_block_free(blk);
    }
    pbf_stream_close(s);

    qsort(keys, nkeys, sizeof(Ent), cmp_ent);
    qsort(hws,  nhws,  sizeof(Ent), cmp_ent);

    printf("ways_total      %llu\n", (unsigned long long)ways);
    printf("ways_with_hwy   %llu  (%.1f%%)\n", (unsigned long long)with_hw,
           ways ? 100.0 * with_hw / ways : 0.0);
    printf("ways_closed     %llu  (首尾同点，可能是建筑/湖/院落)\n",
           (unsigned long long)closed);
    printf("\n-- way 上出现最多的 key（前 %d）--\n", top);
    for (int i = 0; i < nkeys && i < top; i++)
        printf("  %-22s %12llu\n", keys[i].k, (unsigned long long)keys[i].n);
    printf("\n-- highway 取值分布（前 %d）--\n", top);
    for (int i = 0; i < nhws && i < top; i++)
        printf("  %-22s %12llu\n", hws[i].k, (unsigned long long)hws[i].n);
    return 0;
}
