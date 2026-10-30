/* skedaddle_places_build —— PBF → 地名 / POI 库（places.db）
 *
 * 为什么非要有这个工具：
 *   路名有 skedaddle_graph_extract 落到各省的 names.bin，但**地名和 POI 不是 way**——
 *   它们是挂在 node 上的（place=city/town/village、amenity、tourism、shop…），
 *   而 graph_build 只留 ways（pass1 里一个 node 都不写）。所以这些名字既不进
 *   路网、也进不了搜索与地名标注；以前那份 places_auto.db 是旧工具留下的存量，
 *   生成器随源码丢了 —— 新下载一张图，地名就永远补不上。这条链路到这里才闭合。
 *
 *   POI 也可能画成 way（公园、景区、校园是闭合多边形），那种需要坐标，
 *   办法和 graph_build 一样：第 1 遍记下它引用的节点，第 2 遍只回填这批节点的
 *   坐标（排序 + 二分），不建全图索引。
 *
 * 用法：
 *   ./skedaddle_places_build --sources=sources --db=products/places_auto.db [--meta=<file>]
 *   ./skedaddle_places_build --pbf=sources/beijing.osm.pbf --db=/tmp/bj.db
 *   ./skedaddle_places_build --db=<已建好的库> --selftest
 *
 * 产出（SQLite，schema 与旧库存量完全一致，所以加载端不用改）：
 *   place(id, name, lat, lon, kind, weight)
 *   gram (gram, place_id) WITHOUT ROWID
 *   + place(lat,lon) 索引（标注层按视口范围查，没索引就是全表扫）
 *
 * ★ 两个必须对齐的地方，写错都要自我惩罚：
 *   1. kind 的量纲要配得上 src/render/labels.c 的 kind_max_for_zoom()
 *      （z<6 只画 kind<=1 的省会，z>=15 才画到 POI）；
 *   2. bigram 必须和查询端逐字节一致 —— 所以这里直接调用 core 里的
 *      search_bigrams()，不另写一份。两边各抄一遍迟早分叉，分叉的表现是
 *      "某些词搜不到、但又看不出规律"。
 *
 * ★ 跨省去重按 OSM id（省 pbf 彼此重叠，河北那张里已经有整个北京），
 *   键 = id<<2 | 类型位，node / way 各占一位。这是"同一实体"的唯一凭据，
 *   不能拿名字+坐标去重：同一条街两端的中关村姿-med nan two 实体。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <sqlite3.h>

#include "pbf/pbf_stream.h"
#include "pbf/pbf_block.h"
#include "search/search.h"
#include "utils/plat.h"

#define PL_NAME_MAX  96      /* 超出按字符边界截断：显示端 SearchHit.name 是 64 字节 */
#define MAX_WAY_REFS 4000    /* 单个 way 超过这么多顶点就不可能是 POI，跳过 */
#define MAX_KV       64      /* 一个节点最多看多少对 tag */

/* ------------------------------------------------------------------------
 * kind：与 labels.c 的 kind_max_for_zoom 对齐，数值小 = 行政级别高
 * ---------------------------------------------------------------------- */
enum {
    PLK_CAPITAL = 1,   /* 省会 / 直辖市城区 */
    PLK_CITY    = 2,   /* 地级市 */
    PLK_TOWN    = 3,   /* 县 / 镇 */
    PLK_SUBURB  = 4,   /* 城市片区 */
    PLK_VILLAGE = 6,   /* 村（5 留空档，旧库这里也是 0 条） */
    PLK_HAMLET  = 7,   /* 自然村 / 屯 */
    PLK_NBHD    = 9,   /* 小区 / 街区 */
    PLK_NATURAL = 10,  /* 自然地物名（山峰 / 水体 / 岛） */
    PLK_POI     = 11,  /* 有名字的设施（车站 / 学校 / 景区 / 商场…） */
    PLK_FEATURE = 12,  /* 其他有名字的地物（桥梁 / 堤 / 城墙…） */
    PLK_ROAD    = 13,  /* 路名 —— **只进搜索，永不画**（见下方说明） */
};

/* ★ 为什么路名是 13 而不是占一个"正常"档位
 *
 * 旧存量库把路名塞进了 kind 1/3/4/8/12，还给到 weight 1112（一条县道压过所有行政区），
 * 于是 z9 画面上飘的是"北黄路 / 418县道"，而石家庄、唐山一个都不出现 —— 这是把
 * "搜索用的东西"和"画到地图上的东西"混在一张表里导致的。路名本来就该由路网自己画
 * （names.bin 沿路渲染），不需要地名层再叠一遍。
 *
 * 但街道名搜索是真需求（闲逛用户只会记得"中关村大街"），所以这里给路名留一个
 * 单独的 kind 13：labels.c 的 kind_max_for_zoom() 上限是 12，13 天然被排除在标注之外；
 * 而 app_search_query() 传的 kind_max 是 0（不限），搜索照样能命中。
 * 一个字段解决两个需求，谁都不用改。 */

/* ------------------------------------------------------------------------
 * 记账
 * ---------------------------------------------------------------------- */
typedef struct {
    uint64_t nodes_seen, nodes_tagged, ways_seen, ways_named;
    uint64_t rows_place, rows_gram;
    uint64_t dup_dropped, no_gram, name_too_long, way_too_big;
    uint64_t blacklisted, numeric_name, dup_sameplace;
    uint64_t road_rows, road_dup, road_noname;
    double   ms_build, ms_finish;
    int32_t  bb[4];
    int      has_bb;
} BuildStats;

/* ------------------------------------------------------------------------
 * 通用增长缓冲区
 * ---------------------------------------------------------------------- */
typedef struct { char *p; size_t n, cap; } Buf;

static int buf_reserve(Buf *b, size_t add)
{
    if (b->n + add <= b->cap) return 0;
    size_t nc = b->cap ? b->cap : 4096;
    while (nc < b->n + add) nc *= 2;
    char *np = realloc(b->p, nc);
    if (!np) return -1;
    b->p = np; b->cap = nc;
    return 0;
}

/* 存进去并保证以 '\0' 结尾，返回偏移（0 号位空着，方便用 0 表示"没有"） */
static size_t buf_put_cstr(Buf *b, const char *s, size_t len)
{
    if (b->n == 0) {                      /* 留出一个字节当 0 号哨兵 */
        if (buf_reserve(b, 1) != 0) return 0;
        b->p[0] = '\0'; b->n = 1;
    }
    if (buf_reserve(b, len + 1) != 0) return 0;
    size_t off = b->n;
    memcpy(b->p + off, s, len);
    b->p[off + len] = '\0';
    b->n += len + 1;
    return off;
}

/* ------------------------------------------------------------------------
 * 去重键集合：开放寻址，0 当空槽（OSM id 恒 > 0，键不会是 0）
 * ---------------------------------------------------------------------- */
typedef struct { uint64_t *slot; size_t cap, mask, n; } KeySet;

static int keyset_init(KeySet *s, size_t bits)
{
    s->cap = (size_t)1 << bits;
    s->mask = s->cap - 1;
    s->n = 0;
    s->slot = calloc(s->cap, sizeof(uint64_t));
    return s->slot ? 0 : -1;
}

static uint64_t keyset_hash(uint64_t k)
{
    k ^= k >> 33; k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33; k *= 0xc4ceb9fe1a85ec53ULL;
    k ^= k >> 33;
    return k;
}

static void keyset_grow(KeySet *s)
{
    if (s->n * 10 < s->cap * 7) return;            /* 负载 0.7 以下不动 */
    size_t oc = s->cap, nc = oc * 2;
    uint64_t *ns = calloc(nc, sizeof(uint64_t));
    if (!ns) return;                               /* 扩不了就凑合用，只是慢 */
    size_t mask = nc - 1;
    for (size_t i = 0; i < oc; i++) {
        if (!s->slot[i]) continue;
        size_t p = (size_t)keyset_hash(s->slot[i]) & mask;
        while (ns[p]) p = (p + 1) & mask;
        ns[p] = s->slot[i];
    }
    free(s->slot);
    s->slot = ns; s->cap = nc; s->mask = mask;     /* ★ n 不能动：这里只是重排 */
}

