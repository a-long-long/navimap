#ifndef NAVIMAP_QT_ICONS_H
#define NAVIMAP_QT_ICONS_H

#include <QIcon>
#include <QPixmap>
#include <QPainter>
#include <Qt>

/* 用 QPainter 自绘的界面图标（不依赖任何字体）。
 *
 * ★ 为什么不用字体符号（＋ ◎ ☰ 🔍 …）：
 *   实测（Qt offscreen 平台插件）：QFontMetrics::inFont() 对这些符号全部
 *   返回 false，且 advanceWidth 全都相同（fallback 默认宽度）⇒ 它们在界面上
 *   就是**豆腐块**。自绘矢量图不依赖字体，任何环境都一致，且缩放清晰。
 */
namespace Qt {
enum class IconKind {
    Search,     /* 放大镜 */
    ZoomIn,     /* 放大镜 + 加号 */
    ZoomOut,    /* 放大镜 + 横杠 */
    Locate,     /* 十字准星 */
    DayNight,   /* 半圆（日/夜） */
    Grid,       /* 网格（坐标网） */
    Menu,       /* 三横线 */
    Plus,       /* 加号 */
    Track,      /* 轨迹：折线 + 起终点 */
    Via,        /* 途经点：图钉标记（路线依次经过的中间点） */
};
}

/* 生成一个逻辑尺寸 logicalSize 的图标位图（内部按 2x 绘制再缩放）。 */
QPixmap qt_make_icon(Qt::IconKind kind, int logicalSize = 20);

/* ★ 直接在调用者的 QPainter 上画图标（**推荐**）。
 *   坐标是纯逻辑坐标，不经过 pixmap ⇒ 没有 devicePixelRatio 的歧义。
 *   size：图标边长（像素）。
 *
 *   为什么推荐这个：drawPixmap 的各个重载在 DPR≠1 时对"源矩形"的推断
 *   各不相同，实测在 Qt offscreen 插件下图标只被画到目标矩形的左上角。 */
void qt_draw_icon(QPainter &p, Qt::IconKind kind, double size);

#endif
