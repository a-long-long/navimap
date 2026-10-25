#ifndef NAVIMAP_NAVDRAW_H
#define NAVIMAP_NAVDRAW_H

#include <stdint.h>
#include "map/map_view.h"
#include "route/route.h"
#include "route/maneuver.h"
#include "search/search.h"
#include "render/font.h"
#include "render/raster.h"

/* ---------------------------------------------------------------------------
 * 导航叠加层（阶段 9）
 *
 * 只做"画"，不做任何算路 / 状态管理（那在 ui/window.c）。
 * 路线配色：白描边 + 蓝核心。为什么是蓝：路面本身是橙/黄/灰，红色会和
 * 高速的橙红（214,94,40）糊在一起，蓝色在任何底色上都跳出来。
 * ------------------------------------------------------------------------- */

#define NAV_ROUTE_R  30
#define NAV_ROUTE_G 110
#define NAV_ROUTE_B 235

/* 画路线折线。返回真正画出的段数（视口外的段整段跳过——长途路线几万段，
   全画一遍没意义）。width<=0 时按缩放自动选（远了细一点）。
 * done_seg / done_t = 车头走到哪了（模拟导航）：
 *   done_seg < 0  → 整条蓝色（还没开始 / 没有模拟）
 *   done_seg >= 0 → [0, done_seg) 画灰（已走过），done_seg 段按 done_t 切成
 *                   灰/蓝两截，其后全蓝。没有它的话开导航后看不出走到哪。 */
int  paint_route(Raster *rs, const MapView *v,
                 const int32_t *lat_e7, const int32_t *lon_e7, uint32_t n,
                 int width, int done_seg, double done_t);

/* 车头：按航向旋转的三角箭头（蓝芯白边），压在路线上指示"我在哪、朝哪走"。 */
void paint_nav_car(Raster *rs, const MapView *v,
                   double lat, double lon, double heading_deg);

/* 底部导航条（模拟导航时显示）。turn 用大字号，其余小字号。
   传 NULL 或空串的字段自动省略，调用方不用拼字符串。 */
typedef struct {
    const char *turn;     /* "右转" / "环岛第3出口" / "直行" */
    const char *dist;     /* "300 米后" */
    const char *road;     /* "进入建国路"（可 NULL） */
    const char *remain;   /* "剩余 12.4 公里" */
    const char *eta;      /* "约 18 分钟" */
    /* 路口车道指示（可选）：turn:lanes 串，'|' 分车道（如 "left|through|right"）。
     * turn_dir 指明"这条要走的转向"用于高亮哪几格：
     *   0=直行 -1=左转(含靠左/掉头) +1=右转(含靠右) -3=不高亮（环岛/到达）。 */
    const char *lanes;
    int         turn_dir;
} NavBanner;

void paint_nav_banner(Raster *rs, const MapView *v, Font *f, Font *fbig,
                      int line_h, int line_h_big, const NavBanner *b);

/* 顶部横幅："这里没有地图数据" + 去哪下载、下载后怎么办。
 * cov_state 取自 mapset_coverage（COV_NONE / COV_BBOX_ONLY 才值得画）。
 * f 为 NULL 时（--font=none）只画色条，不画字——提示不能因为没字体就消失。 */
void paint_nomap_banner(Raster *rs, const MapView *v, Font *f, int line_h,
                        int cov_state);

/* 右下角缩放按钮。返回 0/1 表示 (px,py) 是否落在按钮里，供窗口命中测试；
   画和判定用同一份坐标，避免"画在这、点在那"。 */
void paint_zoom_buttons(Raster *rs, const MapView *v, Font *f);
int  zoom_button_hit(const MapView *v, int px, int py);   /* 1=放大 -1=缩小 0=没点中 */

/* ---- 搜索框（阶段 7）----
 * 顶栏居中输入框 + 下方结果列表。editing 是输入法组合中的串（只显示，
 * 还没提交），用另一种颜色画出来，否则中文输入时看起来像"没反应"。
 * 结果行右侧显示距离（dist_m >= 0 时）。 */
typedef struct {
    int      active;
    const char *text;
    const char *editing;
    const SearchHit *hits;
    int      n_hits;      /* 总条数 */
    int      sel;         /* 选中行下标 */
    int      show_max;    /* 最多画几行（0 = 用默认 8） */
    int      elapsed_ms;  /* 仅显示 */
} SearchBoxView;

void paint_search_box(Raster *rs, const MapView *v, Font *f, Font *fbig,
                      int line_h, int line_h_big, const SearchBoxView *sb);

/* 搜索结果在图上的钉子：白底方块 + 蓝芯（选中）或灰芯（未选中） */
void paint_search_pin(Raster *rs, const MapView *v,
                      double lat, double lon, int selected, Font *f,
                      const char *label);

/* 起点/终点标记：白底方块 + 彩色芯 + 右侧文字（有字体时）。
   is_start = 1 绿（起点），0 红（终点）。 */
void paint_nav_marker(Raster *rs, const MapView *v,
                      double lat, double lon, int is_start, Font *f);

/* 左上角导航面板：里程/预计时间 + 前几条转向指令。
   status 为顶行文案（"计算中…" / "无可行路线" 等，NULL 时用里程行）。
   line_h 由调用方给（字号 + 行距），因为 Font 不暴露字号。 */
void paint_nav_panel(Raster *rs, const RouteResult *rr,
                     const ManeuverList *ml, Font *f, int line_h,
                     int x, int y, int max_lines, const char *status);

#endif
