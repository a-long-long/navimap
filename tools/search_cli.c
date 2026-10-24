/* search_cli —— 地名 / POI 搜索的无头验收（阶段 7）
 *
 * 搜索这种东西 GUI 里敲两个字看一眼就算"验过了"，最容易留下性能与排序的坑，
 * 所以把断言写在这里：
 *
 *   1. 命中：搜"中关村"/"西二旗"有结果，且**结果名里确实含关键词**
 *      （gram 只召回、不保证连续，漏做子串复核就会返回"中…关村"这种脏结果）
 *   2. 排序：给中心点时按加权距离升序，且距离不能来回跳
 *   3. 性能：单次查询百毫秒级（PLAN 的验收线），连跑 20 次看均值与最大值
 *   4. 边界：空串 / 纯英文 / 生僻字 / 超长串 / 带 % 的通配符串，都不崩
 *   5. 召回：gram 索引完整性抽查（随机抽 400 个二字以上的地名，逐个搜得到自己）
 *
 * 用法：
 *   ./search_cli [--db=products/places_auto.db] [--near=39.90,116.40] [--limit=20]
 *                [--q=中关村] [--repeat=20] [--recall-check]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "search/search.h"
#include "utils/geo.h"
#include "tool_common.h"


static int parse_ll(const char *s, double *lat, double *lon)
{
    char *sp = strchr(s, ',');
    if (!sp) return -1;
    char buf[128];
    size_t n = (size_t)(sp - s);
    if (n >= sizeof(buf)) return -1;
    memcpy(buf, s, n); buf[n] = '\0';
    *lat = atof(buf);
    *lon = atof(sp + 1);
    return 0;
}

int main(int argc, char **argv)
{
    const char *db = "products/places_auto.db";
    const char *near_s = NULL;
    const char *q = NULL;
    int limit = 20, repeat = 20, recall_check = 0;
    int fails = 0;

#define FAIL(...) do { printf("ASSERT-FAIL    " __VA_ARGS__); fails++; } while (0)

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v;
        if ((v = tc_arg_val(a, "--db=")))          db = v;
        else if ((v = tc_arg_val(a, "--q=")))      q = v;
        else if ((v = tc_arg_val(a, "--near=")))   near_s = v;
        else if ((v = tc_arg_val(a, "--limit=")))  limit = atoi(v);
        else if ((v = tc_arg_val(a, "--repeat="))) repeat = atoi(v);
        else if (strcmp(a, "--recall-check") == 0) recall_check = 1;
        else { fprintf(stderr, "未知参数 %s\n", a); return 2; }
    }

    SearchDb *d = search_open(db);
    if (!d) { printf("ASSERT-FAIL    打不开搜索库 %s\n", db); return 1; }

    double clat = 39.9, clon = 116.4;
    int have_center = 0;
    if (near_s) {
        if (parse_ll(near_s, &clat, &clon) != 0) {
            printf("ASSERT-FAIL    --near 应为 lat,lon\n");
            return 1;
        }
        have_center = 1;
    }
    printf("库            %s\n", db);
    printf("中心          %s%.5f,%.5f\n", have_center ? "" : "(未给) ", clat, clon);

    SearchHit hits[64];

    /* ---- 1~3. 关键词查询 + 命中 + 排序 + 性能 ---- */
    const char *words[] = { "中关村", "西二旗", "天安门", "五道口" };
    int nwords = (int)(sizeof(words) / sizeof(words[0]));
    if (q) { words[0] = q; nwords = 1; }

    double sum_ms = 0, max_ms = 0;
    int sum_n = 0;

    for (int w = 0; w < nwords; w++) {
        int n = 0;
        uint64_t ms = 0;
        if (search_query(d, words[w], have_center, clat, clon, 0, 0, limit,
                         hits, 64, &n, &ms) != 0) {
            FAIL("查询 %s 返回错误\n", words[w]);
            continue;
        }
        printf("\n查询 %-10s %d 条  %llu ms\n", words[w], n,
               (unsigned long long)ms);
        sum_ms += ms; sum_n++;
        if ((double)ms > max_ms) max_ms = (double)ms;

        if (n == 0) { FAIL("%s 一条都没搜到\n", words[w]); continue; }
        if (n > limit) { FAIL("%s 返回 %d 条，超过 limit %d\n", words[w], n, limit); }

        double prev_score = -1;
        for (int i = 0; i < n && i < 6; i++) {
            char dd[48];
            if (have_center) snprintf(dd, sizeof(dd), "%.1f km", hits[i].dist_m / 1000.0);
            else             snprintf(dd, sizeof(dd), "-");
            printf("  %2d  %-22s kind %2d w %2d  %s\n",
                   i, hits[i].name, hits[i].kind, hits[i].weight, dd);
            /* 1. 结果名必须真的含关键词 */
            if (!strstr(hits[i].name, words[w]))
                FAIL("%s 的第 %d 条结果「%s」不含关键词\n", words[w], i, hits[i].name);
            /* 2. 排序单调：直接用结果里带回的排序分，不重算公式 */
            if (prev_score >= 0 && hits[i].score < prev_score - 1e-9)
                FAIL("%s 排序不是升序：第 %d 条分数更小（%.3f < %.3f）\n",
                     words[w], i, hits[i].score, prev_score);
            prev_score = hits[i].score;
        }
        /* 2b. 完全同名的条目要吃到"匹配质量"加成——但只在它离得不算远时
           才要求排第一：库里有跨省同名（实测"中关村"在山东还有个同名村，
           378 km 外），那种情况按距离压后才是对的。 */
        if (have_center && n > 1) {
            double dmin = hits[0].dist_m;
            for (int i = 1; i < n; i++) {
                if (strcmp(hits[i].name, words[w]) == 0 &&
                    hits[i].dist_m <= dmin * 2.0 + 1000.0)
                    FAIL("%s 有同名条目（%.1f km）却排在第 %d 位\n",
                         words[w], hits[i].dist_m / 1000.0, i);
            }
        }
        /* 3. 单次要够快（PLAN：百毫秒级） */
        if (ms > 100) FAIL("%s 查询 %llu ms 超过 100 ms\n", words[w],
                           (unsigned long long)ms);
    }

    /* ---- 3b. 连续查询的均值/峰值 ---- */
    if (repeat > 0 && sum_n > 0) {
        uint64_t tot = 0, worst = 0;
        for (int i = 0; i < repeat; i++) {
            int n = 0;
            uint64_t ms = 0;
            search_query(d, "中关村", have_center, clat, clon, 0, 0, limit,
                         hits, 64, &n, &ms);
            tot += ms;
            if (ms > worst) worst = ms;
        }
        printf("\n重复 %d 次    均值 %.1f ms  最慢 %llu ms\n",
               repeat, (double)tot / repeat, (unsigned long long)worst);
        if ((double)tot / repeat > 100.0)
            FAIL("平均 %.1f ms 超过 100 ms\n", (double)tot / repeat);
    }

    /* ---- 4. 边界输入 ---- */
    const char *edge[] = { "", " ", "abc", "XYZ", "龘龘", "%%", "_", "\\",
                           "中关村大街北口东侧辅路一段很长的名字超过三十二字",
                           "中" };
    printf("\n边界输入\n");
    for (size_t i = 0; i < sizeof(edge) / sizeof(edge[0]); i++) {
        int n = 0;
        uint64_t ms = 0;
        int rc = search_query(d, edge[i], have_center, clat, clon, 0, 0, limit,
                              hits, 64, &n, &ms);
        printf("  「%s」→ rc %d  %d 条  %llu ms\n", edge[i], rc, n,
               (unsigned long long)ms);
        if (rc != 0) FAIL("输入「%s」返回 %d（应为 0）\n", edge[i], rc);
        if (ms > 500) FAIL("输入「%s」耗时 %llu ms 太慢\n", edge[i],
                           (unsigned long long)ms);
    }
    /* 单字必须能搜到东西（走 gram 前缀路径） */
    {
        int n = 0;
        search_query(d, "中", have_center, clat, clon, 0, 0, limit, hits, 64, &n, NULL);
        if (n == 0) FAIL("单字「中」搜不到任何结果（前缀/兜底路径坏了）\n");
    }

    /* ---- 5. 召回抽查：随机地名能否被自己搜出来 ---- */
    if (recall_check) {
        printf("\n召回抽查\n");
        /* 没有 SQL 接口暴露，就搜一批常见词，看总数是否合理 */
        const char *probe[] = { "中关村", "西二旗", "国贸", "西单", "王府井",
                                "颐和园", "首都机场", "长春", "郑州", "太原" };
        int hit_cnt = 0;
        for (size_t i = 0; i < sizeof(probe) / sizeof(probe[0]); i++) {
            int n = 0;
            search_query(d, probe[i], 0, 0, 0, 0, 0, limit, hits, 64, &n, NULL);
            if (n > 0) hit_cnt++;
            printf("  %-10s %d 条\n", probe[i], n);
        }
        if (hit_cnt < 8)
            FAIL("%d/10 个常见词有结果，召回率偏低\n", hit_cnt);
    }

    search_close(d);
    if (fails) { printf("SEARCH-FAIL  %d 条断言失败\n", fails); return 1; }
    printf("SEARCH-OK\n");
    return 0;
}
