#include "route/lmtable.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

size_t lm_cell_of(const LmTable *t, int32_t lat_e7, int32_t lon_e7)
{
    if (!t->loaded) return (size_t)-1;
    int64_t r = ((int64_t)lat_e7 - (int64_t)t->h.lat0_e7) / (int64_t)t->h.cell_e7;
    int64_t c = ((int64_t)lon_e7 - (int64_t)t->h.lon0_e7) / (int64_t)t->h.cell_e7;
    if (r < 0 || c < 0) return (size_t)-1;
    if ((uint64_t)r >= t->h.nrow || (uint64_t)c >= t->h.ncol) return (size_t)-1;
    return (size_t)r * t->h.ncol + (size_t)c;
}

double lm_bound_m(const LmTable *t, size_t tc, int32_t lat_e7, int32_t lon_e7)
{
    if (!t->loaded || tc == (size_t)-1) return -1.0;
    size_t uc = lm_cell_of(t, lat_e7, lon_e7);
    if (uc == (size_t)-1) return -1.0;

    uint32_t nlm = t->h.nlm;
    const uint16_t *tb = t->v + tc * nlm;
    const uint16_t *ub = t->v + uc * nlm;
    int best = -1;
    for (uint32_t k = 0; k < nlm; k++) {
        int a = tb[k], b = ub[k];
        if (a == (int)LM_NONE || b == (int)LM_NONE) continue;
        int d = a - b;
        if (d > best) best = d;
    }
    if (best < 0) return -1.0;
    double m = (double)best * (double)t->h.unit_m - (double)t->h.penalty_m;
    return m > 0.0 ? m : 0.0;
}

int lm_table_init(LmTable *t, int32_t lat0_e7, int32_t lon0_e7, uint32_t cell_e7,
                  uint32_t nrow, uint32_t ncol, uint32_t nlm, uint32_t penalty_m,
                  uint32_t nprov, uint64_t total_nodes)
{
    if (!t || cell_e7 == 0 || nrow == 0 || ncol == 0 || nlm == 0) return -1;
    memset(t, 0, sizeof(*t));
    t->h.magic = LM_MAGIC;
    t->h.version = LM_VERSION;
    t->h.lat0_e7 = lat0_e7;
    t->h.lon0_e7 = lon0_e7;
    t->h.cell_e7 = cell_e7;
    t->h.nrow = nrow;
    t->h.ncol = ncol;
    t->h.nlm = nlm;
    t->h.unit_m = LM_UNIT_M;
    t->h.penalty_m = penalty_m;
    t->h.nprov = nprov;
    t->h.total_nodes = total_nodes;
    t->ncell = (size_t)nrow * ncol;
    t->v = malloc(t->ncell * nlm * sizeof(uint16_t));
    if (!t->v) return -1;
    for (size_t i = 0; i < t->ncell * nlm; i++) t->v[i] = LM_NONE;
    t->loaded = 1;
    return 0;
}

void lm_table_free(LmTable *t)
{
    if (!t) return;
    free(t->v);
    memset(t, 0, sizeof(*t));
}

static int wr(const void *p, size_t n, FILE *f)
{
    return fwrite(p, 1, n, f) == n ? 0 : -1;
}

int lm_table_save(const LmTable *t, const char *path)
{
    if (!t->loaded) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (wr(&t->h, sizeof(t->h), f) != 0) { fclose(f); return -1; }
    if (wr(t->v, t->ncell * t->h.nlm * sizeof(uint16_t), f) != 0) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

int lm_table_match(const LmTable *t, uint32_t nprov, uint64_t total_nodes)
{
    if (!t || !t->loaded) return 0;
    return (t->h.nprov == nprov && t->h.total_nodes == total_nodes) ? 1 : 0;
}

int lm_table_load(LmTable *t, const char *dir)
{
    memset(t, 0, sizeof(*t));
    char path[4096 + 32];
    snprintf(path, sizeof(path), "%s/lm.bin", dir);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    LmHeader h;
    if (fread(&h, sizeof(h), 1, f) != 1) { fclose(f); return -1; }
    if (h.magic != LM_MAGIC || h.version != LM_VERSION || h.cell_e7 == 0 ||
        h.nrow == 0 || h.ncol == 0 || h.nlm == 0) {
        fclose(f);
        return -1;
    }
    size_t ncell = (size_t)h.nrow * h.ncol;
    size_t need = ncell * h.nlm * sizeof(uint16_t);
    uint16_t *v = malloc(need);
    if (!v) { fclose(f); return -1; }
    if (fread(v, 1, need, f) != need) { free(v); fclose(f); return -1; }
    fclose(f);
    t->h = h;
    t->v = v;
    t->ncell = ncell;
    t->loaded = 1;
    return 0;
}
