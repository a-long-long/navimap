#ifndef NAVIMAP_SEARCH_H
#define NAVIMAP_SEARCH_H

#include <stdint.h>

/* ---------------------------------------------------------------------------
 * 地名 / POI 搜索（阶段 7）
 *
 * 直接用现成的 places_auto.db（226k 地名）：
 *   place(id, name, lat, lon, kind, weight)   —— 主表
 *   gram(gram, place_id)                      —— 二元文法（bigram）倒排索引
 * arch.md 的要求：SQLite 只当**索引**用，路网主存储仍然是 nmbin 文件。
 *
 * 为什么走 gram 而不是 LIKE '%关键词%'：
 *   place 表只有 id 上的主键，没有 name 索引，'%xx%' 只能全表扫 22 万行
 *   （实测数十毫秒级，且输入过程中每敲一个字都要扫一遍）。gram 是
 *   WITHOUT ROWID 的 (gram, place_id) 主键，等值查询直接走索引。
 *
 * 查询流程：
 *   1. 关键词切 bigram（按**字符**切，不是字节——"中关村" → 中关/关村）
 *   2. gram 表取"同时含有全部 bigram"的 place_id（交集）
 *   3. 按 id 逐条取详情（主键单行查询，微秒级）
 *   4. ★ 再用子串复核一遍：bigram 全命中不等于连续（"中关"+"关村" 也能
 *      命中"中关村"，但也可能命中"中…关村"这类不相邻的），gram 只是召回，
 *      精确性靠这一步
 *   5. 按距离（给了中心点）或 weight 排序，取 top-K
 *
 * 单字查询没有 bigram 可切 → 走 gram 前缀（gram LIKE 'X%'，仍走索引），
 * 前缀也没结果时退化到全表 LIKE（保证不漏，代价是慢一点，只在单字时发生）。
 * ------------------------------------------------------------------------- */

typedef struct {
    char   name[64];
    double lat, lon;
    int    kind;        /* 与标注层同语义：数值小 = 行政级别高 */
    int    weight;
    double dist_m;      /* 到排序中心的距离（米）；无中心时为 -1 */
    double score;       /* 排序分（越小越靠前），把"距离/权重/匹配质量"合成一个数。
                         * 留着它是为了能断言排序：CLI 里直接检查它单调即可，
                         * 不用把排序公式再抄一遍。 */
} SearchHit;

typedef struct SearchDb SearchDb;

/* 只读打开。失败返回 NULL（库文件不存在/不是 sqlite）。 */
SearchDb *search_open(const char *db_path);
void      search_close(SearchDb *d);

/* 查询。
 *   q          UTF-8 关键词（空串 → 直接返回 0 条，不报错）
 *   have_center 非 0 时按到 (clat,clon) 的距离排序
 *   max_dist_m  > 0 时丢弃更远的结果
 *   kind_max    > 0 时只保留 kind<=kind_max 的（0 = 不限）
 *   limit       > 0 为返回上限；<=0 时用默认 20（上限 64）
 * 返回 0 成功（*n_out 是结果条数），-1 参数/库问题。
 * elapsed_ms 可空，回填本次查询耗时。 */
int search_query(SearchDb *d, const char *q,
                 int have_center, double clat, double clon,
                 double max_dist_m, int kind_max, int limit,
                 SearchHit *out, int cap, int *n_out, uint64_t *elapsed_ms);

/* 关键词切 bigram 的可见入口（测试用）：返回 gram 个数，写到 grams[]（每个
   最多 16 字节）。chars 回填关键词的字符数。 */
int search_bigrams(const char *q, char grams[][16], int max_grams, int *chars);

#endif
