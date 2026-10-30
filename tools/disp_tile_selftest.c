/* skedaddle_disp_tile_selftest —— 显示层分 tile 的覆盖性自测（Phase 1 验收）。
 *
 * 判据（行为等价，非逐字节）：把整省 areas/lines/polyrefs 与 disp/ 下全部
 * tile 的同类数据对比，要求**每个整省要素（osm_id + 节点内部下标序列）至少
 * 出现在一个 tile 中**——即分 tile 不丢要素。跨 tile 重复（大要素边界保护）
 * 是允许的，故只断言"全覆盖"，并报告 tile 要素总数 >= 整省。
 *
 * 用法：skedaddle_disp_tile_selftest --graph=<省目录> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "map/binfmt.h"

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "  ✗ " __VA_ARGS__); g_fail++; } } while (0)

typedef struct { uint64_t osm; int idx; } Key;

static int cmp_key(const void *a, const void *b) {
    uint64_t x = ((const Key *)a)->osm, y = ((const Key *)b)->osm;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

static int find_osm(const Key *keys, int n, uint64_t osm) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        if (keys[mid].osm == osm) return keys[mid].idx;
        if (keys[mid].osm < osm) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

static int seq_match(const uint8_t *mpoly, uint64_t moff, uint32_t mcnt,
                     const uint8_t *tpoly, uint64_t toff, uint32_t tcnt) {
    if (mcnt != tcnt) return 0;
    for (uint32_t k = 0; k < mcnt; k++) {
        uint32_t a = nm_get_u32(mpoly + (moff + k) * 4);
        uint32_t b = nm_get_u32(tpoly + (toff + k) * 4);
        if (a != b) return 0;
    }
    return 1;
}

/* 校验一层（areas 或 lines）覆盖。 */
static int check_layer(const char *dir, const char *disp, const char *mono_name,
                       const char *tile_ext, int off_field, int cnt_field,
                       uint64_t *out_mono, uint64_t *out_tile)
{
    char path[4096];
    NMBinFile mono, monop;
    snprintf(path, sizeof(path), "%s/%s", dir, mono_name);
    if (nmbin_open(path, &mono) != 0) { fprintf(stderr, "  无法打开 %s\n", path); return 1; }
    snprintf(path, sizeof(path), "%s/polyrefs.bin", dir);
    if (nmbin_open(path, &monop) != 0) { fprintf(stderr, "  无法打开 %s/polyrefs.bin\n", dir); nmbin_close(&mono); return 1; }

    uint64_t n = mono.hdr.rec_count;
    Key *keys = malloc((size_t)n * sizeof(Key));
    if (!keys) { nmbin_close(&mono); nmbin_close(&monop); return 1; }
    for (uint64_t i = 0; i < n; i++) {
        const uint8_t *r = mono.data + i * mono.hdr.rec_size;
        keys[i].osm = nm_get_u64(r + 0);
        keys[i].idx = (int)i;
    }
    qsort(keys, (size_t)n, sizeof(Key), cmp_key);

    uint8_t *found = calloc((size_t)n, 1);
    uint64_t tile_total = 0;

    FILE *idx = fopen(disp, "rb");
    if (!idx) { fprintf(stderr, "  无法打开 %s\n", disp); free(keys); free(found); nmbin_close(&mono); nmbin_close(&monop); return 1; }
    char line[256]; int in_list = 0; long c, r;
    while (fgets(line, sizeof(line), idx)) {
        if (strncmp(line, "ntiles", 6) == 0) { in_list = 1; continue; }
        if (!in_list) continue;
        if (sscanf(line, "%ld %ld", &c, &r) != 2) continue;
        snprintf(path, sizeof(path), "%s/t_%ld_%ld.%s", dir, c, r, tile_ext);
        NMBinFile ta, tp;
        if (nmbin_open(path, &ta) != 0) continue;
        snprintf(path, sizeof(path), "%s/t_%ld_%ld.polyrefs", dir, c, r);
        if (nmbin_open(path, &tp) != 0) { nmbin_close(&ta); continue; }
        uint64_t nt = ta.hdr.rec_count;
        tile_total += nt;
        for (uint64_t j = 0; j < nt; j++) {
            const uint8_t *tr = ta.data + j * ta.hdr.rec_size;
            uint64_t tosm = nm_get_u64(tr + 0);
            uint32_t toff = nm_get_u32(tr + off_field);
            uint32_t tcnt = nm_get_u32(tr + cnt_field);
            int mi = find_osm(keys, (int)n, tosm);
            if (mi < 0) continue;
            if (found[mi]) continue;
            const uint8_t *mr = mono.data + (uint64_t)mi * mono.hdr.rec_size;
            uint32_t moff = nm_get_u32(mr + off_field);
            uint32_t mcnt = nm_get_u32(mr + cnt_field);
            if (seq_match(monop.data, moff, mcnt, tp.data, toff, tcnt))
                found[mi] = 1;
        }
        nmbin_close(&ta); nmbin_close(&tp);
    }
    fclose(idx);

    uint64_t missing = 0;
    for (uint64_t i = 0; i < n; i++) if (!found[i]) missing++;
    CHECK(missing == 0, "layer %s: %llu 条整省要素在 tile 中找不到（丢失！）\n",
          mono_name, (unsigned long long)missing);
    if (missing) {
        uint64_t shown = 0;
        for (uint64_t i = 0; i < n && shown < 5; i++) if (!found[i]) {
            fprintf(stderr, "    丢失 osm_id=%llu\n", (unsigned long long)keys[i].osm);
            shown++;
        }
    }
    CHECK(tile_total >= n, "layer %s: tile 要素总数 %llu 应 >= 整省 %llu\n",
          mono_name, (unsigned long long)tile_total, (unsigned long long)n);

    printf("  %-10s 整省 %llu 条 / tile 合计 %llu 条 / 丢失 %llu 条\n",
           mono_name, (unsigned long long)n, (unsigned long long)tile_total,
           (unsigned long long)missing);

    free(keys); free(found);
    nmbin_close(&mono); nmbin_close(&monop);
    *out_mono = n; *out_tile = tile_total;
    return (missing == 0) ? 0 : 1;
}

int main(int argc, char **argv)
{
    const char *dir = NULL;
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--graph=", 8) == 0) dir = argv[i] + 8;
    }
    if (!dir) { fprintf(stderr, "用法: %s --graph=<省目录>\n", argv[0]); return 2; }

    char disp[4096];
    snprintf(disp, sizeof(disp), "%s/disp.idx", dir);
    {
        FILE *probe = fopen(disp, "rb");
        if (!probe) {
            fprintf(stderr, "  ✗ %s 不存在：该省未做显示层分 tile\n", disp);
            return 1;
        }
        fclose(probe);
    }

    printf("disp tile 覆盖自测: %s\n", dir);
    uint64_t ma, ta, ml, tl;
    int rc = 0;
    rc |= check_layer(dir, disp, "areas.bin",  "areas",  AF_REF_OFF, AF_REF_CNT, &ma, &ta);
    rc |= check_layer(dir, disp, "lines.bin",  "lines",  LF_REF_OFF, LF_REF_CNT, &ml, &tl);

    if (rc == 0) {
        printf("DISP-TILE-OK  面 %llu / 线 %llu 全部覆盖，无丢失\n",
               (unsigned long long)ma, (unsigned long long)ml);
        return 0;
    }
    printf("DISP-TILE-FAIL\n");
    return 1;
}
