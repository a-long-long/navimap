#include "utils/strpool.h"
#include <stdlib.h>
#include <string.h>

#define SP_EMPTY_ID 0u

struct StrPool {
    char    *blob;
    size_t   blob_len, blob_cap;

    uint32_t *off;        /* id → blob 内偏移（id 从 1 开始；off[0] = blob_len） */
    size_t    n, cap;

    /* 哈希：blob 偏移 → id。开放寻址，容量取 2 的幂。 */
    int32_t  *tab;
    size_t    tab_mask;
};

static uint64_t fnv1a(const char *s, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 1099511628211ull; }
    return h;
}

StrPool *sp_create(size_t cap_hint)
{
    StrPool *p = calloc(1, sizeof(StrPool));
    if (!p) return NULL;
    p->blob_cap = cap_hint ? cap_hint : 1u << 16;
    p->cap      = 1024;
    p->blob     = malloc(p->blob_cap);
    p->off      = malloc(p->cap * sizeof(uint32_t));
    p->tab_mask = 2047;
    p->tab      = malloc((p->tab_mask + 1) * sizeof(int32_t));
    if (!p->blob || !p->off || !p->tab) { sp_destroy(p); return NULL; }
    memset(p->tab, 0xff, (p->tab_mask + 1) * sizeof(int32_t));
    p->n = 1;              /* id 0 = 空串 */
    p->off[0] = 0;
    p->blob_len = 1;       /* blob[0] = '\0' */
    p->blob[0] = '\0';
    return p;
}

void sp_destroy(StrPool *p)
{
    if (!p) return;
    free(p->blob); free(p->off); free(p->tab); free(p);
}

static void sp_grow_tab(StrPool *p)
{
    if ((p->n + 1) * 4 < (p->tab_mask + 1) * 3) return;
    size_t nmask = (p->tab_mask + 1) * 2 - 1;
    int32_t *nt = malloc((nmask + 1) * sizeof(int32_t));
    if (!nt) return;
    memset(nt, 0xff, (nmask + 1) * sizeof(int32_t));
    for (size_t i = 0; i <= p->tab_mask; i++) {
        if (p->tab[i] < 0) continue;
        uint32_t id = (uint32_t)p->tab[i];
        const char *s = p->blob + p->off[id];
        uint64_t h = fnv1a(s, strlen(s));
        size_t j = (size_t)(h & nmask);
        while (nt[j] >= 0) j = (j + 1) & nmask;
        nt[j] = (int32_t)id;
    }
    free(p->tab);
    p->tab = nt;
    p->tab_mask = nmask;
}

uint32_t sp_intern(StrPool *p, const char *s, size_t len)
{
    if (!s || len == 0) return SP_EMPTY_ID;

    uint64_t h = fnv1a(s, len);
    size_t j = (size_t)(h & p->tab_mask);
    while (p->tab[j] >= 0) {
        uint32_t id = (uint32_t)p->tab[j];
        const char *e = p->blob + p->off[id];
        if (strlen(e) == len && memcmp(e, s, len) == 0) return id;
        j = (j + 1) & p->tab_mask;
    }

    /* 新串 */
    if (p->blob_len + len + 1 > p->blob_cap) {
        size_t ncap = p->blob_cap;
        while (ncap < p->blob_len + len + 1) ncap *= 2;
        char *nb = realloc(p->blob, ncap);
        if (!nb) return SP_EMPTY_ID;
        p->blob = nb; p->blob_cap = ncap;
    }
    if (p->n + 2 > p->cap) {   /* 还要写 off[n+1]，多留一格 */
        size_t ncap = p->cap * 2;
        uint32_t *no = realloc(p->off, ncap * sizeof(uint32_t));
        if (!no) return SP_EMPTY_ID;
        p->off = no; p->cap = ncap;
    }
    uint32_t id = (uint32_t)p->n;
    p->off[id] = (uint32_t)p->blob_len;
    memcpy(p->blob + p->blob_len, s, len);
    p->blob_len += len;
    p->blob[p->blob_len++] = '\0';
    p->off[p->n + 1] = (uint32_t)p->blob_len;   /* 便于取长度 */
    p->n++;

    p->tab[j] = (int32_t)id;
    sp_grow_tab(p);
    return id;
}

uint32_t sp_intern_cstr(StrPool *p, const char *s)
{
    return s ? sp_intern(p, s, strlen(s)) : SP_EMPTY_ID;
}

const char *sp_get(StrPool *p, uint32_t id)
{
    if (!p || id == 0 || id >= p->n) return "";
    return p->blob + p->off[id];
}

size_t sp_count(StrPool *p) { return p ? p->n - 1 : 0; }
size_t sp_bytes(StrPool *p) { return p ? p->blob_len : 0; }

void sp_serialize(StrPool *p, uint8_t **out, size_t *out_len)
{
    size_t cnt = p->n;                      /* 含 id 0 */
    size_t len = 4 + 4 * (cnt + 1) + p->blob_len;
    uint8_t *buf = malloc(len);
    if (!buf) { *out = NULL; *out_len = 0; return; }
    uint32_t *u32 = (uint32_t *)buf;
    u32[0] = (uint32_t)cnt;
    for (size_t i = 0; i <= cnt; i++) u32[1 + i] = p->off[i];
    memcpy(buf + 4 + 4 * (cnt + 1), p->blob, p->blob_len);
    *out = buf; *out_len = len;
}
