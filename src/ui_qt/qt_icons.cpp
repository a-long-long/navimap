/* qt_icons.cpp —— 用 QPainter **自绘**界面图标。
 *
 * ★ 为什么不用字体符号（＋ ◎ ☰ 🔍 …）：
 *   那些符号在字体链里的覆盖**不可靠**，实测（Qt offscreen 平台插件下）：
 *     QFontMetrics::inFont() 对 ＋ － ◎ ◐ ▦ ☰ 🔍 全部返回 **false**，
 *     且 advanceWidth **全都是同一个值**（fallback 的默认宽度）——
 *     既说明"没有字形"，也说明度量本身在该插件下不可信。
 *   于是这些符号在界面上就是**豆腐块**（方框）。
 *   （本项目刚修过同一个病：Qt 控件的中文变方框。根因类似：字体没就位。）
 *
 *   自绘矢量图标的优点：
 *     · 不依赖任何字体 ⇒ 任何机器、任何字体环境都一致；
 *     · 任意缩放都清晰（字体符号在小尺寸下会糊）；
 *     · 能精确控制线宽/颜色，和地图风格统一。
 *
 * 全部图标用 **深灰 (#2c3440)**，在浅色地图上对比清晰。
 */
#include "qt_icons.h"

#include <QPainter>
#include <QPixmap>
#include <QPen>
#include <QBrush>
#include <QColor>
#include <QPainterPath>

namespace {

/* 图标线宽（像素，按 2x 画再缩，边缘更顺） */
const int kScale = 2;
const int kBase = 20;          /* 逻辑尺寸 */
const QColor kInk(0x2c, 0x34, 0x40);

QPen mkPen(double w)
{
    QPen p(kInk);
    p.setWidthF(w);
    p.setCapStyle(Qt::RoundCap);
    p.setJoinStyle(Qt::RoundJoin);
    return p;
}

/* 画放大镜（plus=false 时镜内是横杠，即"缩小"）
 * 镜片占画布的大头（r=6.4），手柄到角 —— 20px 的画布上放大镜太大会挤，
 * 太小则看不清（实测 r=5 时几乎看不出是个放大镜）。 */
void drawMagnifier(QPainter &p, bool plus)
{
    const double r = 6.4;                 /* 镜片半径 */
    const double cx = 7.4, cy = 7.4;
    p.setPen(mkPen(1.8));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QRectF(cx - r, cy - r, r * 2, r * 2));
    /* 手柄：45° 斜线到画布右下角 */
    p.drawLine(QPointF(cx + r * 0.70, cy + r * 0.70),
               QPointF(cx + r * 0.70 + 4.6, cy + r * 0.70 + 4.6));
    if (plus) {
        p.drawLine(QPointF(cx - 3.0, cy), QPointF(cx + 3.0, cy));
        p.drawLine(QPointF(cx, cy - 3.0), QPointF(cx, cy + 3.0));
    } else {
        p.drawLine(QPointF(cx - 3.0, cy), QPointF(cx + 3.0, cy));
    }
}

/* 十字准星（定位） */
void drawLocate(QPainter &p)
{
    const double cx = 10.0, cy = 10.0, r = 5.4;
    p.setPen(mkPen(1.7));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QRectF(cx - r, cy - r, r * 2, r * 2));
    /* 中心实心点 */
    p.setBrush(QBrush(kInk));
    p.setPen(Qt::NoPen);
    p.drawEllipse(QRectF(cx - 2.1, cy - 2.1, 4.2, 4.2));
    /* 四向短射线（留空隙，别和圆圈连上） */
    p.setPen(mkPen(1.7));
    p.drawLine(QPointF(cx, cy - r - 3.4), QPointF(cx, cy - r - 0.8));
    p.drawLine(QPointF(cx, cy + r + 0.8), QPointF(cx, cy + r + 3.4));
    p.drawLine(QPointF(cx - r - 3.4, cy), QPointF(cx - r - 0.8, cy));
    p.drawLine(QPointF(cx + r + 0.8, cy), QPointF(cx + r + 3.4, cy));
}

/* 半圆（昼夜：左半亮=白天，右半暗=夜晚） */
void drawDayNight(QPainter &p)
{
    const double cx = 10.0, cy = 10.0, r = 6.6;
    /* 左半填充（白天） */
    QPainterPath path;
    path.moveTo(cx, cy - r);
    path.arcTo(QRectF(cx - r, cy - r, r * 2, r * 2), 90, 180);
    path.lineTo(cx, cy);
    path.closeSubpath();
    p.fillPath(path, QBrush(kInk));
    /* 外圈 */
    p.setPen(mkPen(1.7));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QRectF(cx - r, cy - r, r * 2, r * 2));
    /* 分界线（把明暗分开） */
    p.drawLine(QPointF(cx, cy - r), QPointF(cx, cy + r));
}

