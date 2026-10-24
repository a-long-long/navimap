#include "map_canvas.h"

#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QDateTime>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "main_window.h"

/* Qt 前端的地图画布实现。设计要点见 map_canvas.h 顶部注释。
 *
 * 平移实现说明：不直接用 mv_pan_pixels——那个按"屏幕像素→地图平移"换算，
 * 不知道旋转。这里用两次 mv_unproject（同一个视口状态）求出两个屏幕点
 * 各自的经纬度，把差值加到视口中心上。旋转角是投影的一部分，
 * 所以这个做法天然旋转正确，不用自己推旋转矩阵。
 */

static const double kZoomMin = 3.0, kZoomMax = 19.0;

MapCanvas::MapCanvas(App *a, MainWindow *mw, QWidget *parent)
    : QWidget(parent), m_a(a), m_mw(mw)
{
    setMouseTracking(true);          /* 不按键也收 move：拖拽期间必须 */
    setFocusPolicy(Qt::StrongFocus); /* 键盘快捷键进 keyPressEvent */
    setAttribute(Qt::WA_OpaquePaintEvent, true);
}

MapCanvas::~MapCanvas()
{
    delete[] m_px;
    m_px = nullptr;
}

/* 自持像素缓冲：格式 0x00RRGGBB（与 QImage::Format_RGB32 内存布局一致）。
   ★ 不用 SDL：Qt 前端不链 SDL，缓冲自己管。 */
void MapCanvas::ensureBuffer(int w, int h)
{
    if (m_px && m_px_w == w && m_px_h == h) return;
    delete[] m_px;
    m_px = new uint32_t[(size_t)w * (size_t)h]();
    m_px_w = w;
    m_px_h = h;
    m_valid = false;
    /* QImage 包住 m_px（不拥有它），stride = w 个 uint32 */
    m_img = QImage((uchar *)m_px, w, h, (int)(w * 4), QImage::Format_RGB32);
}

void MapCanvas::renderNow()
{
    int w = m_a->view.w > 0 ? m_a->view.w : width();
    int h = m_a->view.h > 0 ? m_a->view.h : height();
    if (w <= 0 || h <= 0) return;
    ensureBuffer(w, h);
    if (!m_px) return;

    m_a->view.w = w;
    m_a->view.h = h;
    memset(&m_st, 0, sizeof(m_st));
    /* 0x00RRGGBB，alpha 位给 0（无 alpha 通道） */
    if (app_render_to_buf(m_a, m_px, w, h, (size_t)w,
                          0x00FF0000u, 0x0000FF00u, 0x000000FFu, 0x00000000u,
                          &m_st) == 0) {
        m_valid = true;
        m_frame_ms = m_st.ms_bg + m_st.ms_areas + m_st.ms_roads +
                     m_st.ms_lines + m_st.ms_labels;
    }
    update();
}

void MapCanvas::tick()
{
    app_nav_poll(m_a);                       /* 收割后台算路结果 */
    if (m_a->sim_on) app_nav_sim_tick(m_a);  /* 推进模拟车头（真实 dt） */
    if (m_a->need_redraw || m_a->sim_on || m_a->nav_busy) {
        renderNow();
        m_a->need_redraw = 0;
    }
}

QString MapCanvas::statusText() const
{
    QString s = QString::asprintf(
        "lat %.5f  lon %.5f  zoom %.2f  LOD<=%d  帧 %.0f ms  %d省/%llu边",
        m_a->view.lat, m_a->view.lon, m_a->view.zoom, m_a->max_class,
        m_frame_ms, m_a->ms.n, (unsigned long long)m_a->total_edges);
    if (m_a->nav_busy) s += QStringLiteral("  计算中…");
    else if (m_a->nav_msg[0]) s += QStringLiteral("  ") + QString::fromUtf8(m_a->nav_msg);
    return s;
}

QString MapCanvas::summaryText() const
{
    if (m_a->sim_on) {
        return QString::asprintf("模拟 %.0fx  剩 %.1f km / %.0f 分",
                                 m_a->sim.speed_mul,
                                 navsim_remaining_m(&m_a->sim) / 1000.0,
                                 navsim_remaining_s(&m_a->sim) / 60.0);
    }
    if (m_a->have_rr) {
        return QString::asprintf("路线 %.1f km / %.0f 分",
                                 m_a->rr.total_m / 1000.0,
                                 m_a->rr.total_s / 60.0);
    }
    return QString();
}

QImage MapCanvas::grabMap()
{
    renderNow();
    return m_img.copy();
}

void MapCanvas::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    if (m_valid) {
        p.drawImage(0, 0, m_img);
    } else {
        p.fillRect(rect(), QColor(235, 237, 240));
    }
    drawScaleBar(p);
    drawCompass(p);
    drawCursorCoords(p);
    drawRouteInfo(p);
    if (m_showGrid) drawGraticule(p);
}

