/* pbf_info —— PBF 文件的"体检表"。
 *
 * 存在理由：本机没有 xvfb-run / valgrind，GUI 无法无头验证，所以解析层
 * 必须能在命令行上被断言。这里的输出是后续所有对账的基准：
 *   - 北京：way 总数应约 530,968（= 旧 meta.txt 的 ways_kept 273,524 + ways_skip 257,444）
 *   - 天津：旧 graphset 里只抽出 62 个节点，用本工具看它到底有多少 way
 *
 * 用法：
 *   ./pbf_info data/beijing.osm.pbf
 *   ./pbf_info --expect-ways=530968 data/beijing.osm.pbf    # 带断言，不符返回非 0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>

#include <protobuf-c/protobuf-c.h>

#include "pbf/pbf_stream.h"
#include "pbf/pbf_block.h"
#include "utils/mem.h"

typedef struct {
    uint64_t blobs, header_blobs, data_blobs;
    uint64_t dense_nodes, plain_nodes, ways, relations;
    uint64_t way_refs;          /* refs 总数（可用于估算边数） */
    uint64_t max_way_refs;      /* 单条 way 的最大节点数 */
    uint64_t string_bytes;
    uint64_t blocks_without_dense;
    double   min_lat, max_lat, min_lon, max_lon;
    int      have_bbox;
} Stats;