static size_t keyset_find(const KeySet *s, uint64_t key)
{
    size_t p = (size_t)keyset_hash(key) & s->mask;
    while (s->slot[p]) {
        if (s->slot[p] == key) return p;
        p = (p + 1) & s->mask;
    }
    return (size_t)-1;
}

static int keyset_has(const KeySet *s, uint64_t key)
{
    return keyset_find(s, key) != (size_t)-1;
}

static void keyset_add(KeySet *s, uint64_t key)
{
    size_t p = (size_t)keyset_hash(key) & s->mask;
    while (s->slot[p]) {
        if (s->slot[p] == key) return;             /* 已经有了 */
        p = (p + 1) & s->mask;
    }
    s->slot[p] = key;
    s->n++;
}

/* 返回 1 = 已经有了（该跳过），0 = 新插入 */
static int keyset_seen(KeySet *s, uint64_t key)
{
    keyset_grow(s);
    if (keyset_has(s, key)) return 1;
    keyset_add(s, key);
    return 0;
}

/* ------------------------------------------------------------------------
 * 输出
 * ---------------------------------------------------------------------- */
typedef struct {
    sqlite3       *db;
    sqlite3_stmt  *ins_place, *ins_gram;
    sqlite3_int64  next_id;
} Sink;

static const char *kSchema =
    "DROP TABLE IF EXISTS place;"
    "DROP TABLE IF EXISTS gram;"
    "DROP INDEX IF EXISTS place_latlon;"
    "CREATE TABLE place(  id     INTEGER PRIMARY KEY,"
    "                     name   TEXT    NOT NULL,"
    "                     lat    REAL    NOT NULL,"
    "                     lon    REAL    NOT NULL,"
    "                     kind   INTEGER NOT NULL,"
    "                     weight INTEGER NOT NULL);"
    "CREATE TABLE gram(   gram     TEXT    NOT NULL,"
    "                     place_id INTEGER NOT NULL,"
    "                     PRIMARY KEY(gram, place_id)) WITHOUT ROWID;";

static int sink_open(Sink *s, const char *path)
{
    memset(s, 0, sizeof(*s));
    if (sqlite3_open(path, &s->db) != SQLITE_OK) {
        fprintf(stderr, "打不开 %s: %s\n", path, sqlite3_errmsg(s->db));
        return -1;
    }
    /* 这是个一次性构建工具：产物坏了重跑一遍就行，速度比掉电安全重要 */
    sqlite3_exec(s->db, "PRAGMA journal_mode=OFF", NULL, NULL, NULL);
    sqlite3_exec(s->db, "PRAGMA synchronous=OFF", NULL, NULL, NULL);
    sqlite3_exec(s->db, "PRAGMA page_size=4096", NULL, NULL, NULL);
    char *err = NULL;
    if (sqlite3_exec(s->db, "BEGIN", NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "BEGIN 失败: %s\n", err); return -1;
    }
    if (sqlite3_exec(s->db, kSchema, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "建表失败: %s\n", err); return -1;
    }
    if (sqlite3_prepare_v2(s->db,
            "INSERT INTO place(id,name,lat,lon,kind,weight) VALUES(?1,?2,?3,?4,?5,?6)",
            -1, &s->ins_place, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(s->db, "INSERT OR REPLACE INTO gram(gram,place_id) VALUES(?1,?2)",
            -1, &s->ins_gram, NULL) != SQLITE_OK) {
        fprintf(stderr, "prepare 失败: %s\n", sqlite3_errmsg(s->db));
        return -1;
    }
    return 0;
}

/* 收尾才建索引：边插边维护 B 树会让最后这一点点工作拖成几分钟 */
static int sink_finish(Sink *s)
{
    int rc = 0;
    char *err = NULL;
    if (sqlite3_exec(s->db, "COMMIT", NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "COMMIT 失败: %s\n", err); return -1;
    }
    if (sqlite3_exec(s->db, "CREATE INDEX place_latlon ON place(lat,lon)",
                     NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "建索引失败: %s\n", err); rc = -1;
    }
    return rc;
}

static void sink_close(Sink *s)
{
    if (s->ins_place) sqlite3_finalize(s->ins_place);
    if (s->ins_gram)  sqlite3_finalize(s->ins_gram);
    if (s->db)        sqlite3_close(s->db);
    memset(s, 0, sizeof(*s));
}

/* ------------------------------------------------------------------------
 * 命名实体分类
 * ---------------------------------------------------------------------- */
typedef struct {
    int  hit;      /* 是不是我们想要的实体 */
    int  kind;
    int  weight;
} Verdict;

/* 名字：优先 name，没有就用 name:zh（中文 OSM 里两者常一致，别指望本地化后缀） */
static const char *pick_name(PbfBlock *b, const uint32_t *k, const uint32_t *v,
                             size_t n, size_t *len_out)
{
    const char *name = NULL, *name_zh = NULL;
    size_t nl = 0, zl = 0;
    for (size_t i = 0; i < n; i++) {
        const char *kk = pbf_str(b, k[i]);
        if (!kk) continue;
        if (strcmp(kk, "name") == 0) {
            name = pbf_str(b, v[i]);
            nl = name ? strlen(name) : 0;
        } else if (strcmp(kk, "name:zh") == 0) {
            name_zh = pbf_str(b, v[i]);
            zl = name_zh ? strlen(name_zh) : 0;
        }
    }
    if (name && nl) { *len_out = nl; return name; }
    if (name_zh && zl) { *len_out = zl; return name_zh; }
    *len_out = 0;
    return NULL;
}

static const char *kv_get(PbfBlock *b, const uint32_t *k, const uint32_t *v,
                          size_t n, const char *key)
{
    for (size_t i = 0; i < n; i++) {
        const char *kk = pbf_str(b, k[i]);
        if (kk && strcmp(kk, key) == 0) return pbf_str(b, v[i]);
    }
    return NULL;
}

static int64_t parse_pop(const char *s)
{
    if (!s) return 0;
    int64_t v = 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') return v > 0 ? v : 0;  /* "1,2000" 这种取前面那段 */
        v = v * 10 + (*p - '0');
        if (v > 100000000LL) return 100000000LL;
    }
    return v;
}

/* 黑名单：挂了这些 tag 的一律不要。
 * 不是"不重要"，是**它们没资格当地名**——先按宽口径抽一版看质量（北京一次抽
 * 出 7.3 万条），逐个审视噪音源后才定下这张表。最典型的是 bus_stop：北京一个市
 * 就有 2.3 万个，全是"XX桥""XX路口"这种路况描述，会把真正的东西挤到后页去。 */
static int tag_blacklisted(const char *key, const char *val)
{
    struct { const char *k, *v; } blk[] = {
        { "highway", "bus_stop" },   { "highway", "crossing" },
        { "highway", "street_lamp" },{ "highway", "traffic_signals" },
        { "highway", "motorway_junction" }, { "highway", "milestone" },
        { "public_transport", "platform" }, { "public_transport", "stop_position" },
        { "railway", "tram_stop" },  { "railway", "switch" },
        { "railway", "level_crossing" }, { "railway", "signal" },
        { "man_made", "mast" },      { "man_made", "pole" },
        { "man_made", "flagpole" },  { "man_made", "power_tower" },
        { "man_made", "survey_point" }, { "man_made", "water_tower" },
        { "man_made", "storage_tank" }, { "man_made", "utility_pole" },
        { "man_made", "pipeline" },  { "man_made", "surveillance" },
        { "natural", "tree" },       { "natural", "tree_row" },
        { "natural", "shrub" },      { "natural", "grass" },
        { "amenity", "atm" },        { "amenity", "bench" },
        { "amenity", "waste_basket" }, { "amenity", "recycling" },
        { "amenity", "vending_machine" }, { "amenity", "post_box" },
        { "amenity", "telephone" },  { "amenity", "toilets" },
        { "amenity", "bicycle_parking" }, { "amenity", "parking_space" },
        { "amenity", "shelter" },    { "amenity", "water_point" },
        { "amenity", "drinking_water" }, { "amenity", "charging_station" },
        { "leisure", "pitch" },      { "leisure", "playground" },
        { "leisure", "fitness_station" },
        { "tourism", "artwork" },    { "tourism", "picnic_site" },
        { "historic", "milestone" }, { "historic", "wayside_cross" },
    };
    for (size_t i = 0; i < sizeof(blk) / sizeof(blk[0]); i++)
        if (strcmp(blk[i].k, key) == 0 && strcmp(blk[i].v, val) == 0) return 1;
    /* office=* 是公司名（几百家"XX研究所"），不是地名 */
    if (strcmp(key, "office") == 0 || strcmp(key, "advertising") == 0) return 1;
    return 0;
}

