#ifndef NAVIMAP_CHROME_H
#define NAVIMAP_CHROME_H

#include "ui/window.h"
#include "render/raster.h"

/* 自绘标题栏高度（window.c 与 chrome.c 共用，改了要同步） */
#define CHROME_TITLE_H 30

/* ---------------------------------------------------------------------------
 * nav-desktop 风格界面层（11 类元素）
 *
 * 全部只做"绘制 + 命中测试"；动作（改 App 状态 / 调 app_*）由 window.c 派发。
 * 绘制顺序：paint_frame 先把地图画完，最后调 paint_chrome 把这套 chrome
 * 叠在最上层（与 nav-desktop 的覆盖式布局一致，地图铺满窗口、chrome 浮在上面）。
 *
 * 11 类元素：
 *   1. WinCtl      自绘标题栏（标题 + 最小化/最大化/关闭，可拖拽移动窗口）
 *   2. TopBar      顶栏（菜单按钮 + 搜索框 + 浏览/导航 模式页签）
 *   3. RightCol    右栏（放大/缩小/定位/昼夜 四个圆形按钮）
 *   4. NavTop      导航顶栏（返回 + 目的地 + 退出，仅算路后显示）
 *   5. NavBottom   导航底栏（重新规划中指示，仅偏航重算时显示）
 *   6. StepsPanel  步骤面板（转向指令列表，左侧）
 *   7. MainMenu    主菜单浮层（点菜单按钮展开）
 *   8. NoMapCard   无图卡片（图集未加载时居中提示）
 *   9. DayNightCtl 昼夜切换（RightCol 的昼夜按钮 + 夜间半透明蒙版）
 *  10. ScaleBar    比例尺（左下角）
 *  11. Compass     指北针（地图旋转时出现在右上角，点一下回正北）
 * ------------------------------------------------------------------------- */

typedef enum {
    CH_NONE = 0,
    CH_WIN_MIN, CH_WIN_MAX, CH_WIN_CLOSE,
    CH_MENU_OPEN,
    CH_MODE_BROWSE, CH_MODE_NAV,
    CH_SEARCH,
    CH_ZOOM_IN, CH_ZOOM_OUT, CH_LOCATE, CH_DAYNIGHT,
    CH_NAV_BACK, CH_NAV_EXIT,
    CH_COMPASS,
    CH_MENU_ITEM,           /* 具体项下标在 a->chrome_menu_item */
    CH_SETTINGS             /* 设置面板内命中（行/按钮在 a->settings_row/btn） */
} ChromeID;

/* 画全部 chrome（在地图画完之后调用） */
void paint_chrome(Raster *rs, App *a);

/* (x,y) 命中了哪个元素；若是菜单项，把项下标写进 a->chrome_menu_item */
int  chrome_hit(App *a, int x, int y);

/* 执行一次对 chrome 元素的点击（由 window.c 的鼠标 UP 派发）。
   动作里需要改 App 状态 / 调 app_*，都在这一个地方集中处理。 */
void chrome_act(App *a, int id, int x, int y);

/* 主菜单条目（window.c 的动作派发与这里保持一致，改了要同步） */
int         chrome_menu_count(void);
const char *chrome_menu_label(int i);

/* 夜间半透明蒙版：在地图画完后、面板/文字之前调用，只压暗地图、不压 chrome */
void paint_night_tint(Raster *rs, int w, int h);

#endif
