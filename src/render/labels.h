#ifndef NAVIMAP_LABELS_H
#define NAVIMAP_LABELS_H

#include <sqlite3.h>
#include "map/map_view.h"
#include "render/font.h"
#include "map/binfmt.h"
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

/* 路名池：每省目录一份 names.bin，id 只在本省内有意义，所以做成可实例化的，
 * 由 MapSrc 持有；不再用全局单例。NamePool 定义见 paint.h。 */
int  name_pool_open(NamePool *np, const char *names_path);
void name_pool_close(NamePool *np);
void paint_road_labels(Raster *rs, const MapView *v, Font *font,
                       PaintStats *st);

#endif
