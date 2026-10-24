/* 状态表（开地址哈希）的实现。从 route.c 整块搬迁，行为与源码逐字一致。
 * 见 st.h 的接口与注释。 */

#include "route/st.h"
#include "route/route.h"   /* REDGE_SEAM */
#include <stdio.h>

/* 64 位整数混匀（splitmix64 变体），用于哈希；原 route.c 的 mix64。 */
static uint64_t mix64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n);
    if (!q) {
        fprintf(stderr, "路由: 内存不足（需要 %zu 字节）\n", n);
        exit(2);
    }
    return q;
}

void st_init(StateTab *t)
{
    t->cap = 1u << 20;
    t->slot = calloc(t->cap, sizeof(StSlot));
    t->cap_ents = 1u << 18;
    t->ents = malloc(t->cap_ents * sizeof(StEnt));
    if (!t->slot || !t->ents) {
        fprintf(stderr, "路由: 状态表分配失败\n");
        exit(2);
    }
}

void st_free(StateTab *t)
{
    free(t->slot); free(t->ents);
    memset(t, 0, sizeof(*t));
}

void st_rehash(StateTab *t)
{
    size_t ncap = t->cap * 2;
    StSlot *ns = calloc(ncap, sizeof(StSlot));
    if (!ns) { fprintf(stderr, "路由: 状态表扩容失败\n"); exit(2); }
    for (size_t i = 0; i < t->cap; i++) {
        if (!t->slot[i].h) continue;
        /* h 可以复用；下标要按新 cap 重算 ⇒ 需要 key ⇒ 从 ents 取（扩容很罕见） */
        uint64_t m = mix64(t->ents[t->slot[i].idx].key);
        size_t j = (size_t)(m & (ncap - 1));
        while (ns[j].h) j = (j + 1) & (ncap - 1);
        ns[j].h = t->slot[i].h;
        ns[j].idx = t->slot[i].idx;
    }
    free(t->slot);
    t->slot = ns; t->cap = ncap;
}

StEnt *st_intern(StateTab *t, uint64_t key)
{
    uint64_t m = mix64(key);
    uint32_t h = (uint32_t)(m >> 32) | 1u;
    size_t mask = t->cap - 1;
    size_t j = (size_t)(m & mask);
    while (t->slot[j].h) {
        if (t->slot[j].h == h && t->ents[t->slot[j].idx].key == key)
            return &t->ents[t->slot[j].idx];
        j = (j + 1) & mask;
    }
    if (t->n_ents == t->cap_ents) {
        t->cap_ents *= 2;
        t->ents = xrealloc(t->ents, t->cap_ents * sizeof(StEnt));
    }
    uint32_t ei = (uint32_t)t->n_ents++;
    t->ents[ei].key = key;
    t->ents[ei].g = INF_G;
    t->ents[ei].prev_idx = UINT32_MAX;
    t->ents[ei].eprov = 0;
    t->ents[ei].eidx = REDGE_SEAM;
    t->ents[ei].from_node = 0;
    t->ents[ei].rep_prov = 0;
    t->ents[ei].rep_node = 0;
    t->ents[ei].settled = 0;
    t->slot[j].h = h;
    t->slot[j].idx = ei;
    t->used++;
    if (t->used * 10 >= t->cap * 7) st_rehash(t);
    return &t->ents[ei];
}

StEnt *st_find(const StateTab *t, uint64_t key)
{
    uint64_t m = mix64(key);
    uint32_t h = (uint32_t)(m >> 32) | 1u;
    size_t mask = t->cap - 1;
    size_t j = (size_t)(m & mask);
    while (t->slot[j].h) {
        if (t->slot[j].h == h && t->ents[t->slot[j].idx].key == key)
            return &t->ents[t->slot[j].idx];
        j = (j + 1) & mask;
    }
    return NULL;
}
