/* revbuild_cli —— 反向入边索引 rev.bin 的生成与自验收
 *
 * 用法:
 *   revbuild_cli --graph=<dir>                生成/补齐每个省的 rev.bin
 *   revbuild_cli --graph=<dir> --verify       落盘结果与内存构建逐元素对比
 *   revbuild_cli --graph=<dir> --verify --only=hebei
 *
 * ★ --verify 是这条优化唯一的正确性判据：写盘、读盘、内存构建三条路都走同一份
 *   rev_build_mem()，所以"落盘 == 内存构建"这个断言能同时抓住写坏和读坏
 *   （各写一份构建代码的话，比的是两份 bug，谁错都一样）。
 *   判据必须是双向的：验证过"相同"，还要能证明它"能失败"——把某个省的
 *   rev.bin 删掉或改坏，--verify 必须报 FAIL（regress.sh 里的 rev_neg 就是干这个）。
 *
 * 产物可由 sync_maps.sh 确定性重建（graph_extract 每转完一个省就写 rev.bin），
 * 本工具只是给"已经存在、抽取时还没这个功能"的图补一份。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "map/mapset.h"
#include "route/revidx.h"
#include "utils/plat.h"

static uint64_t now_ms(void)
{
    return nm_now_ms();                /* 平台层：clock_gettime / QPC */
}

int main(int argc, char **argv)
{
    const char *graph = NULL, *only = NULL;
    int verify = 0, quiet = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strncmp(a, "--graph=", 8))     graph = a + 8;
        else if (!strncmp(a, "--only=", 7)) only = a + 7;
        else if (!strcmp(a, "--verify"))    verify = 1;
        else if (!strcmp(a, "--quiet"))     quiet = 1;
        else { fprintf(stderr, "未知参数 %s\n", a); return 1; }
    }
    if (!graph) {
        fprintf(stderr, "用法: revbuild_cli --graph=<dir> [--verify] [--only=省名]\n");
        return 1;
    }

    MapSet ms;
    memset(&ms, 0, sizeof(ms));
    if (mapset_load(&ms, graph) != 0) {
        fprintf(stderr, "图集加载失败: %s\n", graph);
        return 1;
    }

    int nsave = 0, nskip = 0, nfail = 0, ncmp = 0;
    for (int i = 0; i < ms.n; i++) {
        const MapSrc *s = &ms.srcs[i];
        if (only && strcmp(s->name, only) != 0) continue;
        uint64_t n  = s->nodes.hdr.rec_count;
        uint64_t ne = s->edges.hdr.rec_count;
        if (n == 0 || ne == 0) continue;

        uint64_t t0 = now_ms();
        if (verify) {
            RevIdx disk, mem;
            memset(&disk, 0, sizeof(disk));
            memset(&mem, 0, sizeof(mem));
            if (rev_load(s->dir, &s->nodes, &s->edges, &disk) != 0) {
                printf("[FAIL ] %-12s 读不到 rev.bin（先跑一次不带 --verify 生成）\n",
                       s->name);
                nfail++;
                continue;
            }
            if (rev_build_mem(&s->nodes, &s->edges, s->name, &mem) != 0) {
                printf("[FAIL ] %-12s 内存构建失败\n", s->name);
                rev_free(&disk);
                nfail++;
                continue;
            }
            int eq = rev_equal(&disk, &mem, n);
            ncmp++;
            printf("%-6s %-12s 落盘 %llu 条 / 内存 %llu 条  %llu ms\n",
                   eq ? "[ OK ]" : "[FAIL ]", s->name,
                   (unsigned long long)disk.list_count,
                   (unsigned long long)mem.list_count,
                   (unsigned long long)(now_ms() - t0));
            if (!eq) nfail++;
            rev_free(&disk);
            rev_free(&mem);
        } else {
            if (rev_write(s->dir, &s->nodes, &s->edges) != 0) {
                printf("[FAIL ] %-12s 写 rev.bin 失败\n", s->name);
                nfail++;
                continue;
            }
            nsave++;
            if (!quiet)
                printf("[ OK ] %-12s rev.bin 已写入（%llu 条入边，%llu ms）\n",
                       s->name, (unsigned long long)ne,
                       (unsigned long long)(now_ms() - t0));
        }
        (void)nskip;
    }

    mapset_free(&ms);
    if (nfail) {
        printf("结果: %d 个失败\n", nfail);
        printf("%s\n", verify ? "REV-VERIFY-FAIL" : "REV-BUILD-FAIL");
        return 1;
    }
    printf("结果: %s %d 个省\n", verify ? "逐元素一致" : "已生成",
           verify ? ncmp : nsave);
    printf("%s\n", verify ? "REV-VERIFY-OK" : "REV-BUILD-OK");
    return 0;
}