/* 网格（坐标网） */
void drawGrid(QPainter &p)
{
    const double x0 = 3.8, y0 = 3.8, s = 12.4, d = s / 3.0;
    p.setPen(mkPen(1.5));
    p.setBrush(Qt::NoBrush);
    p.drawRect(QRectF(x0, y0, s, s));
    p.drawLine(QPointF(x0 + d, y0), QPointF(x0 + d, y0 + s));
    p.drawLine(QPointF(x0 + 2 * d, y0), QPointF(x0 + 2 * d, y0 + s));
    p.drawLine(QPointF(x0, y0 + d), QPointF(x0 + s, y0 + d));
    p.drawLine(QPointF(x0, y0 + 2 * d), QPointF(x0 + s, y0 + 2 * d));
}

/* 三横线（菜单） */
void drawMenu(QPainter &p)
{
    p.setPen(mkPen(1.9));
    const double x0 = 3.6, x1 = 16.4;
    p.drawLine(QPointF(x0, 5.6),  QPointF(x1, 5.6));
    p.drawLine(QPointF(x0, 10.0), QPointF(x1, 10.0));
    p.drawLine(QPointF(x0, 14.4), QPointF(x1, 14.4));
}

/* 上下箭头（放大/缩小合成一个？）—— 备用：单独的加号 */
void drawPlus(QPainter &p)
{
    p.setPen(mkPen(2.0));
    p.drawLine(QPointF(5.0, 10.0), QPointF(15.0, 10.0));
    p.drawLine(QPointF(10.0, 5.0), QPointF(10.0, 15.0));
}

/* 轨迹：一条折线 + 起点实心点 + 终点圆环（GPX 导入/轨迹面板用）。
 * 折线取"左下→中上→右下再翘起"的形状，像地图上的一条记录轨迹。 */
void drawTrack(QPainter &p)
{
    p.setPen(mkPen(1.8));
    p.setBrush(Qt::NoBrush);
    QPainterPath path;
    path.moveTo(3.4, 15.0);
    path.lineTo(8.6, 9.2);
    path.lineTo(12.2, 12.4);
    path.lineTo(16.6, 4.6);
    p.drawPath(path);
    /* 起点：实心小圆 */
    p.setBrush(QBrush(kInk));
    p.setPen(Qt::NoPen);
    p.drawEllipse(QRectF(1.6, 13.2, 3.6, 3.6));
    /* 终点：圆环 */
    p.setBrush(Qt::NoBrush);
    p.setPen(mkPen(1.6));
    p.drawEllipse(QRectF(14.4, 2.4, 4.4, 4.4));
}

}  /* namespace */

/* ---- 直接在 QPainter 上绘制（供 IconButton 的 paintEvent 调用）----
 *
 * ★ 为什么不用 QPixmap 中转：
 *   中转要处理 devicePixelRatio（pixmap 40x40 + DPR2 = 逻辑 20x20），
 *   而 drawPixmap 的各个重载在 DPR≠1 时对"源矩形"的推断各不相同 ——
 *   实测在 Qt offscreen 插件下，图标只被画到目标矩形的**左上角**
 *   （放大镜只剩左上弧、三横线只剩两条）。
 *   直接用 QPainter 画路径就没有这层不确定性：坐标就是逻辑坐标。
 *
 * q：调用者的 painter（已 scale 到 kBase 逻辑尺寸）；size：图标边长。 */
void qt_draw_icon(QPainter &p, Qt::IconKind kind, double size)
{
    const double s = size / kBase;          /* 从 20 的基准缩放到目标尺寸 */
    p.save();
    p.scale(s, s);
    p.setPen(mkPen(1.8));
    p.setBrush(Qt::NoBrush);

    switch (kind) {
    case Qt::IconKind::Search:
    case Qt::IconKind::ZoomIn:  drawMagnifier(p, true);  break;
    case Qt::IconKind::ZoomOut: drawMagnifier(p, false); break;
    case Qt::IconKind::Locate:  drawLocate(p);            break;
    case Qt::IconKind::DayNight:drawDayNight(p);          break;
    case Qt::IconKind::Grid:    drawGrid(p);              break;
    case Qt::IconKind::Menu:    drawMenu(p);              break;
    case Qt::IconKind::Plus:    drawPlus(p);              break;
    case Qt::IconKind::Track:   drawTrack(p);             break;
    }
    p.restore();
}

QPixmap qt_make_icon(Qt::IconKind kind, int logicalSize)
{
    QPixmap pm(kBase * kScale, kBase * kScale);
    pm.setDevicePixelRatio(kScale);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.scale(kScale, kScale);
    qt_draw_icon(p, kind, kBase);
    p.end();

    /* ★ QPixmap::scaled() **不保留 devicePixelRatio**（Qt 的已知坑）：
     *   缩放后 DPR 变 1，逻辑尺寸 = 像素尺寸，图标会被裁掉一截。
     *   ⇒ 缩放后必须**重新设置 DPR**。 */
    if (logicalSize > 0 && logicalSize != kBase) {
        QPixmap out = pm.scaled(logicalSize * kScale, logicalSize * kScale,
                                Qt::KeepAspectRatio, Qt::SmoothTransformation);
        out.setDevicePixelRatio(kScale);
        return out;
    }
    return pm;
}
