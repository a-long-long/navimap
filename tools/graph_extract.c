/* graph_extract —— PBF → 自描述路网二进制
 *
 * 用法：
 *   ./graph_extract --out=products/graph/beijing sources/beijing.osm.pbf
 *   ./graph_extract --fmt-version          # 只打印当前 NMBIN 版本号后退出
 *
 * 产出（全部带 NAVMBIN 自描述头，字段名/偏移/类型写在文件里）：
 *   nodes.bin  edges.bin  osmid.bin  names.bin  restrictions.bin  manifest.txt
 *
 * 退出码非 0 = 有断言失败（闸门见下）。
 *
 * manifest.txt 是"这份产物由哪个源文件、多大的文件、什么时候生成的"指纹，
 * scripts/sync_maps.sh 靠它判断要不要重转；并且★只在全部闸门通过后才写，
 * 所以"有 manifest"等价于"这份产物可用"。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>

#include "map/graph_build.h"
#include "route/revidx.h"
#include "map/binfmt.h"
#include "pbf/pbf_stream.h"
#include "utils/plat.h"

int main(int argc, char **argv)
{
    /* 给同步脚本用的探针：打印当前二进制格式版本。格式一升级，
       所有旧产物的 manifest 版本号就对不上，必须重转——否则加载端
       （app_init 只比对 magic，不校验版本）会静默画错图。 */
    if (argc == 2 && strcmp(argv[1], "--fmt-version") == 0) {
        printf("%d\n", NMBIN_VERSION);
        return 0;
    }

    const char *out_dir = NULL, *src = NULL;
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--out=", 6) == 0) out_dir = argv[i] + 6;
        else if (argv[i][0] == '-') { fprintf(stderr, "未知参数 %s\n", argv[i]); return 2; }
        else src = argv[i];
    }
    if (!src || !out_dir) {
        fprintf(stderr, "用法: %s --out=<目录> <file.osm.pbf>\n", argv[0]);
        return 2;
    }
    nm_mkdir_p(out_dir);

    /* 源文件指纹：同步脚本靠 size+mtime 判断产物是否过期 */
    uint64_t src_bytes = 0;
    int64_t  src_mtime = 0;
    nm_stat_file(src, &src_bytes, &src_mtime);

    GraphBuildStats st;
    memset(&st, 0, sizeof(st));
    if (graph_build(src, out_dir, &st) != 0) {
        fprintf(stderr, "graph_build 失败\n");
        return 1;
    }

    printf("source         %s\n", src);
    printf("ways_total     %" PRIu64 "\n", st.ways_total);
    printf("ways_kept      %" PRIu64 "\n", st.ways_kept);
    printf("ways_skip      %" PRIu64 "  (no highway %" PRIu64 ", too short %" PRIu64 ")\n",
           st.ways_skip, st.ways_no_hw, st.ways_too_short);
    printf("nodes          %" PRIu64 "\n", st.nodes);
    printf("edges          %" PRIu64 "\n", st.edges);
    printf("lines          %" PRIu64 "  (步行道/铁路/水系线，不进路由)\n", st.lines_kept);
    printf("areas          %" PRIu64 "  (不闭合跳过 %" PRIu64 ", 节点缺失丢弃 %" PRIu64 ")\n",
           st.areas_kept, st.areas_skip_open, st.linearea_dropped_missing);
    printf("dropped_missing_node %" PRIu64 "  (无坐标节点剔除 %" PRIu64 ")\n",
           st.dropped_missing_node, st.dead_nodes);
    printf("names          %" PRIu64 "\n", st.names);
    printf("lanes_ways     %" PRIu64 "  (含 turn:lanes 的 way，写入车道指示)\n", st.ways_with_lanes);
    printf("lane_strings   %" PRIu64 "  (去重后的车道转向串)\n", st.lane_strings);
    printf("rt_unknown_ways %" PRIu64 "  (白名单内 %" PRIu64 ", 闸门 rt_unknown_drivable=%" PRIu64 ")\n",
           st.rt_unknown_ways, st.rt_unknown_nondrivable, st.rt_unknown_drivable);
    printf("restrictions   written %" PRIu64 " / scanned %" PRIu64
           "  (via=way 展开 %" PRIu64 ", via=way 落空 %" PRIu64
           ", 成员不全 %" PRIu64 ", 定位失败 %" PRIu64 ")\n",
           st.restr_written, st.restr_scanned, st.restr_via_way_written,
           st.restr_skip_via_way, st.restr_skip_incomplete, st.restr_skip_unresolved);
    printf("bbox_e7        %d %d %d %d\n", st.bbox[0], st.bbox[1], st.bbox[2], st.bbox[3]);
    printf("tiles          %" PRIu64 "\n", st.tiles_written);
    printf("disp_tiles     %" PRIu64 "  (显示层分 tile，运行时只加载视口内)\n", st.disp_tiles);
    printf("elapsed_sec    %.1f\n", st.elapsed_sec);

    /* ---- 闸门 ---- */
    int fail = 0;
    if (st.rt_unknown_drivable != 0) {
        printf("ASSERT-FAIL    rt_unknown_drivable=%" PRIu64
               "（有可通行道路的 highway 取值不在表里，必须补表）\n", st.rt_unknown_drivable);
        fail = 1;
    }
    if (st.nodes == 0 || st.edges == 0) { printf("ASSERT-FAIL    空图\n"); fail = 1; }
    if (st.ways_kept + st.ways_skip + st.lines_kept + st.areas_kept +
        st.areas_skip_open != st.ways_total) {
        printf("ASSERT-FAIL    ways_kept+skip+lines+areas != total\n"); fail = 1;
    }

    /* ★ 闸门没过就不写 manifest：让"有 manifest"严格等价于"产物可用"。
       以前是先写 manifest 再判 fail，结果同步脚本会把失败产物当成已转换。 */
    if (fail) {
        fprintf(stderr, "ASSERT-FAIL 详见上面的 ASSERT-FAIL 行（未写 manifest，产物不可用）\n");
        return 1;
    }

    /* ---- manifest ---- */
    char mp[4096];
    snprintf(mp, sizeof(mp), "%s/manifest.txt", out_dir);
    FILE *f = fopen(mp, "wb");
    if (f) {
        fprintf(f, "# NavMap 路网抽取结果（自描述二进制 NAVMBIN v%d）\n", NMBIN_VERSION);
        fprintf(f, "source         %s\n", src);
        fprintf(f, "src_bytes      %" PRIu64 "\n", src_bytes);
        fprintf(f, "src_mtime      %" PRId64 "\n", src_mtime);
        fprintf(f, "generated_at   %" PRId64 "\n", (int64_t)time(NULL));
        fprintf(f, "binfmt_version %d\n", NMBIN_VERSION);
        fprintf(f, "ways_total     %" PRIu64 "\n", st.ways_total);
        fprintf(f, "ways_kept      %" PRIu64 "\n", st.ways_kept);
        fprintf(f, "ways_skip      %" PRIu64 "\n", st.ways_skip);
        fprintf(f, "nodes          %" PRIu64 "\n", st.nodes);
        fprintf(f, "edges          %" PRIu64 "\n", st.edges);
        fprintf(f, "lines          %" PRIu64 "\n", st.lines_kept);
        fprintf(f, "areas          %" PRIu64 "   # 不闭合 %" PRIu64 " 缺失丢弃 %" PRIu64 "\n",
                st.areas_kept, st.areas_skip_open, st.linearea_dropped_missing);
        fprintf(f, "dropped_missing_node %" PRIu64 "\n", st.dropped_missing_node);
        fprintf(f, "dead_nodes       %" PRIu64 "   # 无坐标被剔除\n", st.dead_nodes);
        fprintf(f, "names          %" PRIu64 "\n", st.names);
        fprintf(f, "rt_unknown_ways %" PRIu64 "\n", st.rt_unknown_ways);
        fprintf(f, "rt_unknown_nondrivable %" PRIu64 "\n", st.rt_unknown_nondrivable);
        fprintf(f, "rt_unknown_drivable %" PRIu64 "   # ★ 闸门：必须为 0\n", st.rt_unknown_drivable);
        fprintf(f, "restrictions   %" PRIu64 "   # 扫描 %" PRIu64
                "（via=way 展开 %" PRIu64 " / via=way 落空 %" PRIu64
                " / 成员不全 %" PRIu64 " / 定位失败 %" PRIu64 "）\n",
                st.restr_written, st.restr_scanned, st.restr_via_way_written,
                st.restr_skip_via_way, st.restr_skip_incomplete,
                st.restr_skip_unresolved);
        fprintf(f, "bbox_e7        %d %d %d %d\n", st.bbox[0], st.bbox[1], st.bbox[2], st.bbox[3]);
        fprintf(f, "disp_tiles     %" PRIu64 "\n", st.disp_tiles);
        fprintf(f, "elapsed_sec    %.1f\n", st.elapsed_sec);
        fclose(f);
    } else {
        fprintf(stderr, "警告: manifest.txt 写不出来（%s），同步脚本会认为这里没转换过\n", mp);
    }

    /* ---- 派生索引 rev.bin（反向入边）----
       它 100% 由 nodes/edges 决定，抽取时顺手写一份，运行时就不用每次算路
       现建（hebei 实测 323 ms，占短途端到端 401 ms 的 80%）。
       ★ 写失败不算闸门失败：运行时读到没有/坏的 rev.bin 会回退到现场构建，
         结果一定正确，只是慢。不能让"派生文件没写成"毁掉一整次抽取。 */
    {
        NMBinFile nf, ef;
        char p[4096];
        int ok = 0;
        snprintf(p, sizeof p, "%s/nodes.bin", out_dir);
        if (nmbin_open(p, &nf) == 0) {
            snprintf(p, sizeof p, "%s/edges.bin", out_dir);
            if (nmbin_open(p, &ef) == 0) {
                ok = (rev_write(out_dir, &nf, &ef) == 0);
                nmbin_close(&ef);
            }
            nmbin_close(&nf);
        }
        printf("rev_bin        %s\n", ok ? "written" : "skipped（运行时会现场构建）");
    }

    printf("ASSERT-OK\n");
    return 0;
}