/* 单线程的一次性构建工具，用文件级计数器换省心（省得把 BuildStats 一路传下来） */
static uint64_t g_blacklisted = 0;

static int has_blacklisted(PbfBlock *b, const uint32_t *k, const uint32_t *v, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        const char *kk = pbf_str(b, k[i]);
        if (!kk) continue;
        const char *vv = pbf_str(b, v[i]);
        if (vv && tag_blacklisted(kk, vv)) { g_blacklisted++; return 1; }
    }
    return 0;
}

/* POI 类。返回权重；0 = 不认 */
static int poi_weight(const char *key, const char *val)
{
    struct { const char *k, *v; int w; } tbl[] = {
        { "railway",    "station",      60 }, { "aeroway",  "aerodrome",  60 },
        { "amenity",    "airport",      60 }, { "amenity",  "ferry_terminal", 55 },
        { "amenity",    "university",   50 }, { "amenity",  "hospital",   50 },
        { "tourism",    "attraction",   45 }, { "historic", "monument",   45 },
        { "leisure",    "park",         40 }, { "amenity",  "school",     35 },
        { "amenity",    "college",      35 }, { "tourism",  "museum",     35 },
        { "historic",   "memorial",     30 }, { "amenity",  "library",    30 },
        { "leisure",    "stadium",      30 }, { "amenity",  "police",     25 },
        { "amenity",    "bank",         25 }, { "amenity",  "pharmacy",   25 },
        { "tourism",    "hotel",        25 }, { "amenity",  "marketplace",25 },
        { "shop",       "mall",         22 }, { "leisure",  "square",     22 },
        { "amenity",    "restaurant",   18 }, { "amenity",  "fast_food",  15 },
        { "amenity",    "cafe",         15 }, { "shop",     "supermarket",15 },
    };
    for (size_t i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++)
        if (strcmp(tbl[i].k, key) == 0 && strcmp(tbl[i].v, val) == 0) return tbl[i].w;
    return 0;
}

/* 只要见到这些键中的任意一个（带名字），就当 POI / 自然地物收进来，
 * 权重走上面那张表，表里没有的给个底分。 */
static int class_node_tags(PbfBlock *b, const uint32_t *k, const uint32_t *v,
                           size_t n, Verdict *out)
{
    out->hit = 0; out->kind = 0; out->weight = 0;
    if (has_blacklisted(b, k, v, n)) return 0;

    /* ---- 行政区：place=* ---- */
    const char *place = kv_get(b, k, v, n, "place");
    if (place) {
        int64_t pop = parse_pop(kv_get(b, k, v, n, "population"));
        const char *cap = kv_get(b, k, v, n, "capital");
        int is_capital = cap && (strcmp(cap, "yes") == 0 || strcmp(cap, "2") == 0 ||
                                 strcmp(cap, "4") == 0);
        int kind, w;
        if      (strcmp(place, "city") == 0)         { kind = is_capital ? PLK_CAPITAL : PLK_CITY;
                                                       w = is_capital ? 300 : 240; }
        else if (strcmp(place, "town") == 0)         { kind = PLK_TOWN;    w = 160; }
        else if (strcmp(place, "suburb") == 0)       { kind = PLK_SUBURB;  w = 120; }
        else if (strcmp(place, "village") == 0)      { kind = PLK_VILLAGE; w = 80;  }
        else if (strcmp(place, "hamlet") == 0)       { kind = PLK_HAMLET;  w = 50;  }
        else if (strcmp(place, "isolated_dwelling") == 0) { kind = PLK_HAMLET; w = 40; }
        else if (strcmp(place, "neighbourhood") == 0 ||
                 strcmp(place, "quarter") == 0 ||
                 strcmp(place, "city_block") == 0)   { kind = PLK_NBHD;    w = 40;  }
        else if (strcmp(place, "island") == 0 ||
                 strcmp(place, "islet") == 0 ||
                 strcmp(place, "locality") == 0 ||
                 strcmp(place, "farm") == 0)         { kind = PLK_NATURAL; w = 30;  }
        else if (strcmp(place, "square") == 0)       { kind = PLK_FEATURE; w = 25;  }
        else return 0;                                /* place=* 的其余取值不收 */

        /* 人口给个温和加成：同级的"省会 vs 小城"要能分开，但别盖过级别 */
        if      (pop >= 2000000) w += 40;
        else if (pop >= 500000)  w += 25;
        else if (pop >= 100000)  w += 15;
        else if (pop >= 30000)   w += 8;
        out->hit = 1; out->kind = kind; out->weight = w;
        return 1;
    }

    /* ---- 行政首府的另一副面孔：带 capital=* 但没有 place=* ----
     * OSM 里这两种写法并存，而且偏偏是"最重要的那个"常常是后者：
     * 北京市（node 25248662）身上只有 capital=yes + admin_level=2，没有任何 place 标签，
     * 而它下辖的 16 个区反而都有 place=city（capital=6）。只认 place=* 的结果是
     * —— 全省最重要的一条地名被丢掉，低缩（z<6 只画 kind<=1）一个字都不剩。
     * 实测河北省/石家庄同理。所以这里用 capital=yes|2|4 兜住，行政级别从
     * admin_level 走：level<=2 首都 / <=4 省会，其余（6=区县、8=乡镇）不收，
     * 那是 place=city/capital=6 已经覆盖的范围，再收就重复了。 */
    {
        const char *cap = kv_get(b, k, v, n, "capital");
        int is_capital = cap && (strcmp(cap, "yes") == 0 || strcmp(cap, "2") == 0 ||
                                 strcmp(cap, "4") == 0);
        if (is_capital) {
            int64_t lvl = parse_pop(kv_get(b, k, v, n, "admin_level"));  /* 复用整数解析 */
            if (lvl > 0 && lvl <= 4) {
                out->hit = 1;
                out->kind = PLK_CAPITAL;
                out->weight = (lvl <= 2) ? 320 : 300;   /* 首都压过省会一点点 */
                return 1;
            }
        }
    }

    /* ---- POI / 自然地物 / 其他地物 ---- */
    static const char *keys[] = {
        "amenity", "shop", "tourism", "leisure", "historic", "natural",
        "man_made", "aeroway", "railway", "highway", "bridge", "waterway"
    };
    int best = 0, best_kind = 0;
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        const char *val = kv_get(b, k, v, n, keys[i]);
        if (!val) continue;
        /* building=* 单独出现不收：满地图"3 号楼""XX 小区 A 座"，只灌噪音 */
        if (strcmp(keys[i], "highway") == 0) {
            if (strcmp(val, "services") == 0 || strcmp(val, "rest_area") == 0 ||
                strcmp(val, "bus_stop") == 0 || strcmp(val, "trailhead") == 0) {
                if (best < 12) { best = 12; best_kind = PLK_FEATURE; }
            }
            continue;                          /* 其余 highway 是路，交给 names.bin */
        }
        if (strcmp(keys[i], "railway") == 0 && strcmp(val, "station") != 0 &&
            strcmp(val, "halt") != 0 && strcmp(val, "yard") != 0) {
            if (best < 10) { best = 10; best_kind = PLK_FEATURE; }
            continue;
        }
        if (strcmp(keys[i], "waterway") == 0 || strcmp(keys[i], "bridge") == 0) {
            if (best < 15) { best = 15; best_kind = PLK_FEATURE; }
            continue;
        }
        if (strcmp(keys[i], "natural") == 0) {
            int w = poi_weight("natural", val);
            if (w == 0) w = 30;
            if (best < w) { best = w; best_kind = PLK_NATURAL; }
            continue;
        }
        int w = poi_weight(keys[i], val);
        if (w == 0) {
            if (best < 10) { best = 10; best_kind = PLK_POI; }
        } else if (best < w) {
            best = w; best_kind = PLK_POI;
        }
    }
    if (!best) return 0;
    out->hit = 1; out->kind = best_kind; out->weight = best > 400 ? 400 : best;
    return 1;
}

