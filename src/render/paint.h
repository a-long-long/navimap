#ifndef NAVIMAP_PAINT_H
#define NAVIMAP_PAINT_H

#include <stdint.h>
#include "map/map_view.h"
#include "map/binfmt.h"
#include "map/namepool.h"   /* NamePool：路名池，已从本头下沉到 map 层（PLAN.md §53） */
#include "render/raster.h"
#include "render/font.h"   /* Font / font_draw：等高线刻度标注用到 */

/* 软件渲染的路网绘制。
 *
 * 取舍：v1 不做空间索引，每帧扫一遍全部节点/边做视口裁剪。北京 145 万节点、
 * 284 万边，扫一遍大约几毫秒，够用；等扩到 8 省（5400 万节点）再上网格索引。
 * 那时这个函数的签名不用变，只替换内部遍历。
 */

/* 面的屏幕外接框最大边小于这个值时，走"只点一个像素"的快速路径，
 * 不再做扫描线填充 + 描边。见 paint_areas 里的实测分布（z9 有 70% 的面
 * 小于 2 px，是大头）。调大更省时间但会让细小水面/绿地消失，调小则相反。 */
#define AREA_DOT_PX 2

typedef struct {
    uint64_t visited;     /* 扫过的边数 */
    uint64_t drawn;       /* 真正画的边数 */
    int      max_class;   /* 本帧画到哪一级（LOD） */
    uint64_t areas_drawn; /* 面要素画出数 */
    uint64_t areas_dotted;/* 其中走了"小面画点"快速路径的数量 */
    uint64_t lines_drawn; /* 线要素画出数 */
    uint64_t labels_drawn;     /* 地名标注数 */
    uint64_t poi_icons;        /* POI / 地名矢量图标标记数 */
    uint64_t road_labels_drawn;/* 路名标注数 */
    uint64_t dedup_skipped;    /* 因跨省重复而跳过的要素数 */

    /* 分阶段耗时（毫秒）。加它的原因：帧时间一直下不来，而"投影贵"只是猜——
       实测一帧里投影只占个位数毫秒，大头全在 SDL 的画线/填充调用次数上。
       没有这个分解就会去优化根本不是瓶颈的地方（比如做投影缓存）。 */
    double ms_bg, ms_areas, ms_roads, ms_lines, ms_labels;
} PaintStats;

/* ---------------------------------------------------------------------------
 * 跨省去重集合
 *
 * 背景：各省 pbf 是按多边形裁出来的，地理上的飞地会被邻省整块包进去——
 * 实测北京的节点 id 100% 同时存在于河北、天津也是（见 docs 里的调研记录）。
 * 结果是同一条路/同一个建筑被连画两遍：z16 北京城区的 edges/areas/lines
 * 全部精确 2.0 倍，帧时间 269ms → 681ms，而渲染出的 BMP 字节完全相同。
 *
 * 身份键（都是 OSM 原始 id，天然跨文件一致）：
 *   路 EF：两端节点 osm_id 的有序对 {from, to}（保留方向，否则双向道路
 *          的回程边会被误判成重复而整条消失——oneway 处理上踩过同类坑）
 *   线/面：记录自带的 OSM way id（lines.bin 的 LF_OSM_ID、areas.bin 的
 *          AF_OSM_ID，实测 8 省数据里没有一条为 0）
 *
 * 开放寻址 + 世代标记：换帧只 ++gen，不用 memset 整表。
 * 只有多个数据源同时盖住视口时才启用（单省加载走 NULL 快路径，零开销）。
 *
 * ★ 槽里只存 64 位指纹，不存原始 128 位键。原因：8 省 z9 一帧要做 115 万次
 *   探表，实测占路网耗时 1/3（PLAN.md §22）；原来 hi/lo/gen 是三个平行数组
 *   （2M 槽时 16+16+8 MB），一次探表最多摸 3 条 cache line。合成一个 16 字节
 *   的槽后每次探表只摸 1 条线。代价是退化成"指纹相同即判重"：
 *   115 万键的碰撞概率 ≈ N²/2^64 ≈ 3.6e-8/帧，中一次也只是少画一段 1 像素的
 *   要素、且下一帧就恢复——比每帧多花 100 ms 划算得多。
 *
 * ★ 面/线要素**不**走这张表：它们的填充和描边都是不透明色，同一条要素被两个
 *   省各画一遍落在同样的像素上，去重只是白花哈希钱——实测 8 省 z9 关掉面/线的
 *   去重省 22 ms 且逐像素 0 差异（PLAN.md §23）。只有路网需要它，因为同一条
 *   路在两省里的**分级可能不同**，不判重会出现"谁后画谁说了算"的花色。
 * ------------------------------------------------------------------------- */
#define DD_TAG_LINE 0x8000000000000000ULL   /* OSM id 远小于 2^40，不会撞 */
#define DD_TAG_AREA 0xA000000000000000ULL

typedef struct {
    uint64_t key;        /* 64 位指纹（dd_mix 混过） */
    uint32_t gen;        /* 世代：!= cur 视为空槽 */
    uint32_t pad;        /* 凑 16 字节，一槽正好一条 cache line */
} DdSlot;

typedef struct {
    DdSlot  *slot;
    size_t   cap, mask, used;
    uint32_t cur;
} DedupSet;

void dedup_init(DedupSet *d);
void dedup_free(DedupSet *d);
void dedup_begin(DedupSet *d);            /* 每帧开头调用一次 */
/* 返回 1 = 这个键本帧已经登记过（调用方应跳过绘制）；0 = 首次，已登记 */
int  dedup_seen(DedupSet *d, uint64_t hi, uint64_t lo);

