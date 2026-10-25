/* dmap_selftest：验证 PBF 显示层**后台异步**解码（不阻塞主线程）。
 *
 * 判据（数据驱动，不是"看着没卡"）：
 *   1) set_view 单次耗时远低于一省实际解码耗时（北京 ~1.6s）——证明解码在
 *      后台线程进行，主线程只做"派发 / 发布"，不进重活；
 *   2) 轮询期间观测到进度从 0 爬升到 100（pbfmap_decode 的进度回调生效）；
 *   3) 20s 内该省 has_features 置 1 且 lines 记录数 > 0（解码确实完成且产出正确）。
 *
 * 用法: dmap_selftest [<graph_dir> [<pbf_dir>]]
 *   默认 graph_dir=products/graph，pbf_dir=sources（需 sources/<省>.osm.pbf 在）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "map/mapset.h"
#include "utils/plat.h"   /* nm_now_ms / nm_sleep_ms */

int main(int argc, char **argv)
{
    const char *graph  = (argc > 1) ? argv[1] : "products/graph";
    const char *pbfdir = (argc > 2) ? argv[2] : "sources";

    MapSet ms;
    memset(&ms, 0, sizeof(ms));
    if (mapset_load(&ms, graph) != 0) {
        fprintf(stderr, "load fail: %s\n", graph);
        return 2;
    }
    if (ms.n == 0) { fprintf(stderr, "无省份\n"); return 2; }

    int prov = 0;
    for (int i = 0; i < ms.n; i++)
        if (strcmp(ms.srcs[i].name, "beijing") == 0) { prov = i; break; }
    MapSrc *s = &ms.srcs[prov];

    if (mapset_enable_pbf_display(&ms, pbfdir, 3) != 0) {
        fprintf(stderr, "enable_pbf_display 失败：%s 下需有 <省>.osm.pbf\n", pbfdir);
        return 2;
    }

    /* 放大视口边距，确保该省被判定为"视口内"并触发异步派发 */
    int32_t m  = (s->bbox[2] - s->bbox[0]) / 2;
    int32_t mn = (s->bbox[3] - s->bbox[1]) / 2;
    int32_t eb[4] = { s->bbox[0] - m, s->bbox[1] - mn,
                      s->bbox[2] + m, s->bbox[3] + mn };

    /* 第一次 set_view：派发后台解码任务 */
    mapset_set_view(&ms, eb);

    double   max_dt      = 0;
    int      saw_prog    = 0;
    int      reached_100 = 0;
    uint64_t t0 = nm_now_ms();
    int      done = 0;

    while (nm_now_ms() - t0 < 20000) {
        uint64_t ta = nm_now_ms();
        mapset_set_view(&ms, eb);                /* 主线程只派发/发布，不解码 */
        double dt = (double)(nm_now_ms() - ta);
        if (dt > max_dt) max_dt = dt;

        int p = mapset_dmap_progress(&ms, prov);
        if (p > 0 && p < 100) saw_prog = 1;
        if (p >= 100) reached_100 = 1;
        if (s->has_features) { done = 1; break; }
        nm_sleep_ms(2);     /* 让后台线程跑 */
    }
    if (s->has_features) done = 1;

    printf("dmap: prov=%s loaded=%d has_features=%d 进度爬升=%d 到100=%d "
           "max_set_view=%.1f ms\n",
           s->name, s->loaded, s->has_features, saw_prog, reached_100, max_dt);

    int bad = 0;
    if (max_dt > 250) {
        printf("ASSERT-FAIL  set_view 单次 %.1f ms 过长（应远小于 1.6s 解码耗时，证明未阻塞主线程）\n",
               max_dt);
        bad++;
    }
    if (!saw_prog) { printf("ASSERT-FAIL  未观测到进度爬升（pbfmap_decode 进度回调未生效？）\n"); bad++; }
    if (!done)     { printf("ASSERT-FAIL  20s 内未解码完成\n"); bad++; }
    if (s->has_features && s->lines.hdr.rec_count == 0) {
        printf("ASSERT-FAIL  解码完成但 lines 记录数为 0\n"); bad++;
    }

    printf(bad ? "DMAP-FAIL\n" : "DMAP-OK\n");
    return bad ? 1 : 0;
}
