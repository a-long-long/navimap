# 第 9 课：地名搜索

> 模块：`src/search/search.{c,h}`（直接吃现成 `places_auto.db`，SQLite + bigram 倒排）
> 前置：SQL `LIKE`、`GROUP BY ... HAVING COUNT` 交集查询、UTF-8 字符与字节的区别。

## 一、教学目的

理解用 **bigram 倒排索引**替代 `LIKE '%x%'` 全表扫描的经典索引设计，以及 precision/recall 的权衡。学完后学生应能：

- 解释"为什么 bigram + 子串复核"比纯 `LIKE` 快且准；
- 说明单字查询如何退化到前缀 + 全表扫描；
- 理解 UTF-8 必须按**字符**切 bigram（不能按字节）；
- 看懂 `SearchHit` 排序分的设计：把"距离 / 权重 / 匹配质量 / 长度惩罚"合成一个数以便断言。

`search.c` 只做三件容易写错的事（见头文件顶部注释）：bigram 按字符切、gram 只负责召回必须子串复核、`LIKE` 的 `%`/`_` 要转义。

## 二、功能构成

**`SearchHit`**（`src/search/search.h:32-41`）：结果结构体。`name[64]`、`lat/lon`、`kind`（数值小=行政级别高）、`weight`、`dist_m`（到排序中心距离，-1 无中心）、`score`（升序，越小越靠前）。

**关键接口**（`src/search/search.h:43-64`）：

```c
SearchDb *search_open(const char *db_path);                 /* 只读打开，失败返回 NULL */
void      search_close(SearchDb *d);
int search_query(SearchDb *d, const char *q,
                 int have_center, double clat, double clon,
                 double max_dist_m, int kind_max, int limit,
                 SearchHit *out, int cap, int *n_out, uint64_t *elapsed_ms);
int search_bigrams(const char *q, char grams[][16], int max_grams, int *chars); /* 切词可见入口（测试用） */
```

**`search.c` 内部函数一览**（签名 + 一行说明 + 行号；`search_bigrams` / `search_open` / `search_query` 的完整实现见下方 §2~§4，其余内部函数在 §2.5 逐一给出逐字完整实现）：

