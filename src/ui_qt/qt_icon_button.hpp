/* qt_icon_button.hpp —— 画矢量图标的扁平按钮。
 *
 * ★ 为什么不用 QToolButton::setIcon()：
 *   QToolButton 的图标尺寸由 style 决定（QStyle::pixelMetric(PM_SmallIconSize)），
 *   加上边框/padding 后可用区不够，pixmap 会被**按 style 的对齐方式画到一角**
 *   —— 实测：放大镜只剩左上角、菜单三横线只剩两条，看起来像"图标画错了"。
 *   自己 paintEvent 画就没有这层不确定性：图标位置/大小完全由代码说了算。
 *
 * 行为：
 *   · 悬停：底色变亮 + 边框变蓝（鼠标悬停提示由 setToolTip 提供）；
 *   · 按下：底色变深；
 *   · 可选中（checkable）：选中态底色偏蓝。
 *
 * ★ 浮层透明度（setOverlayPct）：**只淡底板与边框，图标与文字保持不透明**
 *   （与 C 侧 chrome.c / navdraw.c 的规则一致：J 先生 2026-10-25 要求
 *   "只淡底板、文字保持清晰"）。pct<=0 ⇒ 整个按钮不画（该浮层隐藏）。
 *   之前用的是 QGraphicsOpacityEffect，会把图标和文字一起淡掉。
 */
#ifndef NAVIMAP_QT_ICON_BUTTON_HPP
#define NAVIMAP_QT_ICON_BUTTON_HPP

#include <QToolButton>
#include <QPainter>
#include <QPixmap>

#include "qt_icons.h"

class IconButton : public QToolButton {
public:
    IconButton(Qt::IconKind kind, const QString &tip, QWidget *parent = nullptr)
        : QToolButton(parent), m_kind(kind)
    {
        setToolTip(tip);                 /* 鼠标悬停提示 */
        setToolButtonStyle(Qt::ToolButtonIconOnly);
        setAutoRaise(false);
        setFocusPolicy(Qt::NoFocus);     /* 浮层按钮不该抢键盘焦点 */
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover, true);
        setFixedSize(kSize, kSize);
    }

    static const int kSize = 36;        /* 纯图标按钮的边长（含边框） */
    static const int kIcon = 20;        /* 图标边长 */

    /* 浮层透明度（0~100）。只影响底板/边框的 alpha。 */
    void setOverlayPct(int pct)
    {
        if (m_ovlPct == pct) return;
        m_ovlPct = pct;
        update();
    }
    int overlayPct() const { return m_ovlPct; }

protected:
    void paintEvent(QPaintEvent *) override
    {
        /* 浮层整体隐藏（0%）：底板和前景都不画，否则图标会孤零零浮在底图上 */
        if (m_ovlPct <= 0) return;

        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        /* --- 底板 --- */
        QColor bg(255, 255, 255, 225);
        QColor bd(120, 130, 145, 160);
        if (isChecked()) {
            bg = QColor(214, 228, 250, 235);
            bd = QColor(60, 110, 200, 230);
        }
        if (isDown()) {
            bg = QColor(205, 220, 245, 235);
        } else if (underMouse()) {
            bg = QColor(255, 255, 255, 250);
            bd = QColor(60, 110, 200, 220);
        }
        /* ★ 只有底板/边框按浮层透明度淡；图标与文字保持完全不透明。 */
        const double k = m_ovlPct / 100.0;
        bg.setAlpha(qBound(0, qRound(bg.alpha() * k), 255));
        bd.setAlpha(qBound(0, qRound(bd.alpha() * k), 255));

        const qreal r = 6.0;
        QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        p.setPen(QPen(bd, 1.0));
        p.setBrush(bg);
        p.drawRoundedRect(box, r, r);

        /* --- 图标 + 可选文字 --- */
        /* ★ 直接用 QPainter 画矢量路径（qt_draw_icon），**不经过 QPixmap**：
         *   drawPixmap 在 DPR≠1 时对源矩形的推断有歧义，实测图标只被画到
         *   目标矩形的左上角（放大镜只剩左上弧、三横线只剩两条）。 */
        const bool withText = !text().isEmpty() &&
                              toolButtonStyle() == Qt::ToolButtonTextBesideIcon;
        if (withText) {
            /* 文字在右：图标居左、文字居中排布 */
            QRect tr = rect().adjusted(8, 0, -8, 0);
            QFontMetrics fm(font());
            int tw = fm.horizontalAdvance(text());
            int ih = kIcon;
            int gap = 6;
            int total = ih + gap + tw;
            int x = tr.left() + (tr.width() - total) / 2;
            p.save();
            p.translate(x, tr.center().y() - ih / 2.0);
            qt_draw_icon(p, m_kind, ih);
            p.restore();
            p.setPen(QColor(0x2c, 0x34, 0x40));
            p.drawText(QRect(x + ih + gap, tr.top(), tw + 2, tr.height()),
                       Qt::AlignVCenter | Qt::AlignLeft, text());
        } else {
            p.save();
            p.translate(rect().center().x() - kIcon / 2.0,
                        rect().center().y() - kIcon / 2.0);
            qt_draw_icon(p, m_kind, kIcon);
            p.restore();
        }
    }

private:
    Qt::IconKind m_kind;
    int m_ovlPct = 100;     /* 浮层透明度：只作用在底板/边框上 */
};

#endif