/* way 的分类：只收"有面 / 有位置意义"的那几类。
 * ★ 刻意不收纯道路：路名本来就在各省 names.bin 里，再灌一份进 place 表，
 *   一是体积翻倍，二是同一条路会被 paint_road_labels 和 paint_place_labels
 *   各标一次，画面上重影。 */
static int class_way_tags(PbfBlock *b, const uint32_t *k, const uint32_t *v,
                          size_t n, Verdict *out)
{
    /* 黑名单优先：整类不要的东西先挡掉，省得后面再给它打权重 */
    if (has_blacklisted(b, k, v, n)) return 0;

    static const char *keys[] = {
        "amenity", "shop", "tourism", "leisure", "historic", "natural",
        "man_made", "aeroway", "landuse", "place"
    };
    static const char *skip_keys[] = { "building", "highway", "railway", "waterway" };

    /* 带路面 tag 的 way（就算同时挂着 name）一律不收 */
    for (size_t i = 0; i < sizeof(skip_keys) / sizeof(skip_keys[0]); i++)
        if (kv_get(b, k, v, n, skip_keys[i])) return 0;

    int best = 0, best_kind = 0;
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        const char *val = kv_get(b, k, v, n, keys[i]);
        if (!val) continue;
        int kind, w;
        if (strcmp(keys[i], "natural") == 0) {
            kind = PLK_NATURAL;
            w = poi_weight("natural", val);
            if (w == 0) w = 25;
        } else if (strcmp(keys[i], "place") == 0) {
            /* place=square / island 画成多边形的情况 */
            if (strcmp(val, "square") != 0 && strcmp(val, "island") != 0 &&
                strcmp(val, "islet") != 0 && strcmp(val, "locality") != 0) continue;
            kind = (strcmp(val, "square") == 0) ? PLK_FEATURE : PLK_NATURAL;
            w = 25;
        } else {
            kind = PLK_POI;
            w = poi_weight(keys[i], val);
            if (w == 0) w = 10;
        }
        if (best < w) { best = w; best_kind = kind; }
    }
    if (!best) return 0;
    out->hit = 1; out->kind = best_kind; out->weight = best > 400 ? 400 : best;
    return 1;
}

/* ------------------------------------------------------------------------
 * 构建主体
 * ---------------------------------------------------------------------- */
typedef struct {
    int64_t  osm_id;
    uint32_t ref_off, ref_cnt;
    uint32_t name_off;
    uint16_t kind;
    int16_t  weight;
} WayCand;

typedef struct {
    KeySet    seen;      /* OSM id：跨省重复（河北那张里整个北京都在） */
    KeySet    sameplace; /* 同名且位置几乎重合：同一个东西既有点又有多边形（颐和园）*/
    KeySet    roadnames; /* 路名（hash64）：一条路被切成几十个 way，只留第一个 */
    Sink      sink;
    Buf       refs, names;
    WayCand  *wc;
    size_t    nwc, capwc;
    int64_t  *ids;        /* 候选 way 引用的所有节点，排序去重后 */
    int32_t  *lat, *lon;
    uint8_t  *have;
    size_t    n_ids;
    BuildStats st;       /* 当前这一个源文件 */
    BuildStats tot;      /* 累计 */
} Builder;

static int cmp_i64(const void *a, const void *b)
{
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return (x > y) - (x < y);
}

static size_t lower_bound_i64(const int64_t *a, size_t n, int64_t key)
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (a[mid] < key) lo = mid + 1; else hi = mid;
    }
    return lo;
}

static void bb_update(BuildStats *st, int32_t lat, int32_t lon)
{
    if (!st->has_bb) {
        st->bb[0] = st->bb[2] = lat;
        st->bb[1] = st->bb[3] = lon;
        st->has_bb = 1;
        return;
    }
    if (lat < st->bb[0]) st->bb[0] = lat;
    if (lon < st->bb[1]) st->bb[1] = lon;
    if (lat > st->bb[2]) st->bb[2] = lat;
    if (lon > st->bb[3]) st->bb[3] = lon;
}

/* 按字符边界截断，不能把汉字劈成两半（劈了会写进库里变成半个乱码，
 * 而且到时候肉眼看截图是"某个地名最后一个字是问号"，很难联想到这里） */
static size_t clip_name(const char *s, size_t len, size_t max)
{
    if (len <= max) return len;
    size_t i = 0, last = 0;
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        size_t n = 1;
        if      (c >= 0xF0) n = 4;
        else if (c >= 0xE0) n = 3;
        else if (c >= 0xC0) n = 2;
        if (i + n > max) return last ? last : i;
        last = i + n;
        i += n;
    }
    return last;
}

/* 去掉空白后全是数字的名字（"551"、"226"）是编号，不是地名。
 * 实测北京抽上来一百多条这种，混在搜索结果里很难解释为"搜索质量"。 */
static int name_is_numeric(const char *s)
{
    int any = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (isspace(*p)) continue;
        if (*p < '0' || *p > '9') return 0;
        any = 1;
    }
    return any;
}

static uint64_t fnv1a(const char *s)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        h ^= (uint64_t)*p;
        h *= 0x100000001b3ULL;
    }
    return h;
}

/* 同名 ± 300 m 之内只留先到的那条。
 *
 * 为什么要这一层：同一个实体常被同时画成点和一个多边形（颐和园既有签到点、
 * 又有整片园区），抽出来就是两条同名记录，搜索结果里挨着显示两遍。
 *
 * ★ 为什么不能按"同名就删"：实测北京同名同 kind 的配对（1.7 万组里剩下那些）
 *   中位距离 20 km —— 那是全国几十个"八大处村"，删了就真的少了地方的。
 *   所以只在"近到几乎肯定同一实体"这个尺度上合并，并且要把邻格也看一眼，
 *   否则正好跨格子边界的一对会漏。
 */
#define SAME_PLACE_DEG 0.003     /* ≈ 330 m */
static int same_place_seen(Builder *b, const char *name, double lat, double lon)
{
    uint64_t nm = fnv1a(name);
    int qy = (int)floor(lat / SAME_PLACE_DEG);
    int qx = (int)floor(lon / SAME_PLACE_DEG);
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            uint64_t cell = ((uint64_t)(uint32_t)(qy + dy) << 32) |
                            (uint64_t)(uint32_t)(qx + dx);
            if (keyset_has(&b->sameplace, nm ^ cell)) return 1;
        }
    keyset_grow(&b->sameplace);
    keyset_add(&b->sameplace,
               nm ^ (((uint64_t)(uint32_t)qy << 32) | (uint64_t)(uint32_t)qx));
    return 0;
}

