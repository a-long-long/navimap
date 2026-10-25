/* 校验工具：PBF 按需解码出的显示层 vs nmbin 落盘的显示层，逐字节比对。
 *
 * 用法: pbfmap_check <province_dir> <xxx.osm.pbf>
 *   province_dir 里应有 nodes.bin / areas.bin / lines.bin / polyrefs.bin
 *
 * 这是"只留 PBF"方案的核心门禁：解码结果必须与落盘结果一致，
 * 否则渲染会画出不同的图。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "map/binfmt.h"
#include "map/pbfmap.h"

static int cmp_file(const char *tag, const NMBinFile *a, const NMBinFile *b)
{
    if (a->hdr.rec_count != b->hdr.rec_count) {
        printf("  %-10s 记录数不同: 磁盘 %llu / 解码 %llu\n", tag,
               (unsigned long long)a->hdr.rec_count,
               (unsigned long long)b->hdr.rec_count);
        return 1;
    }
    if (a->hdr.rec_size != b->hdr.rec_size) {
        printf("  %-10s 记录大小不同: %u / %u\n", tag, a->hdr.rec_size, b->hdr.rec_size);
        return 1;
    }
    size_t n = (size_t)a->hdr.rec_count * a->hdr.rec_size;
    if (n && memcmp(a->data, b->data, n) != 0) {
        /* 找第一处不同 */
        size_t i = 0;
        while (i < n && a->data[i] == b->data[i]) i++;
        printf("  %-10s 内容不同 @%zu (记录 %zu, 字节偏移 %zu)\n",
               tag, i, i / a->hdr.rec_size, i % a->hdr.rec_size);
        return 1;
    }
    printf("  %-10s 一致 (%llu 条, %zu 字节)\n", tag,
           (unsigned long long)a->hdr.rec_count, n);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "用法: %s <province_dir> <file.osm.pbf>\n", argv[0]);
        return 2;
    }
    const char *dir = argv[1];
    const char *pbf = argv[2];

    NMBinFile dn, da, dl, dp;
    char path[4096];
    snprintf(path, sizeof(path), "%s/nodes.bin", dir);
    if (nmbin_open(path, &dn) != 0) { fprintf(stderr, "打不开 %s\n", path); return 2; }
    snprintf(path, sizeof(path), "%s/areas.bin", dir);
    if (nmbin_open(path, &da) != 0) { fprintf(stderr, "打不开 %s\n", path); return 2; }
    snprintf(path, sizeof(path), "%s/lines.bin", dir);
    if (nmbin_open(path, &dl) != 0) { fprintf(stderr, "打不开 %s\n", path); return 2; }
    snprintf(path, sizeof(path), "%s/polyrefs.bin", dir);
    if (nmbin_open(path, &dp) != 0) { fprintf(stderr, "打不开 %s\n", path); return 2; }

    printf("=== %s vs %s ===\n", dir, pbf);
    PbfMap m;
    if (pbfmap_decode(pbf, &dn, &m, NULL, NULL) != 0) {
        fprintf(stderr, "解码失败\n");
        return 2;
    }

    int bad = 0;
    /* nodes 不由本模块产出：polyrefs 的下标空间就是 nmbin nodes.bin 的下标空间，
     * 坐标直接取自 nmbin。这里只校验三件套。 */
    bad |= cmp_file("areas", &da, &m.areas);
    bad |= cmp_file("lines", &dl, &m.lines);
    bad |= cmp_file("polyrefs", &dp, &m.polyrefs);

    pbfmap_free(&m);
    nmbin_close(&dn); nmbin_close(&da); nmbin_close(&dl); nmbin_close(&dp);

    printf(bad ? "DIFF\n" : "IDENTICAL\n");
    return bad ? 1 : 0;
}