void MapCanvas::setShowGrid(bool v)
{
    m_showGrid = v;
    m_a->need_redraw = 1;
}

/* ---- 坐标网格（lat/lon 经纬网）：Qt 端叠加层，C 渲染核心一行不改 ----
 * 用 mv_visible_bbox 取当前可见范围的经纬度边界，再按 nice 1/2/5 步进
 * 取经线/纬线，逐条 mv_project 到像素画直线 + 边缘标注。
 * 旋转角 rot 是投影的一部分，所以网格随地图一起转，天然正确。 */
void MapCanvas::drawGraticule(QPainter &p)
{
    double bbox[4];
    mv_visible_bbox(&m_a->view, bbox);   /* [la0(底), lo0(左), la1(顶), lo1(右)] */
    double la0 = bbox[0], lo0 = bbox[1], la1 = bbox[2], lo1 = bbox[3];
    double span = qMax(lo1 - lo0, la1 - la0);
    if (span <= 0) return;

    /* nice 1/2/5 步进：让屏幕上大约出现 6 条线 */
    double p10 = pow(10.0, floor(log10(span / 6.0)));
    double n = (span / 6.0) / p10;
    double step = ((n < 1.5) ? 1.0 : (n < 3.0) ? 2.0 : (n < 7.0) ? 5.0 : 10.0) * p10;

    QFont f = p.font();
    f.setPointSize(8);
    p.setFont(f);
    p.setPen(QColor(40, 60, 95, 160));

    char buf[32];
    /* 经线（恒定 lon）：竖向，标签标在顶端 */
    double lon = ceil(lo0 / step - 1e-9) * step;
    for (; lon <= lo1 + 1e-9; lon += step) {
        int xb, yb, xt, yt;
        mv_project(&m_a->view, la0, lon, &xb, &yb);
        mv_project(&m_a->view, la1, lon, &xt, &yt);
        p.drawLine(xb, yb, xt, yt);
        snprintf(buf, sizeof(buf), "%.4f°%c", fabs(lon), lon >= 0 ? 'E' : 'W');
        p.drawText(xt + 2, yt + 10, QString::fromUtf8(buf));
    }
    /* 纬线（恒定 lat）：横向，标签标在左端 */
    double lat = ceil(la0 / step - 1e-9) * step;
    for (; lat <= la1 + 1e-9; lat += step) {
        int xb, yb, xt, yt;
        mv_project(&m_a->view, lat, lo0, &xb, &yb);
        mv_project(&m_a->view, lat, lo1, &xt, &yt);
        p.drawLine(xb, yb, xt, yt);
        snprintf(buf, sizeof(buf), "%.4f°%c", fabs(lat), lat >= 0 ? 'N' : 'S');
        p.drawText(xb + 2, yb - 3, QString::fromUtf8(buf));
    }
}

/* ---- 比例尺：与 chrome.c 的 draw_scale 同一套换算，Qt 侧重画 ---- */
void MapCanvas::drawScaleBar(QPainter &p)
{
    double mpp = (40075016.686 * cos(m_a->view.lat * M_PI / 180.0)) /
                 (256.0 * pow(2.0, m_a->view.zoom));
    double target = 110.0 * mpp;
    if (target <= 0) return;
    double step = pow(10.0, floor(log10(target)));
    double rem = target / step;
    double unit = (rem >= 5.0) ? 5.0 : (rem >= 2.0) ? 2.0 : 1.0;
    double dist = unit * step;
    int px = (int)(dist / mpp);
    if (px < 8 || px > width() - 40) return;

    int x = 12, y = height() - 26;
    QFont f = p.font();
    f.setPointSize(9);
    p.setFont(f);
    char buf[32];
    if (dist >= 1000.0) snprintf(buf, sizeof(buf), "%.1f 公里", dist / 1000.0);
    else                snprintf(buf, sizeof(buf), "%.0f 米", dist);

    p.setPen(QColor(30, 36, 48));
    p.drawLine(x, y, x + px, y);
    p.drawLine(x, y - 5, x, y + 5);
    p.drawLine(x + px, y - 5, x + px, y + 5);
    p.drawText(x, y - 8, QString::fromUtf8(buf));
}

/* ---- 右下角：光标处经纬度（没鼠标时显示视口中心）----
 *
 * 为什么放在这里而不是状态栏：状态栏在**左侧**放的是视图中心坐标，
 * 而"鼠标指哪"是另一个信息 —— 主流地图软件（OsmAnd / Google Maps /
 * 高德）都把光标经纬度单独放在右下角，方便用户抄坐标。
 *
 * ★ 不依赖字体符号：只画数字/小数点/度数符号（° 在任何字体链里都有）。
 *   经纬度的数字用 QString::asprintf 直接格式化，避免 QString::number
 *   的 locale 影响。
 */