static int emit_row(Builder *b, BuildStats *st, uint64_t osm_key, double lat,
                    double lon, int kind, int weight, const char *name)
{
    if (!name || !*name) return 0;
    if (keyset_seen(&b->seen, osm_key)) { st->dup_dropped++; return 0; }
    if (name_is_numeric(name))          { st->numeric_name++; return 0; }
    if (same_place_seen(b, name, lat, lon)) { st->dup_sameplace++; return 0; }

    sqlite3_int64 id = ++b->sink.next_id;
    sqlite3_stmt *s = b->sink.ins_place;
    sqlite3_reset(s);
    sqlite3_bind_int64(s, 1, id);
    sqlite3_bind_text(s, 2, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(s, 3, lat);
    sqlite3_bind_double(s, 4, lon);
    sqlite3_bind_int(s, 5, kind);
    sqlite3_bind_int(s, 6, weight);
    if (sqlite3_step(s) != SQLITE_DONE) return -1;
    st->rows_place++;

    /* ★ bigram 走 core 里那份，保证与 search.c 的查询口径一致 */
    char grams[32][16];
    int nchar = 0;
    int ng = search_bigrams(name, grams, 32, &nchar);
    if (ng == 0) st->no_gram++;
    for (int i = 0; i < ng; i++) {
        sqlite3_stmt *g = b->sink.ins_gram;
        sqlite3_reset(g);
        sqlite3_bind_text(g, 1, grams[i], -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(g, 2, id);
        if (sqlite3_step(g) != SQLITE_DONE) return -1;
        st->rows_gram++;
    }
    return 0;
}

static int handle_tagged_node(Builder *b, PbfBlock *blk, int64_t osm_id,
                              int32_t lat_e7, int32_t lon_e7,
                              const uint32_t *keys, const uint32_t *vals, size_t n)
{
    BuildStats *st = &b->st;
    st->nodes_tagged++;
    if (n == 0) return 0;

    Verdict vd;
    if (!class_node_tags(blk, keys, vals, n, &vd) || !vd.hit) return 0;

    size_t nl = 0;
    const char *nm = pick_name(blk, keys, vals, n, &nl);
    if (!nm) return 0;

    char name[PL_NAME_MAX];
    size_t clen = clip_name(nm, nl, PL_NAME_MAX - 1);
    if (clen < nl) st->name_too_long++;
    memcpy(name, nm, clen);
    name[clen] = '\0';

    uint64_t key = ((uint64_t)osm_id << 2) | 1u;    /* 1 = node */
    return emit_row(b, st, key, lat_e7 / 1e7, lon_e7 / 1e7,
                    vd.kind, vd.weight, name);
}

/* 名字的 64 位指纹，用于路名去重。
 * ★ 刻意用 hash 而不是存字符串：8 省约 53 万条带名字的 highway way、去重后十几万个名字，
 *   存字符串要管内存，而 64 bit 空间下十几万个键的碰撞概率可以忽略；万一碰上了，
 *   后果也只是"某条路没进搜索"，不是错的搜索结果。 */
static uint64_t name_fnv64(const char *s)
{
    uint64_t h = 1469598103934665603ULL;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        h ^= *p;
        h *= 1099511628211ULL;
    }
    return h ? h : 1;                    /* 0 是"空槽"的哨兵，不能给出去 */
}

/* 道路等级 → 搜索排序权重。比的是"同一条街的几个候选先报哪个"，
 * 不参与地图渲染（kind 13 被 labels 排除），所以只要相对次序对就行。 */
static int road_weight(const char *hw)
{
    struct { const char *v; int w; } tbl[] = {
        { "motorway", 34 }, { "trunk", 32 },   { "primary", 30 },
        { "secondary", 28 },{ "tertiary", 26 },{ "unclassified", 22 },
        { "residential", 20 }, { "living_street", 18 }, { "pedestrian", 18 },
        { "service", 12 },  { "track", 10 },   { "path", 8 },
        { "footway", 8 },   { "cycleway", 8 }, { "steps", 6 },
    };
    for (size_t i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++)
        if (strcmp(hw, tbl[i].v) == 0) return tbl[i].w;
    return 8;                            /* 其他取值（road / corridor…）给个底分 */
}

static int pass1(Builder *b, const char *path)
{
    PbfStream *s = pbf_stream_open(path);
    if (!s) { fprintf(stderr, "打不开 %s\n", path); return -1; }

    const char *type; const uint8_t *buf; size_t len;
    PbfStatus rc;
    int ret = 0;

    while ((rc = pbf_stream_next(s, &type, &buf, &len)) == PBF_OK) {
        if (!type || strcmp(type, "OSMData") != 0) continue;
        PbfBlock *blk = NULL;
        if (pbf_unpack_block(buf, len, &blk) != 0) { ret = -1; break; }

        for (size_t gi = 0; gi < blk->pb->n_primitivegroup; gi++) {
            PrimitiveGroup *pg = blk->pb->primitivegroup[gi];
            if (!pg) continue;

            /* ---- DenseNodes：带了 keys_vals 的才可能是地名 / POI ---- */
            if (pg->dense) {
                DenseNodes *dn = pg->dense;
                if (dn->n_id != dn->n_lat || dn->n_id != dn->n_lon) {
                    fprintf(stderr, "DenseNodes 数组长度不一致\n");
                    ret = -1; break;
                }
                size_t kvp = 0;
                size_t nkv = dn->n_keys_vals;
                int64_t acc_id = 0, acc_lat = 0, acc_lon = 0;
                for (size_t i = 0; i < dn->n_id; i++) {
                    acc_id  += dn->id[i];
                    acc_lat += dn->lat[i];
                    acc_lon += dn->lon[i];
                    b->st.nodes_seen++;

                    uint32_t ks[MAX_KV], vs[MAX_KV];
                    size_t nk = 0;
                    if (kvp < nkv) {
                        while (kvp < nkv && dn->keys_vals[kvp] != 0) {
                            uint32_t kk = dn->keys_vals[kvp++];
                            if (kvp >= nkv) break;
                            uint32_t vv = dn->keys_vals[kvp++];
                            if (nk < MAX_KV) { ks[nk] = kk; vs[nk] = vv; nk++; }
                        }
                        kvp++;                       /* 跳过这个节点的分隔 0 */
                    }
                    if (nk == 0) continue;

                    int32_t la = (int32_t)pbf_to_e7(blk, acc_lat, blk->pb->lat_offset);
                    int32_t lo = (int32_t)pbf_to_e7(blk, acc_lon, blk->pb->lon_offset);
                    if (handle_tagged_node(b, blk, acc_id, la, lo, ks, vs, nk) != 0) {
                        ret = -1; break;
                    }
                }
                if (ret) break;
            }

            /* ---- 非 dense 的 Node 消息（有些文件把带 tag 的节点写成这样） ---- */
            for (size_t i = 0; i < pg->n_nodes; i++) {
                Node *nd = pg->nodes[i];
                if (!nd) continue;
                b->st.nodes_seen++;
                size_t nk = nd->n_keys < nd->n_vals ? nd->n_keys : nd->n_vals;
                if (nk == 0) continue;
                if (nk > MAX_KV) nk = MAX_KV;
                int32_t la = (int32_t)pbf_to_e7(blk, nd->lat, blk->pb->lat_offset);
                int32_t lo = (int32_t)pbf_to_e7(blk, nd->lon, blk->pb->lon_offset);
                if (handle_tagged_node(b, blk, nd->id, la, lo,
                                       nd->keys, nd->vals, nk) != 0) { ret = -1; break; }
            }
            if (ret) break;

            /* ---- ways：只挑 POI / 自然地物 ---- */
            for (size_t i = 0; i < pg->n_ways; i++) {
                Way *w = pg->ways[i];
                if (!w) continue;
                b->st.ways_seen++;

                /* ---- ways 之一：路名（kind 13，只进搜索） ----
                 * 不需要几何中心 —— 街道名只要定位到"这条街上的某个点"就够了，
                 * 差几十米不影响用户找得到它。所以这里**只取 refs 中点一个节点**，
                 * 而不是把整条路的顶点都攒下来：北京 6.3 万条、8 省 53 万条带名字的路，
                 * 全量攒 refs 是几百 MB 的内存换一堆没有用的精度。 */
                {
                    size_t nw = w->n_keys < w->n_vals ? w->n_keys : w->n_vals;
                    size_t nl = 0;
                    const char *nm = pick_name(blk, w->keys, w->vals, nw, &nl);
                    const char *hw = nm ? kv_get(blk, w->keys, w->vals, nw, "highway")
                                        : NULL;
                    if (nm && hw && w->n_refs > 0) {
                        char name[PL_NAME_MAX];
                        size_t clen = clip_name(nm, nl, PL_NAME_MAX - 1);
                        memcpy(name, nm, clen);
                        name[clen] = '\0';

                        if (b->nwc >= b->capwc) {
                            size_t nc = b->capwc ? b->capwc * 2 : 1024;
                            WayCand *wp = realloc(b->wc, nc * sizeof(WayCand));
                            if (!wp) { ret = -1; break; }
                            b->wc = wp; b->capwc = nc;
                        }
                        if (buf_reserve(&b->refs, sizeof(int64_t)) != 0) { ret = -1; break; }

                        size_t mid = w->n_refs / 2;
                        int64_t acc = 0;
                        for (size_t k = 0; k <= mid; k++) acc += w->refs[k];

                        size_t word_ndx = b->refs.n / sizeof(int64_t);
                        ((int64_t *)b->refs.p)[word_ndx] = acc;
                        b->refs.n += sizeof(int64_t);

                        size_t noff = buf_put_cstr(&b->names, name, clen);
                        WayCand *wc = &b->wc[b->nwc++];
                        wc->osm_id  = w->id;
                        wc->ref_off = (uint32_t)word_ndx;
                        wc->ref_cnt = 1;
                        wc->name_off = (uint32_t)noff;
                        wc->kind    = PLK_ROAD;
                        wc->weight  = (int16_t)road_weight(hw);
                        continue;
                    }
                }

                Verdict vd;
                if (!class_way_tags(blk, w->keys, w->vals,
                                    w->n_keys < w->n_vals ? w->n_keys : w->n_vals, &vd))
                    continue;
                if (!vd.hit) continue;

                size_t nl = 0;
                const char *nm = pick_name(blk, w->keys, w->vals,
                                           w->n_keys < w->n_vals ? w->n_keys : w->n_vals, &nl);
                if (!nm) continue;
                if (w->n_refs == 0 || w->n_refs > MAX_WAY_REFS) {
                    b->st.way_too_big++;
                    continue;
                }
                b->st.ways_named++;

                if (b->nwc >= b->capwc) {
                    size_t nc = b->capwc ? b->capwc * 2 : 1024;
                    WayCand *nw = realloc(b->wc, nc * sizeof(WayCand));
                    if (!nw) { ret = -1; break; }
                    b->wc = nw; b->capwc = nc;
                }
                if (buf_reserve(&b->refs, w->n_refs * sizeof(int64_t)) != 0) { ret = -1; break; }

                char name[PL_NAME_MAX];
                size_t clen = clip_name(nm, nl, PL_NAME_MAX - 1);
                memcpy(name, nm, clen);
                name[clen] = '\0';

                size_t word_ndx = b->refs.n / sizeof(int64_t);
                int64_t *rp = (int64_t *)b->refs.p;
                int64_t acc = 0;
                for (size_t k = 0; k < w->n_refs; k++) {
                    acc += w->refs[k];
                    rp[word_ndx + k] = acc;
                }
                b->refs.n += w->n_refs * sizeof(int64_t);

                size_t noff = buf_put_cstr(&b->names, name, clen);
                WayCand *wc = &b->wc[b->nwc++];
                wc->osm_id  = w->id;
                wc->ref_off = (uint32_t)word_ndx;
                wc->ref_cnt = (uint32_t)w->n_refs;
                wc->name_off = (uint32_t)noff;
                wc->kind    = (uint16_t)vd.kind;
                wc->weight  = (int16_t)vd.weight;
            }
            if (ret) break;
        }
        pbf_block_free(blk);
        if (ret) break;
    }
    pbf_stream_close(s);
    return ret == 0 && rc != PBF_ERR ? 0 : -1;
}

/* 候选 way 引用了哪些节点 —— 排序去重，供第 2 遍回填坐标 */
static int build_coord_index(Builder *b)
{
    size_t nref = b->refs.n / sizeof(int64_t);
    b->n_ids = 0;
    if (nref == 0) return 0;

    int64_t *tmp = malloc(nref * sizeof(int64_t));
    if (!tmp) return -1;
    memcpy(tmp, b->refs.p, nref * sizeof(int64_t));
    qsort(tmp, nref, sizeof(int64_t), cmp_i64);

    size_t w = 0;
    for (size_t i = 0; i < nref; i++)
        if (i == 0 || tmp[i] != tmp[i - 1]) tmp[w++] = tmp[i];

    b->ids  = tmp;
    b->n_ids = w;
    /* ★ 一律显式清零：count/flag 数组靠"大块走 mmap 是零页"活着的代码，
     *   会在数据量变小、分配改走主堆那天突然带着垃圾值崩给人看（本机踩过）。 */
    b->lat  = calloc(w, sizeof(int32_t));
    b->lon  = calloc(w, sizeof(int32_t));
    b->have = calloc(w, sizeof(uint8_t));
    if (!b->lat || !b->lon || !b->have) return -1;
    return 0;
}

static int pass2(Builder *b, const char *path)
{
    PbfStream *s = pbf_stream_open(path);
    if (!s) return -1;

    const char *type; const uint8_t *buf; size_t len;
    PbfStatus rc;
    while ((rc = pbf_stream_next(s, &type, &buf, &len)) == PBF_OK) {
        if (!type || strcmp(type, "OSMData") != 0) continue;
        PbfBlock *blk = NULL;
        if (pbf_unpack_block(buf, len, &blk) != 0) { pbf_stream_close(s); return -1; }

        for (size_t gi = 0; gi < blk->pb->n_primitivegroup; gi++) {
            PrimitiveGroup *pg = blk->pb->primitivegroup[gi];
            if (!pg || !pg->dense) continue;
            DenseNodes *dn = pg->dense;
            if (dn->n_id != dn->n_lat || dn->n_id != dn->n_lon) continue;

            int64_t acc_id = 0, acc_lat = 0, acc_lon = 0;
            for (size_t i = 0; i < dn->n_id; i++) {
                acc_id  += dn->id[i];
                acc_lat += dn->lat[i];
                acc_lon += dn->lon[i];
                size_t pos = lower_bound_i64(b->ids, b->n_ids, acc_id);
                if (pos >= b->n_ids || b->ids[pos] != acc_id) continue;
                int32_t la = (int32_t)pbf_to_e7(blk, acc_lat, blk->pb->lat_offset);
                int32_t lo = (int32_t)pbf_to_e7(blk, acc_lon, blk->pb->lon_offset);
                b->lat[pos] = la; b->lon[pos] = lo; b->have[pos] = 1;
                bb_update(&b->st, la, lo);
            }
        }
        pbf_block_free(blk);
    }
    pbf_stream_close(s);
    return rc == PBF_ERR ? -1 : 0;
}

static void emit_ways(Builder *b)
{
    const int64_t *rp = (const int64_t *)b->refs.p;
    for (size_t i = 0; i < b->nwc; i++) {
        WayCand *wc = &b->wc[i];
        double slat = 0, slon = 0;
        int nfound = 0;
        for (uint32_t k = 0; k < wc->ref_cnt; k++) {
            size_t pos = lower_bound_i64(b->ids, b->n_ids, rp[wc->ref_off + k]);
            if (pos >= b->n_ids || !b->have[pos]) continue;
            slat += b->lat[pos];
            slon += b->lon[pos];
            nfound++;
        }
        if (nfound == 0) continue;                 /* 引用到的节点都不在这张图里 */
        const char *name = b->names.p ? b->names.p + wc->name_off : NULL;

        /* 路名：一条街在 OSM 里被切成几十个 way（路口、等级、限速都会切），
         * 全都收进来只会让搜索结果里重复出现同一条街。这里按名字去重，保留先遇到的
         * —— 同一等级的路取哪一段差别不大，但内存和时间省了两个数量级。 */
        /* 路名：一条街在 OSM 里被切成几十个 way（路口、等级、限速都会切），
         * 全都收进来只会让搜索结果里同一个名字出现几十次。
         *
         * ★ 但去重范围必须是"同城"而不是"全局同名"：解放路、建设路这种名字全国
         * 几百个城市都有，第一版按名字全局去重的结果是 —— 搜「长安街」给你报到
         * 河北（40.4,116.8）去，因为建库顺序上河北先遇到它；而真正的"长安街"
         * 那条路所在的城市一个点都不剩。
         * 所以键 = 名字指纹 ^ 1°×1° 格（约 110 km，一个城市的量级）。同一个城市里
         * 的几十段合并成一条，另一个城市里同名的照样留下。 */
        if (wc->kind == PLK_ROAD) {
            if (!name) continue;
            double rlat = slat / nfound / 1e7, rlon = slon / nfound / 1e7;
            uint64_t cell = (((uint64_t)(int)floor(rlat) & 0x3FFu) << 10) |
                            ( (uint64_t)(int)floor(rlon) & 0x3FFu);
            uint64_t rk = name_fnv64(name) ^ (cell * 0x9E3779B97F4A7C15ULL);
            if (keyset_seen(&b->roadnames, rk)) { b->st.road_dup++; continue; }
            b->st.road_rows++;
        }

        uint64_t key = ((uint64_t)wc->osm_id << 2) | 2u;   /* 2 = way */
        emit_row(b, &b->st, key, slat / nfound / 1e7, slon / nfound / 1e7,
                 wc->kind, wc->weight, name);
    }
}

static void clear_per_file(Builder *b)
{
    free(b->refs.p);  memset(&b->refs,  0, sizeof(b->refs));
    free(b->names.p); memset(&b->names, 0, sizeof(b->names));
    free(b->wc);      b->wc = NULL; b->nwc = b->capwc = 0;
    free(b->ids);  b->ids = NULL;
    free(b->lat);  b->lat = NULL;
    free(b->lon);  b->lon = NULL;
    free(b->have); b->have = NULL;
    b->n_ids = 0;
}

static double now_ms(void)
{
    return (double)nm_now_ms();       /* 平台层：clock_gettime / QPC */
}

static void acc_stats(BuildStats *tot, const BuildStats *st)
{
#define ACC(f) tot->f += st->f
    ACC(nodes_seen); ACC(nodes_tagged); ACC(ways_seen); ACC(ways_named);
    ACC(rows_place); ACC(rows_gram); ACC(dup_dropped); ACC(no_gram);
    ACC(name_too_long); ACC(way_too_big);
    ACC(blacklisted); ACC(numeric_name); ACC(dup_sameplace);
    ACC(road_rows); ACC(road_dup);
#undef ACC
    if (st->has_bb) {
        if (!tot->has_bb) { memcpy(tot->bb, st->bb, sizeof(tot->bb)); tot->has_bb = 1; }
        else {
            if (st->bb[0] < tot->bb[0]) tot->bb[0] = st->bb[0];
            if (st->bb[1] < tot->bb[1]) tot->bb[1] = st->bb[1];
            if (st->bb[2] > tot->bb[2]) tot->bb[2] = st->bb[2];
            if (st->bb[3] > tot->bb[3]) tot->bb[3] = st->bb[3];
        }
    }
}

static int build_one(Builder *b, const char *path, int verbose)
{
    memset(&b->st, 0, sizeof(b->st));

    double t0 = now_ms();
    if (pass1(b, path) != 0) { clear_per_file(b); return -1; }
    double t1 = now_ms();
    if (b->nwc > 0) {
        if (build_coord_index(b) != 0) { clear_per_file(b); return -1; }
        if (pass2(b, path) != 0)       { clear_per_file(b); return -1; }
    }
    double t2 = now_ms();
    emit_ways(b);
    double t3 = now_ms();
    b->st.blacklisted = g_blacklisted;
    g_blacklisted = 0;

    printf("source        %s\n", path);
    printf("  节点 %llu（带 tag %llu）  way %llu（候选 %llu，过大跳过 %llu）\n",
           (unsigned long long)b->st.nodes_seen,
           (unsigned long long)b->st.nodes_tagged,
           (unsigned long long)b->st.ways_seen,
           (unsigned long long)b->st.ways_named,
           (unsigned long long)b->st.way_too_big);
    printf("  写入 place %llu（其中路名 %llu，同名段丢 %llu）  gram %llu\n"
           "  重复（跨省 %llu / 同名同处 %llu）  过滤（黑名单 %llu / 纯编号 %llu / 单字无 gram %llu）\n",
           (unsigned long long)b->st.rows_place,
           (unsigned long long)b->st.road_rows,
           (unsigned long long)b->st.road_dup,
           (unsigned long long)b->st.rows_gram,
           (unsigned long long)b->st.dup_dropped,
           (unsigned long long)b->st.dup_sameplace,
           (unsigned long long)b->st.blacklisted,
           (unsigned long long)b->st.numeric_name,
           (unsigned long long)b->st.no_gram);
    if (verbose)
        printf("  去重键 %zu  坐标槽 %zu  耗时 读1 %.0f / 读2 %.0f / 写 %.0f ms\n",
               b->seen.n, b->n_ids, t1 - t0, t2 - t1, t3 - t2);

    acc_stats(&b->tot, &b->st);
    clear_per_file(b);
    return 0;
}
/* ------------------------------------------------------------------------
 * meta：给 sync_maps.sh 判"要不要重建"用的指纹，格式与旧的一致
 * ---------------------------------------------------------------------- */
static int write_meta(const char *meta_path, const char *db_path,
                      char **srcs, int nsrc)
{
    FILE *f = fopen(meta_path, "w");
    if (!f) { fprintf(stderr, "写不了 %s\n", meta_path); return -1; }
    fprintf(f, "# NavMap 自动地名库（skedaddle_places_build 生成）\n");
    fprintf(f, "db           = %s\n", db_path);
    fprintf(f, "sources      = %d\n", nsrc);
    for (int i = 0; i < nsrc; i++) {
        FILE *fp = fopen(srcs[i], "rb");
        long bytes = 0;
        long mtime = 0;
        if (fp) {
            fseek(fp, 0, SEEK_END);
            bytes = ftell(fp);
            fclose(fp);
        }
        {
            int64_t mt = 0;
            if (nm_stat_file(srcs[i], NULL, &mt) == 0) mtime = (long)mt;
        }
        fprintf(f, "source[%d]    = %s\n", i, srcs[i]);
        fprintf(f, "src_bytes[%d] = %ld\n", i, bytes);
        fprintf(f, "src_mtime[%d] = %ld\n", i, mtime);
    }
    fclose(f);
    return 0;
}

/* ------------------------------------------------------------------------
 * selftest：库本身能不能被查
 *   —— 每条地名的 bigram 必须都能在 gram 表里找到；这是"索引写歪了"的唯一
 *      自查手段。没有这一步，bigram 口径分叉会表现为"某些词莫名其妙搜不到"。
 * ---------------------------------------------------------------------- */
static int selftest(const char *db_path)
{
    sqlite3 *db = NULL;
    int fails = 0;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        printf("ASSERT-FAIL    打不开 %s\n", db_path);
        return 1;
    }
    sqlite3_stmt *one = NULL;
    sqlite3_prepare_v2(db,
        "SELECT 1 FROM gram WHERE gram=?1 AND place_id=?2", -1, &one, NULL);
    sqlite3_stmt *all = NULL;
    sqlite3_prepare_v2(db, "SELECT id,name,lat,lon,kind,weight FROM place", -1, &all, NULL);

    uint64_t rows = 0, checked = 0, bad_ll = 0, bad_kind = 0, no_idx = 0;
    while (sqlite3_step(all) == SQLITE_ROW) {
        int64_t id = sqlite3_column_int64(all, 0);
        const unsigned char *nm = sqlite3_column_text(all, 1);
        double lat = sqlite3_column_double(all, 2), lon = sqlite3_column_double(all, 3);
        int kind = sqlite3_column_int(all, 4), weight = sqlite3_column_int(all, 5);
        rows++;
        if (!nm || !*nm) { printf("ASSERT-FAIL    id %lld 名字为空\n", (long long)id); fails++; continue; }
        if (lat < -90 || lat > 90 || lon < -180 || lon > 180) bad_ll++;
        /* 上限写成 PLK_ROAD 而不是 12：标注层的上限才是 12（labels.c 的
         * kind_max_for_zoom），kind 13 是"只进搜索"的路名，合法。上次加完路名
         * 就是在这条上吃了一记 ASSERT-FAIL —— 判据必须跟着 kind 表一起走。 */
        if (kind < 1 || kind > PLK_ROAD) bad_kind++;
        if (weight <= 0) bad_kind++;

        if (rows % 7 != 0 && rows % 13 != 0) continue;   /* 抽样约 20%，其余靠总数兜底 */
        char grams[32][16];
        int nchar = 0;
        int ng = search_bigrams((const char *)nm, grams, 32, &nchar);
        checked++;
        if (nchar < 2) continue;                          /* 单字没有 bigram，允许 */
        for (int i = 0; i < ng; i++) {
            sqlite3_reset(one);
            sqlite3_bind_text(one, 1, grams[i], -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(one, 2, id);
            if (sqlite3_step(one) != SQLITE_ROW) {
                printf("ASSERT-FAIL    「%s」(id %lld) 缺 gram「%s」\n",
                       (const char *)nm, (long long)id, grams[i]);
                no_idx++;
                fails++;
                break;
            }
        }
    }
    sqlite3_finalize(all);
    sqlite3_finalize(one);
    sqlite3_close(db);

    if (rows == 0)                { printf("ASSERT-FAIL    库里一条地名都没有\n"); fails++; }
    if (bad_ll)                   { printf("ASSERT-FAIL    %llu 条坐标越界\n", (unsigned long long)bad_ll); fails++; }
    if (bad_kind)                 { printf("ASSERT-FAIL    %llu 条 kind/weight 非法\n", (unsigned long long)bad_kind); fails++; }
    printf("自检           %llu 条地名，抽查 %llu 条 bigram，漏索引 %llu\n",
           (unsigned long long)rows, (unsigned long long)checked,
           (unsigned long long)no_idx);
    printf(fails ? "PLACES-FAIL  %d 条断言失败\n" : "PLACES-OK\n", fails);
    return fails ? 1 : 0;
}

/* ------------------------------------------------------------------------ */

static int ends_with(const char *s, const char *suf)
{
    size_t n = strlen(s), m = strlen(suf);
    return n >= m && strcmp(s + n - m, suf) == 0;
}

/* 文件名排序（替代旧版 scandir 的 alphasort）：目录枚举顺序由文件系统决定，
   构建顺序必须可复现。 */
static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}