| 函数 | 位置 | 说明 |
|---|---|---|
| `u8_next` | `search.c:33-42` | 按 UTF-8 首字节判 1/2/3/4 字节，返回下一字符起点（非法字节按 1 字节推进，不死循环） |
| `search_bigrams` | `search.c:44-73` | 关键词按**字符**切 bigram，去重相邻重复 gram |
| `like_literal` | `search.c:76-88` | 把 `\` `%` `_` 转义成字面量，防用户输入通配符 |
| `ids_push` | `search.c:94-106` | 候选 id 动态数组 push，到 `CAND_MAX` 上限丢弃 |
| `ids_by_grams` | `search.c:109-131` | 交集查询：含全部 gram 的 `place_id`（`GRAM IN (...) GROUP BY ... HAVING COUNT(DISTINCT gram)=ng`） |
| `ids_by_prefix` | `search.c:134-157` | 单字回退：gram 前缀 `LIKE 'X%'`（仍走索引），同 place 去重 |
| `ids_by_scan` | `search.c:160-178` | 兜底：全表 `name LIKE '%X%'`，只在前面都没结果时触发 |
| `cmp_cand` | `search.c:192-199` | 排序键比较（score 升序；相同则按 weight 降序、name） |
| `contains_ci` | `search.c:203-219` | 大小写不敏感子串复核（只对 ASCII 生效，UTF-8 多字节不动） |
| `search_open` | `search.c:221-235` | 以 `SQLITE_OPEN_READONLY` 打开，预编译取详情语句 |
| `search_query` | `search.c:245-330` | 查询主流程（五步，见下） |

### 2.5 其余内部函数（完整实现）

> 以下函数按 `src/search/search.c` 真实行号逐字摘录，与源码可编译一致（仅去除文件头 include / 宏定义等外围，函数体一字未改）。

**`u8_next`** —— UTF-8 字符步进（返回下一字符起点，非法字节按 1 字节推进避免死循环）：`src/search/search.c:33-42`

```c
static const char *u8_next(const char *p, const char *end)
{
    unsigned char c = (unsigned char)*p;
    int n = 1;
    if      (c >= 0xF0) n = 4;
    else if (c >= 0xE0) n = 3;
    else if (c >= 0xC0) n = 2;
    const char *q = p + n;
    return q > end ? end : q;
}
```

**`like_literal`** —— 把 `\` `%` `_` 转义成字面量，防用户输入通配符：`src/search/search.c:76-88`

```c
static void like_literal(const char *s, char *out, size_t n)
{
    size_t o = 0;
    for (size_t i = 0; s[i] && o + 3 < n; i++) {
        if (s[i] == '\\' || s[i] == '%' || s[i] == '_') {
            out[o++] = '\\';
            out[o++] = s[i];
        } else {
            out[o++] = s[i];
        }
    }
    out[o] = '\0';
}
```

**`ids_push`** —— 候选 id 动态数组 push，到 `CAND_MAX` 上限丢弃：`src/search/search.c:94-106`

```c
static int ids_push(IdList *l, uint32_t id)
{
    if (l->n >= l->cap) {
        int nc = l->cap ? l->cap * 2 : 512;
        if (nc > CAND_MAX) nc = CAND_MAX;
        if (l->n >= nc) return 0;              /* 到上限就丢弃（后面还有更好的？没有，但保住了耗时） */
        uint32_t *nv = realloc(l->v, (size_t)nc * sizeof(uint32_t));
        if (!nv) return -1;
        l->v = nv; l->cap = nc;
    }
    l->v[l->n++] = id;
    return 0;
}
```

**`ids_by_grams`** —— 交集查询：含全部 gram 的 `place_id`：`src/search/search.c:109-131`

```c
static int ids_by_grams(SearchDb *d, char grams[][GRAM_BYTES], int ng, IdList *out)
{
    char sql[1536];
    int pos = snprintf(sql, sizeof(sql),
                       "SELECT place_id FROM gram WHERE gram IN (");
    for (int i = 0; i < ng; i++)
        pos += snprintf(sql + pos, sizeof(sql) - (size_t)pos, "%s?", i ? "," : "");
    pos += snprintf(sql + pos, sizeof(sql) - (size_t)pos,
                    ") GROUP BY place_id HAVING COUNT(DISTINCT gram)=%d", ng);
    if (pos < 0 || (size_t)pos >= sizeof(sql)) return -1;

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(d->db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    for (int i = 0; i < ng; i++)
        sqlite3_bind_text(st, i + 1, grams[i], -1, SQLITE_TRANSIENT);

    while (sqlite3_step(st) == SQLITE_ROW) {
        if (ids_push(out, (uint32_t)sqlite3_column_int64(st, 0)) != 0) break;
        if (out->n >= CAND_MAX) break;
    }
    sqlite3_finalize(st);
    return 0;
}
```

**`ids_by_prefix`** —— 单字回退：gram 前缀 `LIKE 'X%'`（仍走索引），同 place 去重：`src/search/search.c:134-157`

```c
static int ids_by_prefix(SearchDb *d, const char *q, IdList *out)
{
    char pat[GRAM_BYTES + 8];
    char lit[GRAM_BYTES];
    like_literal(q, lit, sizeof(lit));
    snprintf(pat, sizeof(pat), "%s%%", lit);

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(d->db,
            "SELECT place_id FROM gram WHERE gram LIKE ?1 ESCAPE '\\' LIMIT 4000",
            -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(st, 1, pat, -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW) {
        uint32_t id = (uint32_t)sqlite3_column_int64(st, 0);
        /* 同一个 place 可能有多个以该字开头的 gram，去重 */
        int dup = 0;
        for (int i = 0; i < out->n; i++) if (out->v[i] == id) { dup = 1; break; }
        if (dup) continue;
        if (ids_push(out, id) != 0) break;
        if (out->n >= CAND_MAX) break;
    }
    sqlite3_finalize(st);
    return 0;
}
```

**`ids_by_scan`** —— 兜底：全表 `name LIKE '%X%'`，只在前面都没结果时触发：`src/search/search.c:160-178`

```c
static int ids_by_scan(SearchDb *d, const char *q, IdList *out)
{
    char pat[160];
    char lit[128];
    like_literal(q, lit, sizeof(lit));
    snprintf(pat, sizeof(pat), "%%%s%%", lit);

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(d->db,
            "SELECT id FROM place WHERE name LIKE ?1 ESCAPE '\\' LIMIT 2000",
            -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(st, 1, pat, -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (ids_push(out, (uint32_t)sqlite3_column_int64(st, 0)) != 0) break;
        if (out->n >= CAND_MAX) break;
    }
    sqlite3_finalize(st);
    return 0;
}
```

**`cmp_cand`** —— 排序键比较（score 升序；相同则按 weight 降序、name）：`src/search/search.c:192-199`

```c
static int cmp_cand(const void *pa, const void *pb)
{
    const Cand *a = (const Cand *)pa, *b = (const Cand *)pb;
    if (a->score < b->score) return -1;
    if (a->score > b->score) return 1;
    if (a->h.weight != b->h.weight) return b->h.weight - a->h.weight;
    return strcmp(a->h.name, b->h.name);
}
```

**`contains_ci`** —— 大小写不敏感子串复核（只对 ASCII 生效，UTF-8 多字节不动）：`src/search/search.c:203-219`

```c
static int contains_ci(const char *hay, const char *needle)
{
    if (strstr(hay, needle)) return 1;
    size_t n = strlen(hay), m = strlen(needle);
    if (m == 0 || m > n) return 0;
    for (size_t i = 0; i + m <= n; i++) {
        size_t k = 0;
        for (; k < m; k++) {
            int ca = (unsigned char)hay[i + k], cb = (unsigned char)needle[k];
            if (ca >= 'A' && ca <= 'Z') ca += 32;
            if (cb >= 'A' && cb <= 'Z') cb += 32;
            if (ca != cb) break;
        }
        if (k == m) return 1;
    }
    return 0;
}
```

## 三、完整实现代码

### 1. 头文件（完整）：`src/search/search.h`

```h
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
```

### 2. `search_bigrams` —— bigram 按"字符"切分（完整）：`src/search/search.c:44-73`

```c
int search_bigrams(const char *q, char grams[][GRAM_BYTES], int max_grams,
                   int *chars)
{
    if (!q) { if (chars) *chars = 0; return 0; }
    const char *end = q + strlen(q);
    const char *st[MAX_GRAMS + 2];
    int nc = 0;
    for (const char *p = q; p < end && nc < MAX_GRAMS + 2;) {
        st[nc++] = p;
        p = u8_next(p, end);
    }
    if (chars) *chars = nc;

    int ng = 0;
    for (int i = 0; i + 1 < nc && ng < max_grams; i++) {
        const char *s = st[i];
        const char *e = (i + 2 < nc) ? st[i + 2] : end;
        size_t len = (size_t)(e - s);
        if (len == 0) continue;
        if (len > GRAM_BYTES - 1) len = GRAM_BYTES - 1;
        int dup = 0;
        for (int k = 0; k < ng; k++)
            if (strlen(grams[k]) == len && memcmp(grams[k], s, len) == 0) { dup = 1; break; }
        if (dup) continue;                     /* "呵呵" 只算一个 gram */
        memcpy(grams[ng], s, len);
        grams[ng][len] = '\0';
        ng++;
    }
    return ng;
}
```

**bigram 搜索原理**：一个长度为 `nc` 字符的关键词会产生 `nc-1` 个相邻二元文法（如"中关村"→`中关`/`关村`）。建库时 `gram(gram, place_id)` 是 `(gram, place_id)` 主键的 WITHOUT ROWID 表，等值查询直接走索引；查询时取"同时含有全部 bigram"的 `place_id` 交集，就把候选从全表 22 万行骤降到极少数。**地名 / POI 反查**：`place` 主表按 `id` 主键单行取详情（`search_open` 预编译的 `st_one` 语句），再由 `search_query` 做子串复核与排序——即从"关键词"反查到"地名/POI 的坐标与属性"。

### 3. `search_open` —— 只读打开（完整）：`src/search/search.c:221-235`

```c
SearchDb *search_open(const char *db_path)
{
    if (!db_path) return NULL;
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return NULL;
    }
    SearchDb *d = calloc(1, sizeof(*d));
    if (!d) { sqlite3_close(db); return NULL; }
    d->db = db;
    sqlite3_prepare_v2(db, "SELECT name,lat,lon,kind,weight FROM place WHERE id=?",
                       -1, &d->st_one, NULL);
    return d;
}
```

### 4. `search_query` —— 查询主流程（完整）：`src/search/search.c:245-330`

五步：① `search_bigrams` 切词；② `ids_by_grams` 取交集（`CAND_MAX=3000` 上限保护耗时）；③ 逐条 `st_one` 取详情（主键单行，微秒级）；④ 子串复核（bigram 全命中 ≠ 连续）；⑤ 排序取 top-K。

```c
int search_query(SearchDb *d, const char *q,
                 int have_center, double clat, double clon,
                 double max_dist_m, int kind_max, int limit,
                 SearchHit *out, int cap, int *n_out, uint64_t *elapsed_ms)
{
    if (n_out) *n_out = 0;
    if (elapsed_ms) *elapsed_ms = 0;
    if (!d || !out || cap <= 0) return -1;
    if (!q || !*q) return 0;                 /* 空串：0 条，不算错 */

    clock_t t0 = clock();

    char grams[MAX_GRAMS][GRAM_BYTES];
    int nchars = 0;
    int ng = search_bigrams(q, grams, MAX_GRAMS, &nchars);

    IdList ids = { 0 };
    if (ng > 0) {
        ids_by_grams(d, grams, ng, &ids);
    } else {
        /* 单字（或空）：先走 gram 前缀，没结果再全表兜底 */
        ids_by_prefix(d, q, &ids);
        if (ids.n == 0) {
            free(ids.v);
            ids.v = NULL; ids.n = ids.cap = 0;
            ids_by_scan(d, q, &ids);
        }
    }

    int lim = limit > 0 ? limit : LIMIT_DEF;
    if (lim > LIMIT_MAX) lim = LIMIT_MAX;
    if (lim > cap) lim = cap;

    Cand *cand = malloc((size_t)(ids.n > 0 ? ids.n : 1) * sizeof(Cand));
    int nc = 0;
    if (cand && d->st_one) {
        for (int i = 0; i < ids.n; i++) {
            sqlite3_reset(d->st_one);
            sqlite3_bind_int64(d->st_one, 1, (sqlite3_int64)ids.v[i]);
            if (sqlite3_step(d->st_one) != SQLITE_ROW) continue;
            const unsigned char *nm = sqlite3_column_text(d->st_one, 0);
            if (!nm) continue;

            SearchHit h;
            snprintf(h.name, sizeof(h.name), "%s", (const char *)nm);
            h.lat    = sqlite3_column_double(d->st_one, 1);
            h.lon    = sqlite3_column_double(d->st_one, 2);
            h.kind   = sqlite3_column_int(d->st_one, 3);
            h.weight = sqlite3_column_int(d->st_one, 4);
            h.dist_m = -1;

            /* ★ 子串复核：gram 全中不等于连续 */
            if (!contains_ci(h.name, q)) continue;
            if (kind_max > 0 && h.kind > kind_max) continue;

            double dist = -1;
            if (have_center) {
                dist = nm_haversine_m(clat, clon, h.lat, h.lon);
                if (max_dist_m > 0 && dist > max_dist_m) continue;
                h.dist_m = dist;
            }
            double base = have_center ? dist / (1.0 + h.weight * 0.05)
                                      : 1000.0 / (1.0 + (double)h.weight);
            int exact  = (strcmp(h.name, q) == 0);
            int prefix = !exact && (strncmp(h.name, q, strlen(q)) == 0);
            double qm   = exact ? 3.0 : (prefix ? 2.0 : 1.0);
            int extra   = (int)strlen(h.name) - (int)strlen(q);
            if (extra < 0) extra = 0;
            h.score = base * (1.0 + extra * 0.03) / qm;
            cand[nc].h = h;
            cand[nc].score = h.score;
            nc++;
        }
    }
    free(ids.v);

    if (nc > 1) qsort(cand, (size_t)nc, sizeof(Cand), cmp_cand);
    int n = nc < lim ? nc : lim;
    for (int i = 0; i < n; i++) out[i] = cand[i].h;
    free(cand);

    if (n_out) *n_out = n;
    if (elapsed_ms)
        *elapsed_ms = (uint64_t)((double)(clock() - t0) / CLOCKS_PER_SEC * 1000);
    return 0;
}
```

**为什么子串复核必要**：bigram 全中不等于连续串（如"中"和"关村"恰好相邻但非连续），不复核会召回误命中。这是 precision/recall 权衡的范例。排序分把"距离主导、权重温和折扣、匹配质量加权、长度惩罚"合成一个数（`cmp_cand` 直接比它），便于 CLI 用单调性断言。

## 四、与其他功能之间的联系

- **上游（L4 自描述二进制与图集 / L3 PBF 解析）**：`places_auto.db` 的 `gram`/`place` 表由 `tools/places_build.c` 从 PBF 生成（地名库建库）。
- **下游 · L8 渲染管线**：搜索结果被 `paint_search_box` / `paint_search_pin`（chrome 步骤 9）消费，经 `a->su` 显示。
- **下游 · L10 路由**：**L9 搜索结果（地名/POI 坐标）直接作为 L10 路由的起终点**——用户在搜索框选一个点，拿它的经纬度喂给 `router_route_ll` / `router_plan`。
- **下游 · L12 Qt 前端**：被 Qt 搜索面板直接调用。
- **注意**：`search_open` 的 sqlite 连接独立于 `LabelCache` 的连接（`window.h` 注释），避免游标互踩。

## 五、演示、练习与验收

- **演示**：`./build_rel/skedaddle_search_cli --db=products/places_auto.db --q=中关村`；`./build_rel/skedaddle_places_build --help`（看地名库怎么生成）。
- **练习**：把"中关村"换成单字查询（如"中"），观察它如何先走 `gram` 前缀再退化到全表 `LIKE`（开 `NAVIMAP_RESTR_DEBUG` 无意义，但可在 `search_query` 里加打印看 `ids_by_prefix` 与 `ids_by_scan` 的触发顺序）。
- **验收（读源码 / 跑命令）**：
  1. 能解释"为什么 bigram + 子串复核"比纯 `LIKE` 快且准，以及单字回退的触发条件（`search.c:264-272`）。
  2. 确认 `search_bigrams` 按**字符**切（`u8_next`，`search.c:33-42`），而非按字节——否则汉字 3 字节会被劈开导致搜索 0 结果。
  3. `search_query` 返回的 `score` 在给定中心点时必须单调（`qsort` 后 `out[i].score` 升序），可用 `skedaddle_search_cli` 输出的距离/权重手算验证。
  4. `LIKE` 的 `%`/`_` 经 `like_literal` 转义（`search.c:76-88`），确认输入通配符不会被当成全表匹配。