void MapCanvas::drawCursorCoords(QPainter &p)
{
    double la, lo;
    const bool has_cursor = m_cursor_in;
    if (has_cursor) {
        mv_unproject(&m_a->view, m_cursor_x, m_cursor_y, &la, &lo);
    } else {
        la = m_a->view.lat;
        lo = m_a->view.lon;
    }
    /* 前缀区分语义：光标处 vs 视口中心（状态栏里已经有中心坐标了）。
       主流地图软件（OsmAnd / 高德）的做法也是鼠标一动就显示光标处坐标。 */
    QString txt = QString::asprintf("%s %.5f°%c  %.5f°%c",
                                    has_cursor ? "光标" : "中心",
                                    fabs(la), la >= 0 ? 'N' : 'S',
                                    fabs(lo), lo >= 0 ? 'E' : 'W');

    QFont f = p.font();
    f.setPointSize(8);
    p.setFont(f);
    QFontMetrics fm(f);
    const int pad = 6;
    int tw = fm.horizontalAdvance(txt);
    const int th = fm.height();
    const int margin = 10;
    int x = width()  - margin - tw - pad * 2;
    int y = height() - margin - th - pad * 2;
    /* 画布太窄时收缩文字而不是溢出（与搜索框浮层的处理一致） */
    if (x < margin) { x = margin; tw = qMax(40, width() - margin * 2 - pad * 2); }
    if (y < margin) { y = margin; }

    /* 半透明白底 + 细边框（与比例尺/指北针同一套视觉语言） */
    QRect box(x, y, tw + pad * 2, th + pad * 2);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(QColor(120, 130, 145, 160), 1.0));
    p.setBrush(QColor(255, 255, 255, 225));
    p.drawRoundedRect(box, 4, 4);
    p.setPen(QColor(30, 36, 48));
    p.drawText(box.adjusted(pad, pad, -pad, -pad),
               Qt::AlignVCenter | Qt::AlignRight, txt);
    p.setRenderHint(QPainter::Antialiasing, false);
}

/* ---- 左下角：路线里程 / 算路状态（替代被取消的底部状态栏）----
 *
 * ★ 只在信息**存在**时才画，否则不画（画面保持干净，正是取消状态栏的目的）：
 *     · 后台正在算路（nav_busy）→ "计算中…"
 *     · 有导航消息（nav_msg，如「无可行路线」「已到达目的地」）→ 显示该消息
 *     · 模拟导航中 / 已规划路线 → 显示里程与时间（summaryText）
 *   没有路线、也不在算路时，这里什么也不画。
 *
 * ★ 视觉语言与比例尺 / 坐标框一致：半透明白底 + 细边框 + 深色文字。
 *   位置在左下角、比例尺上方，不与之重叠。 */
void MapCanvas::drawRouteInfo(QPainter &p)
{
    QString txt;
    if (m_a->nav_busy) {
        txt = QStringLiteral("计算中…");
    } else if (m_a->nav_msg[0]) {
        txt = QString::fromUtf8(m_a->nav_msg);
    } else {
        txt = summaryText();
    }
    if (txt.isEmpty()) return;

    QFont f = p.font();
    f.setPointSize(9);
    p.setFont(f);
    QFontMetrics fm(f);
    const int pad = 6;
    int tw = fm.horizontalAdvance(txt);
    const int th = fm.height();

    /* 左下角，比例尺文字（baseline ≈ height-34）上方留间隙 */
    const int margin = 10;
    int x = margin;
    int y = height() - 44 - th - pad * 2;
    if (y < margin) y = margin;

    QRect box(x, y, tw + pad * 2, th + pad * 2);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(QColor(120, 130, 145, 160), 1.0));
    p.setBrush(QColor(255, 255, 255, 225));
    p.drawRoundedRect(box, 4, 4);
    p.setPen(QColor(30, 36, 48));
    p.drawText(box.adjusted(pad, pad, -pad, -pad),
               Qt::AlignVCenter | Qt::AlignLeft, txt);
    p.setRenderHint(QPainter::Antialiasing, false);
}

/* ---- 指北针：rot=0 时隐藏（正北朝上是默认态，nav-desktop 同款行为）---- */
void MapCanvas::drawCompass(QPainter &p)
{
    if (m_a->view.rot == 0.0) {
        m_compassRect = QRect();
        return;
    }
    int cx = width() - 40, cy = 52, R = 17;
    m_compassRect = QRect(cx - R - 4, cy - R - 4, 2 * R + 8, 2 * R + 8);

    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(QColor(70, 80, 100), 2));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QPoint(cx, cy), R, R);

    double ang = m_a->view.rot * M_PI / 180.0;
    double nx = cx + R * sin(ang), ny = cy - R * cos(ang);
    p.setPen(QPen(QColor(210, 60, 50), 2));
    p.drawLine(QPoint(cx, cy), QPointF(nx, ny));
    QFont f = p.font();
    f.setPointSize(10);
    f.setBold(true);
    p.setFont(f);
    p.drawText(QPointF(nx - 5, ny - 6), QStringLiteral("N"));
    p.setRenderHint(QPainter::Antialiasing, false);
}