static void bbox_update_deg(Stats *st, double lat, double lon)
{
    if (!st->have_bbox) {
        st->min_lat = st->max_lat = lat;
        st->min_lon = st->max_lon = lon;
        st->have_bbox = 1;
        return;
    }
    if (lat < st->min_lat) st->min_lat = lat;
    if (lat > st->max_lat) st->max_lat = lat;
    if (lon < st->min_lon) st->min_lon = lon;
    if (lon > st->max_lon) st->max_lon = lon;
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    uint64_t expect_ways = 0;
    int check_ways = 0;
    int verbose = 0;

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--expect-ways=", 14) == 0) {
            expect_ways = strtoull(argv[i] + 14, NULL, 10);
            check_ways = 1;
        } else if (strcmp(argv[i], "-v") == 0) {
            verbose = 1;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "未知参数: %s\n", argv[i]);
            return 2;
        } else {
            path = argv[i];
        }
    }
    if (!path) {
        fprintf(stderr, "用法: %s [--expect-ways=N] [-v] <file.osm.pbf>\n", argv[0]);
        return 2;
    }

    clock_t t0 = clock();
    PbfStream *s = pbf_stream_open(path);
    if (!s) { fprintf(stderr, "打不开: %s\n", path); return 2; }

    Stats st; memset(&st, 0, sizeof(st));
    const char *type; const uint8_t *buf; size_t len;
    PbfStatus rc;

    while ((rc = pbf_stream_next(s, &type, &buf, &len)) == PBF_OK) {
        st.blobs++;
        if (type && strcmp(type, "OSMHeader") == 0) {
            st.header_blobs++;
            HeaderBlock *hb = header_block__unpack(NULL, len, buf);
            if (hb && hb->bbox) {
                printf("header_bbox %.7f %.7f %.7f %.7f\n",
                       hb->bbox->left   / 1e9, hb->bbox->bottom / 1e9,
                       hb->bbox->right  / 1e9, hb->bbox->top    / 1e9);
            }
            if (hb && hb->writingprogram) printf("writingprogram %s\n", hb->writingprogram);
            if (hb) header_block__free_unpacked(hb, NULL);
            continue;
        }
        st.data_blobs++;

        PbfBlock *blk = NULL;
        if (pbf_unpack_block(buf, len, &blk) != 0) {
            fprintf(stderr, "PrimitiveBlock 解析失败（偏移 %llu）\n",
                    (unsigned long long)pbf_stream_offset(s));
            pbf_stream_close(s);
            return 1;
        }

        if (blk->pb->stringtable) {
            for (size_t i = 0; i < blk->pb->stringtable->n_s; i++)
                st.string_bytes += blk->pb->stringtable->s[i].len + 1;
        }

        int saw_dense = 0;
        for (size_t g = 0; g < blk->pb->n_primitivegroup; g++) {
            PrimitiveGroup *pg = blk->pb->primitivegroup[g];
            if (!pg) continue;

            st.plain_nodes += pg->n_nodes;
            for (size_t i = 0; i < pg->n_nodes; i++) {
                if (!pg->nodes[i]) continue;
                bbox_update_deg(&st, pg->nodes[i]->lat / 1e9, pg->nodes[i]->lon / 1e9);
            }

            if (pg->dense) {
                saw_dense = 1;
                st.dense_nodes += pg->dense->n_id;
                /* delta 解码只为统计 bbox，不落盘 */
                int64_t acc_id = 0, acc_lat = 0, acc_lon = 0;
                for (size_t i = 0; i < pg->dense->n_id; i++) {
                    acc_id  += pg->dense->id[i];
                    acc_lat += (i < pg->dense->n_lat) ? pg->dense->lat[i] : 0;
                    acc_lon += (i < pg->dense->n_lon) ? pg->dense->lon[i] : 0;
                    (void)acc_id;
                    bbox_update_deg(&st,
                                    pbf_to_e7(blk, acc_lat, blk->pb->lat_offset) / 1e7,
                                    pbf_to_e7(blk, acc_lon, blk->pb->lon_offset) / 1e7);
                }
            }

            for (size_t i = 0; i < pg->n_ways; i++) {
                Way *w = pg->ways[i];
                if (!w) continue;
                st.ways++;
                st.way_refs += w->n_refs;
                if (w->n_refs > st.max_way_refs) st.max_way_refs = w->n_refs;
            }

            st.relations += pg->n_relations;
        }
        if (!saw_dense) st.blocks_without_dense++;

        if (verbose && st.data_blobs % 200 == 0) {
            fprintf(stderr, "  ...%llu blocks, %llu ways\r",
                    (unsigned long long)st.data_blobs,
                    (unsigned long long)st.ways);
        }
        pbf_block_free(blk);
    }

    if (rc == PBF_ERR) {
        fprintf(stderr, "读取失败: %s\n", pbf_stream_error(s));
        pbf_stream_close(s);
        return 1;
    }

    double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    uint64_t fsz = pbf_stream_file_size(s);
    pbf_stream_close(s);

    /* 链接到的 protobuf-c 运行时版本：头文件是 1.3.1 而库是系统那份 1.0.0 的话
       能编过、一跑就废，所以这条要打出来 */
    printf("protobuf_c     %s (编译期 %u)\n",
           protobuf_c_version(), PROTOBUF_C_VERSION_NUMBER);

    printf("file           %s\n", path);
    printf("file_bytes     %" PRIu64 "\n", fsz);
    printf("blobs          %" PRIu64 "  (header %" PRIu64 ", data %" PRIu64 ")\n",
           st.blobs, st.header_blobs, st.data_blobs);
    printf("nodes_dense    %" PRIu64 "\n", st.dense_nodes);
    printf("nodes_plain    %" PRIu64 "\n", st.plain_nodes);
    printf("ways           %" PRIu64 "\n", st.ways);
    printf("way_refs       %" PRIu64 "  (max per way %" PRIu64 ")\n",
           st.way_refs, st.max_way_refs);
    printf("relations      %" PRIu64 "\n", st.relations);
    printf("stringtable_B  %" PRIu64 "\n", st.string_bytes);
    printf("blocks_wo_dense %" PRIu64 "\n", st.blocks_without_dense);
    if (st.have_bbox) {
        printf("bbox_observed  %.7f %.7f %.7f %.7f\n",
               st.min_lat, st.min_lon, st.max_lat, st.max_lon);
    }
    printf("elapsed_sec    %.1f\n", secs);

    int fail = 0;
    if (check_ways && st.ways != expect_ways) {
        printf("ASSERT-FAIL    ways=%" PRIu64 " expected=%" PRIu64 "\n",
               st.ways, expect_ways);
        fail = 1;
    }
    if (st.data_blobs == 0) { printf("ASSERT-FAIL    没有任何 OSMData 块\n"); fail = 1; }
    if (st.ways == 0)       { printf("ASSERT-FAIL    没有任何 way\n"); fail = 1; }
    if (fail) return 1;

    printf("ASSERT-OK\n");
    return 0;
}
