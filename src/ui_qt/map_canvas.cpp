#include "map_canvas.h"

#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QDateTime>

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
    if (m_surf) SDL_FreeSurface(m_surf);
    /* 字形纹理挂在每帧的临时软件渲染器上，app_render_to_surface
       出口已经 drop 过；这里没有常驻 renderer 需要清理。 */
}

void MapCanvas::ensureSurface(int w, int h)
{
    if (m_surf) SDL_FreeSurface(m_surf);
    m_surf = SDL_CreateRGBSurface(0, w, h, 32, 0, 0, 0, 0);
    m_valid = false;
    if (m_surf)
        /* 实测定版：SDL_CreateRGBSurface(…,32,0,0,0,0) 默认给 ARGB8888
         * （小端机内存字节序 B,G,R,A）。QImage 对应 Format_RGB32——
         * 同布局且忽略第 4 字节（C 管线直写像素不保证 alpha=255）。
         * 前两次踩坑记录：RGBA8888 ⇒ 整帧黑屏（alpha=0 全透明）；
         * RGBX8888 ⇒ 红蓝互换（字节序想当然）。 */
        m_img = QImage((uchar *)m_surf->pixels, m_surf->w, m_surf->h,
                       (int)m_surf->pitch, QImage::Format_RGB32);
}

void MapCanvas::renderNow()
{
    int w = m_a->view.w > 0 ? m_a->view.w : width();
    int h = m_a->view.h > 0 ? m_a->view.h : height();
    if (w <= 0 || h <= 0) return;
    if (!m_surf || m_surf->w != w || m_surf->h != h) ensureSurface(w, h);
    if (!m_surf) return;

    m_a->view.w = w;
    m_a->view.h = h;
    memset(&m_st, 0, sizeof(m_st));
    if (app_render_to_surface(m_a, m_surf, &m_st) == 0) {
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
    if (m_compassRect.isValid() && m_compassRect.contains(e->pos())) {
        m_a->view.rot = 0.0;                 /* 点指北针回正北 */
        m_a->need_redraw = 1;
        renderNow();
        return;
    }
    m_lastx = m_downx = e->x();
    m_lasty = m_downy = e->y();
    if (e->modifiers() & Qt::ControlModifier) m_rotating = true;
    else                                      m_panning = true;
}

void MapCanvas::mouseMoveEvent(QMouseEvent *e)
{
    if (m_rotating) {
        m_a->view.rot += (e->x() - m_lastx) * 0.3;
        if (m_a->view.rot > 180.0)  m_a->view.rot -= 360.0;
        if (m_a->view.rot < -180.0) m_a->view.rot += 360.0;
        m_a->need_redraw = 1;
    } else if (m_panning) {
        int dx = e->x() - m_lastx, dy = e->y() - m_lasty;
        if (dx || dy) {
            /* 旋转正确的平移：同一视口下反投影两个屏幕点，取地理差 */
            double la1, lo1, la2, lo2;
            mv_unproject(&m_a->view, m_lastx, m_lasty, &la1, &lo1);
            mv_unproject(&m_a->view, e->x(), e->y(), &la2, &lo2);
            m_a->view.lat += la1 - la2;
            m_a->view.lon += lo1 - lo2;
            m_a->need_redraw = 1;
        }
    }
    m_lastx = e->x();
    m_lasty = e->y();
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
    if (abs(e->x() - m_downx) + abs(e->y() - m_downy) <= 4) {
        double la, lo;
        mv_unproject(&m_a->view, e->x(), e->y(), &la, &lo);
        app_nav_click(m_a, la, lo);          /* 先起点后终点，后台算路 */
    }
}

void MapCanvas::wheelEvent(QWheelEvent *e)
{
    /* 锚点缩放：光标下的地理位置保持不动 */
    mv_zoom_at(&m_a->view, e->delta() > 0 ? 0.5 : -0.5, e->x(), e->y());
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
        ensureSurface(width(), height());
        m_a->need_redraw = 1;
        renderNow();
    }
}