/* ---- 鼠标 ---- */
void MapCanvas::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::RightButton) {
        app_nav_clear(m_a);                  /* 右键：清起终点/路线（与 SDL 版一致） */
        m_a->need_redraw = 1;
        return;
    }
    if (e->button() != Qt::LeftButton) return;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const QPointF ep = e->position();
#else
    const QPointF ep = e->pos();
#endif
    if (m_compassRect.isValid() && m_compassRect.contains(ep.toPoint())) {
        m_a->view.rot = 0.0;                 /* 点指北针回正北 */
        m_a->need_redraw = 1;
        renderNow();
        return;
    }
    m_lastx = m_downx = (int)ep.x();
    m_lasty = m_downy = (int)ep.y();
    if (e->modifiers() & Qt::ControlModifier) m_rotating = true;
    else                                      m_panning = true;
}

void MapCanvas::mouseMoveEvent(QMouseEvent *e)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const QPointF ep = e->position();
#else
    const QPointF ep = e->pos();
#endif
    int ex = (int)ep.x(), ey = (int)ep.y();
    /* 记光标位置：右下角坐标显示用（不需要按键也会触发，见 setMouseTracking） */
    if (m_cursor_x != ex || m_cursor_y != ey) {
        m_cursor_x = ex;
        m_cursor_y = ey;
        if (!m_cursor_in) { m_cursor_in = true; update(); }
        else update();
    }
    if (m_rotating) {
        m_a->view.rot += (ex - m_lastx) * 0.3;
        if (m_a->view.rot > 180.0)  m_a->view.rot -= 360.0;
        if (m_a->view.rot < -180.0) m_a->view.rot += 360.0;
        m_a->need_redraw = 1;
    } else if (m_panning) {
        int dx = ex - m_lastx, dy = ey - m_lasty;
        if (dx || dy) {
            /* 旋转正确的平移：同一视口下反投影两个屏幕点，取地理差 */
            double la1, lo1, la2, lo2;
            mv_unproject(&m_a->view, m_lastx, m_lasty, &la1, &lo1);
            mv_unproject(&m_a->view, ex, ey, &la2, &lo2);
            m_a->view.lat += la1 - la2;
            m_a->view.lon += lo1 - lo2;
            m_a->need_redraw = 1;
        }
    }
    m_lastx = ex;
    m_lasty = ey;
}

void MapCanvas::mouseReleaseEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton) return;
    bool was_rotating = m_rotating;
    bool was_panning  = m_panning;
    m_panning = m_rotating = false;
    if (was_rotating) return;
    if (!was_panning) return;
    /* 拖动与点击的判别：位移 ≤4px 算点击（与 SDL 版同口径） */
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const QPointF ep = e->position();
#else
    const QPointF ep = e->pos();
#endif
    if (abs((int)ep.x() - m_downx) + abs((int)ep.y() - m_downy) <= 4) {
        double la, lo;
        mv_unproject(&m_a->view, (int)ep.x(), (int)ep.y(), &la, &lo);
        app_nav_click(m_a, la, lo);          /* 先起点后终点，后台算路 */
    }
}

void MapCanvas::wheelEvent(QWheelEvent *e)
{
    /* 锚点缩放：光标下的地理位置保持不动。
     * angleDelta() 自 Qt 5.0 即有；位置取点 Qt 5.14+/Qt6 用 position()，
     * 更早的 Qt5（<5.14，如 5.1）无 position() 故用 pos()。 */
#if QT_VERSION >= QT_VERSION_CHECK(5,14,0)
    const QPointF p = e->position();
#else
    const QPointF p = e->pos();
#endif
    mv_zoom_at(&m_a->view, e->angleDelta().y() > 0 ? 0.5 : -0.5,
               (int)p.x(), (int)p.y());
    mv_clamp_zoom(&m_a->view, kZoomMin, kZoomMax);
    m_a->need_redraw = 1;
}

void MapCanvas::keyPressEvent(QKeyEvent *e)
{
    if (m_mw) { m_mw->handleKey(e->key()); return; }
    QWidget::keyPressEvent(e);
}

void MapCanvas::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    if (width() > 0 && height() > 0 &&
        (m_a->view.w != width() || m_a->view.h != height())) {
        m_a->view.w = width();
        m_a->view.h = height();
        ensureBuffer(width(), height());
        m_a->need_redraw = 1;
        renderNow();
    }
}
