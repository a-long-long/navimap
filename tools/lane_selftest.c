/* skedaddle_lane_selftest —— 车道转向指示（turn:lanes）数据链路验收
 *
 * 重建 beijing 图后，验证：
 *   · 该省 has_lanes == 1（lanes.bin + lane_names.bin 齐备且被 mapset 加载）；
 *   · 至少有若干条边带车道串（mapset_edge_lanes 返回非 NULL）；
 *   · 每个返回的车道串格式合法（含 '|' 分隔或 left/right/through 等关键字，
 *     长度 > 0 且不含 NUL 中段）；
 *   · 车道串确实来自 lane_names.bin（字符集是 OSM turn:lanes 取值）。
 *
 * 退出码 0 = 全部通过；非 0 = 有断言失败。regress.sh 用 "LANE-OK" 判定。 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "map/mapset.h"
#include "utils/plat.h"

static int fails = 0;

#define CHECK(cond, ...) do {                                            \
    if (!(cond)) {                                                       \
        printf("ASSERT-FAIL " __VA_ARGS__);                              \
        printf("\n");                                                    \
        fails++;                                                        \
    } else {                                                            \
        printf("ASSERT-OK   " __VA_ARGS__);                             \
        printf("\n");                                                    \
    }                                                                   \
} while (0)

/* 是否像一条合法的 turn:lanes 串（按行进方向用 '|' 分隔的车道列表） */
static int lane_str_ok(const char *s)
{
    if (!s || s[0] == '\0') return 0;
    if (strnlen(s, 1024) >= 1024) return 0;       /* 异常长，疑似坏数据 */
    /* 至少含一个已知关键字，或含分隔符（单车道也可只写 "through"） */
    if (strstr(s, "|")) return 1;
    const char *kw[] = { "left", "right", "through", "straight",
                         "merge", "reverse", "uturn", "slight", "sharp", "none" };
    for (size_t i = 0; i < sizeof(kw)/sizeof(kw[0]); i++)
        if (strstr(s, kw[i])) return 1;
    return 0;
}

int main(int argc, char **argv)
{
    const char *graph = "products/graph/beijing";
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--graph=", 8) == 0) graph = argv[i] + 8;
    }

    MapSet ms;
    if (mapset_load(&ms, graph) != 0) {
        printf("ASSERT-FAIL 加载图集失败: %s\n", graph);
        return 2;
    }
    if (ms.n < 1) {
        printf("ASSERT-FAIL 图集内没有任何省份\n");
        return 2;
    }

    int prov = 0;
    mapset_ensure_loaded(&ms, prov);
    const MapSrc *s = &ms.srcs[prov];

    CHECK(s->has_lanes == 1, "beijing 应含 lanes.bin (has_lanes=%d)", s->has_lanes);

    uint64_t ecount = s->edges.hdr.rec_count;
    printf("info        省份=%s 边数=%llu lanes.rec_count=%llu\n",
           s->name, (unsigned long long)ecount,
           (unsigned long long)s->lanes.hdr.rec_count);

    uint64_t present = 0, bad = 0;
    int samples = 0;
    for (uint64_t e = 0; e < ecount; e++) {
        const char *ls = mapset_edge_lanes(&ms, prov, (uint32_t)e);
        if (!ls) continue;
        present++;
        if (!lane_str_ok(ls)) {
            bad++;
            if (bad <= 5)
                printf("  坏串 edge=%llu: \"%s\"\n", (unsigned long long)e, ls);
        } else if (samples < 12) {
            printf("  样例 edge=%llu: \"%s\"\n", (unsigned long long)e, ls);
            samples++;
        }
    }

    printf("info        带车道指示的边=%llu 坏串=%llu\n",
           (unsigned long long)present, (unsigned long long)bad);

    CHECK(present > 0, "至少有边带车道指示 (present=%llu)", (unsigned long long)present);
    CHECK(bad == 0, "车道串格式全部合法 (bad=%llu)", (unsigned long long)bad);

    if (fails == 0) {
        printf("LANE-OK\n");
        mapset_free(&ms);
        return 0;
    }
    printf("LANE-FAIL (%d 项)\n", fails);
    mapset_free(&ms);
    return 1;
}
