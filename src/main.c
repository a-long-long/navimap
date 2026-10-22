#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui/window.h"

/* navimap —— 离线地图窗口
 * 用法：./navimap [--graph=products/graph/beijing] [--w=1280] [--h=800]
 *                 [--font=<路径.ttf|none>] [--font-size=14]
 *                 [--no-autobuild] [--sources=<目录>]
 * 字体不指定时用默认（宋体优先的候选链）；--font=none 关闭文字标注。
 * 启动时会自检：源 pbf 在而产物没了/坏了会自动重建（--no-autobuild 可关）。
 * 交互：左键拖动平移，滚轮缩放，右下角 +/- 按钮缩放，0-4 切 LOD，
 *       [ ] 调字号（8~32pt），左键点两下选起终点算路线，
 *       g 开始/停止模拟导航，t 视口跟随开关，n 注入一次偏航（自动重算），
 *       . / , 调模拟倍速，c 清除，f 缩放到整条路线，p 开关面板，Esc/q 退出。
 */
int main(int argc, char **argv)
{
    const char *graph = "products/graph/beijing";
    int w = 1280, h = 800;
    const char *font = NULL;    /* NULL = 默认字体 */
    int font_size = 0;          /* <=0 = 用默认字号 */
    const char *q = NULL;       /* 启动时搜一次并跳到第一条结果 */

    /* 启动自愈：源 pbf 在、产物没了/坏了就当场重建（判据同 sync_maps.sh）。
       默认开；--no-autobuild 关；--sources= 指定源目录（自动探测不到时用）。 */
    AutoBuildOpt ab;
    memset(&ab, 0, sizeof(ab));
    ab.enable = 1;

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--graph=", 8) == 0) graph = argv[i] + 8;
        /* 同 map_shot：n 必须是字面量长度，写 5 会把 '\0' 比进去，永远不相等 */
        else if (strncmp(argv[i], "--w=", 4) == 0) w = atoi(argv[i] + 4);
        else if (strncmp(argv[i], "--h=", 4) == 0) h = atoi(argv[i] + 4);
        else if (strncmp(argv[i], "--font=", 7) == 0) font = argv[i] + 7;
        else if (strncmp(argv[i], "--font-size=", 12) == 0)
            font_size = atoi(argv[i] + 12);
        else if (strncmp(argv[i], "--q=", 4) == 0) q = argv[i] + 4;
        else if (strcmp(argv[i], "--no-autobuild") == 0) ab.enable = 0;
        else if (strncmp(argv[i], "--sources=", 10) == 0) ab.src_dir = argv[i] + 10;
        else { fprintf(stderr, "未知参数 %s\n", argv[i]); return 2; }
    }

    App a;
    if (app_init(&a, graph, w, h, font, font_size, &ab) != 0) return 1;
    if (q) {
        int n = app_search_query(&a, q);
        printf("搜索「%s」%d 条（%llu ms）\n", q, n,
               (unsigned long long)a.su.elapsed_ms);
        for (int i = 0; i < n && i < 5; i++)
            printf("  %d. %s  %.1f km\n", i, a.su.hits[i].name,
                   a.su.hits[i].dist_m / 1000.0);
        if (n > 0) {
            a.view.lat = a.su.hits[0].lat;
            a.view.lon = a.su.hits[0].lon;
            if (a.view.zoom < 14.0) a.view.zoom = 15.0;
        }
    }
    uint64_t tn, te;
    mapset_totals(&a.ms, &tn, &te);
    printf("图: %s  %d 省  节点 %llu  边 %llu\n", graph, a.ms.n,
           (unsigned long long)tn, (unsigned long long)te);
    printf("字体: %s  %d pt\n",
           a.font_used[0] ? a.font_used : "(无)", a.font_size);
    app_run(&a);
    app_shutdown(&a);
    return 0;
}