/* 路名池（NamePool）的定义与实现已搬到 map/namepool.h —— 它是纯地图数据概念
 * （每省 names.bin 的解析结果，随 MapSrc 走），放在 render 会让 map/mapset.h
 * 反向依赖 render ⇒ 模块级倒置。施工记录见 PLAN.md §53。
 * 本头仍用它（RoadLabel 持有 const NamePool *），故引 map 的头（render→map 是
 * 既有正确方向）。 */

/* 路名收集：paint_roads 遍历边时顺带记录"屏幕内、有名、主干以上"的边中点，
 * 供 labels.c 的 paint_road_labels 去重绘制。np 是该省的路名池——
 * name_id 只在本省 names.bin 内有意义，多省时必须各自带池。
 * reset 由调用方在每帧开头做一次（多省循环时各省依次追加，不能各省 reset）。 */
typedef struct { const NamePool *np; uint32_t name_id; int x, y; } RoadLabel;
void road_labels_reset(void);
void road_labels_add(const NamePool *np, uint32_t name_id, int x, int y);
void road_labels_get(const RoadLabel **out, int *n);

void paint_background(Raster *rs, int w, int h);

/* 画路网。max_class = 0..4，超过这个等级的路不画（LOD）。
 * 5-7（步行道/铁路/水系线）走 paint_lines，不在这里。
 * tiles/edgelist 是空间索引（graph_extract 生成），非 NULL 时按 tile
 * 只遍历视口覆盖的部分；任一为 NULL 回退全量扫描（旧目录兼容）。
 * np 是该省路名池（供顺带收集路名标签；可为 NULL，只是不收集）。
 *
 * peer_tiles / n_peer：图集里**所有**省（含本省自己）的 tiles.bin，只用来判
 * "这一格瓦片有几个省有数据"。≥2 个省才可能有跨省重复，才需要查去重表
 * （8 省 z9 一帧 55 万条里只有 3 万条落在多省格子里，其余 95% 的探表是白付的，
 * 实测门控后省 70 ms）。传 NULL/0 则退回"一律去重"的旧行为。 */
/* ★ rs 是"直接写像素"的目标（见 raster.h）—— 现在整条绘制管线都走它，
 * 不再依赖 SDL_Renderer（核心层已彻底零 SDL；SDL 只在 blend_equiv /
 * clip_equiv 两个门禁对照体里合法出现）。
 * rs 为 NULL 时这几个函数什么都不画（调用方必须先 raster_bind_mem 成功）。 */
void paint_roadpix_begin(void);
void paint_roads(Raster *rs, const MapView *v,
                 const NMBinFile *nodes, const NMBinFile *edges,
                 const NMBinFile *tiles, const NMBinFile *edgelist,
                 const NamePool *np, DedupSet *dd,
                 int max_class, PaintStats *st,
                 const NMBinFile *const *peer_tiles, int n_peer);

/* 面要素（建筑/水面/绿地）。bbox_cache 是调用方持有的每面 bbox 缓存
 * （na*4 个 i32，e7 度），首次传 NULL 会就地分配并填充。
 * min_zoom_area/green 与建筑分开控制：建筑只在 zoom>=13 出现。 */
int  paint_areas(Raster *rs, const MapView *v,
                 const NMBinFile *nodes, const NMBinFile *areas,
                 const NMBinFile *polyrefs,
                 int32_t **bbox_cache, PaintStats *st);

/* 线要素（步行道/铁路/水系线）。
 * bbox_cache 与面要素同机制：每条线的经纬度包围盒（nl*4 个 i32，e7 度），
 * 首次传 NULL 就地分配填充。
 * ★ 这个缓存不是"锦上添花"：线要素没有 tile 索引，每帧要扫全部 10 万条线；
 *   没有 bbox 预筛就得把每条线的**全部顶点**（上限 4096）都投影一遍才能判断
 *   它可不可见——实测 z14 一帧 248 ms 里的绝大部分就是这些白投影
 *   （占了整帧 331 ms 的 75%）。 */
int  paint_lines(Raster *rs, const MapView *v,
                 const NMBinFile *nodes, const NMBinFile *lines,
                 const NMBinFile *polyrefs,
                 int32_t **bbox_cache, PaintStats *st);

/* 等高线（SRTM 生成，contours/polyrefs/pts 三件套）。bbox_cache 与线要素同机制
 * （nc*4 个 i32，e7 度），首次传 NULL 就地分配填充。LOD：zoom>=9 才画，
 * 其中首曲线（class=0，密集）要 zoom>=11 才出现，计曲线（class=1）全程可见。 */
int  paint_contours(Raster *rs, const MapView *v,
                   const NMBinFile *contours, const NMBinFile *polyrefs,
                   const NMBinFile *pts,
                   int32_t **bbox_cache, PaintStats *st, Font *font);

/* 在屏幕上标一个点（当前位置/选点） */
void paint_marker(Raster *rs, int x, int y, int radius,
                  uint8_t rr, uint8_t gg, uint8_t bb);

/* 粗线：raster 一次只画 1 像素宽。竖向偏移画 n 条近似加粗，
   近垂直的线再补横向偏移（否则斜线的粗细看着不对）。路网和路线叠加都用它。 */
void paint_thick_line(Raster *rs, int x0, int y0, int x1, int y1, int width, uint32_t col);

#endif
