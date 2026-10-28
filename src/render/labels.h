#ifndef NAVIMAP_LABELS_H
#define NAVIMAP_LABELS_H

#include <sqlite3.h>
#include "map/map_view.h"
#include "render/font.h"
#include "map/binfmt.h"
#include "map/namepool.h"   /* NamePool / name_pool_open·close：已从本头下沉到 map 层 */
#include "render/paint.h"

/* 地名标注：从 places.db（place 表）按视口查询，网格避让后画。
 * 查询结果缓存在 LabelCache 里，视口变化超阈值才重新查 SQL，
 * 平移缩放时只重投影 + 重避让。 */

typedef struct {
    char   name[64];
    double lat, lon;
    int    kind, weight;
} LabelItem;

typedef struct {
    sqlite3   *db;
    LabelItem *items;
    int        n_items, cap_items;
    double     cached_bb[4];
    int        cached_kindmax;
    int        loaded;
} LabelCache;

int  label_cache_init(LabelCache *lc, const char *db_path);
void label_cache_free(LabelCache *lc);
/* show_poi / show_labels：分别控制"矢量图标"与"文字标签"是否绘制
 * （受 Qt『设置』菜单里的 POI 图标 / 地名标注 开关控制）。二者独立，共用一个避让格。 */
void paint_place_labels(Raster *rs, const MapView *v,
                        LabelCache *lc, Font *font, PaintStats *st,
                        int show_poi, int show_labels);

/* 反查关注点最近的上级行政区（市级 / 区县级），供"无图区提示"回答"这是哪儿"。
 *
 * 为什么放在这里而不是 map/coverage.c：地名库由 LabelCache 持有（render 层），
 * 而 map 层不能反向依赖 render ⇒ 省一级在 coverage_locate()、市/区在这里，
 * 由 UI 层把两者拼成一条 CovPlace。
 *
 * 分层取值按 places_build.c 的 kind 语义（数值小 = 行政级别高）：
 *   市级   kind <= 2（1=省会/直辖市城区，2=地级市）
 *   区县级 kind == 3（县/镇）
 * 输出为"最近的"同名点：地名库里没有省界多边形，只能按最近点近似，故可能
 * 在省界附近报成邻省的市——调用方应把它当作"大致位置"，不要当行政归属依据。
 * 返回填了几项（0/1/2）；lc 未打开时返回 0 且两个缓冲置空。 */
int  label_nearest_admin(LabelCache *lc, double lat, double lon,
                         char *city, size_t ccap,
                         char *district, size_t dcap);

/* 路名池：每省目录一份 names.bin，id 只在本省内有意义，所以做成可实例化的，
 * 由 MapSrc 持有；不再用全局单例。
 * ★ NamePool 与 name_pool_open/close 已搬到 map/namepool.h（PLAN.md §53 R1）——
 * 它们是纯地图数据概念，放在 render 会让 map 反向依赖 render。 */
void paint_road_labels(Raster *rs, const MapView *v, Font *font,
                       PaintStats *st);

#endif