int main(int argc, char **argv)
{
    const char *db_path   = NULL;
    const char *src_dir   = NULL;
    const char *one_pbf   = NULL;
    const char *meta_path = NULL;
    int verbose = 0, do_selftest = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strncmp(a, "--db=", 5))        db_path   = a + 5;
        else if (!strncmp(a, "--sources=", 10)) src_dir = a + 10;
        else if (!strncmp(a, "--pbf=", 6))  one_pbf   = a + 6;
        else if (!strncmp(a, "--meta=", 7)) meta_path = a + 7;
        else if (!strcmp(a, "--verbose"))   verbose   = 1;
        else if (!strcmp(a, "--selftest"))  do_selftest = 1;
        else { fprintf(stderr, "未知参数 %s\n", a); return 2; }
    }

    if (do_selftest) {
        if (!db_path) { fprintf(stderr, "--selftest 需要 --db=\n"); return 2; }
        return selftest(db_path);
    }
    if (!db_path || (!src_dir && !one_pbf)) {
        fprintf(stderr,
            "用法: %s --db=<out.db> (--sources=<目录> | --pbf=<file.osm.pbf>)"
            " [--meta=<file>] [--verbose]\n"
            "      %s --db=<已建好的库> --selftest\n", argv[0], argv[0]);
        return 2;
    }

    char **srcs = NULL;
    int nsrc = 0;
    if (one_pbf) {
        srcs = calloc(1, sizeof(char *));
        srcs[0] = (char *)one_pbf;
        nsrc = 1;
    } else {
        /* 目录枚举 + 排序：顺序必须可复现（旧版用 scandir+alphasort，
           平台层只保证"枚举到"，排序自己来）。 */
        NmDir *d = nm_dir_open(src_dir);
        if (!d) { fprintf(stderr, "读不了目录 %s\n", src_dir); return 2; }
        char **names = NULL;
        int nn = 0, cap = 0;
        const char *de;
        while ((de = nm_dir_next(d)) != NULL) {
            if (!ends_with(de, ".osm.pbf")) continue;
            if (nn == cap) {
                cap = cap ? cap * 2 : 16;
                char **t = realloc(names, (size_t)cap * sizeof(char *));
                if (!t) break;
                names = t;
            }
            names[nn++] = nm_strdup(de);
        }
        nm_dir_close(d);
        qsort(names, (size_t)nn, sizeof(char *), cmp_str);
        srcs = calloc((size_t)(nn > 0 ? nn : 1), sizeof(char *));
        for (int i = 0; i < nn; i++) {
            char p[4096];
            if (nm_path_join(p, sizeof(p), src_dir, names[i]) != 0) continue;
            srcs[nsrc++] = nm_strdup(p);
            free(names[i]);
        }
        free(names);
        if (nsrc == 0) { fprintf(stderr, "%s 下没有 *.osm.pbf\n", src_dir); return 2; }
    }

    Builder B;
    memset(&B, 0, sizeof(B));
    if (keyset_init(&B.seen, 22) != 0)      { fprintf(stderr, "内存不足\n"); return 1; }
    if (keyset_init(&B.sameplace, 22) != 0) { fprintf(stderr, "内存不足\n"); return 1; }
    if (keyset_init(&B.roadnames, 21) != 0) { fprintf(stderr, "内存不足\n"); return 1; }
    if (sink_open(&B.sink, db_path) != 0) return 1;

    double t0 = now_ms();
    for (int i = 0; i < nsrc; i++) {
        if (build_one(&B, srcs[i], verbose) != 0) {
            fprintf(stderr, "%s 处理失败\n", srcs[i]);
            sink_close(&B.sink);
            return 1;
        }
    }
    B.tot.ms_build = now_ms() - t0;
    if (sink_finish(&B.sink) != 0) return 1;
    B.tot.ms_finish = now_ms() - t0 - B.tot.ms_build;

    printf("\n合计\n");
    printf("  place %llu  gram %llu  重复 %llu  超长截断 %llu\n",
           (unsigned long long)B.tot.rows_place,
           (unsigned long long)B.tot.rows_gram,
           (unsigned long long)B.tot.dup_dropped,
           (unsigned long long)B.tot.name_too_long);
    printf("  bbox  %.4f,%.4f .. %.4f,%.4f\n",
           B.tot.bb[0] / 1e7, B.tot.bb[1] / 1e7, B.tot.bb[2] / 1e7, B.tot.bb[3] / 1e7);
    printf("  耗时  %.1f s（建索引收尾 %.1f s）\n",
           (B.tot.ms_build + B.tot.ms_finish) / 1000, B.tot.ms_finish / 1000);
    printf("db            %s\n", db_path);

    int rc = 0;
    if (meta_path && write_meta(meta_path, db_path, srcs, nsrc) != 0) rc = 1;

    sink_close(&B.sink);
    free(B.seen.slot);
    free(B.sameplace.slot);
    free(B.roadnames.slot);
    for (int i = 0; i < nsrc; i++) if (one_pbf == NULL) free(srcs[i]);
    free(srcs);
    return rc;
}
