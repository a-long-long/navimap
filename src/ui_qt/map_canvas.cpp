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
#include "utils/datum.h"   /* 坐标系（大地基准）：经纬网/读数按所选基准呈现 */

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

/* ---- 坐标网格（经纬网 / 方里格，与坐标系解耦）：Qt 端叠加层，C 渲染核心一行不改 ----
 * 网格类型由 m_a->grid_type 决定（默认经纬网 WGS84）；坐标系（m_a->datum）只管读数。
 * 经纬网：用 mv_visible_bbox 取可见范围（WGS84），把四角换到所选基准取范围，
 *   再按 nice 1/2/5 步进取该基准下的经线/纬线；每条线两端先换回 WGS84 再 mv_project，
 *   标签标基准坐标。旋转角 rot 是投影的一部分，所以网格随地图一起转，天然正确。
 * 方里格：高斯-克吕格公里网，投影基准用 grid_gk_datum（读数 WGS84 时取 CGCS2000）。 */
void MapCanvas::drawGraticule(QPainter &p)
{
    DatumId id = (DatumId)m_a->datum;
    if (m_a->grid_type == GRID_GK) { drawGraticuleGK(p, grid_gk_datum(id)); return; }  /* 方里格（公里网） */
    double bbox[4];
    mv_visible_bbox(&m_a->view, bbox);   /* [la0(底), lo0(左), la1(顶), lo1(右)]，WGS84 */

    /* 可见区四角 WGS84 → 基准，取基准下的经纬度范围（偏移近常数，4 角 min/max 保不漏画）。 */
    static const int corner[4][2] = { {0,1}, {0,3}, {2,1}, {2,3} };
    double dlat_min = 1e9, dlat_max = -1e9, dlon_min = 1e9, dlon_max = -1e9;
    double latc, lonc;
    for (int i = 0; i < 4; i++) {
        datum_wgs84_to(id, bbox[corner[i][0]], bbox[corner[i][1]], 0, &latc, &lonc, NULL);
        if (latc < dlat_min) dlat_min = latc;
        if (latc > dlat_max) dlat_max = latc;
        if (lonc < dlon_min) dlon_min = lonc;
        if (lonc > dlon_max) dlon_max = lonc;
    }
    double la0 = dlat_min, lo0 = dlon_min, la1 = dlat_max, lo1 = dlon_max;
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
    double lw0, lo0w, lw1, lo1w;
    int xb, yb, xt, yt;
    /* 经线（恒定 lon，基准下）：两端换回 WGS84 投影，标签标基准坐标 */
    double lon = ceil(lo0 / step - 1e-9) * step;
    for (; lon <= lo1 + 1e-9; lon += step) {
        datum_to_wgs84(id, la0, lon, 0, &lw0, &lo0w, NULL);
        datum_to_wgs84(id, la1, lon, 0, &lw1, &lo1w, NULL);
        mv_project(&m_a->view, lw0, lo0w, &xb, &yb);
        mv_project(&m_a->view, lw1, lo1w, &xt, &yt);
        p.drawLine(xb, yb, xt, yt);
        snprintf(buf, sizeof(buf), "%.4f°%c", fabs(lon), lon >= 0 ? 'E' : 'W');
        p.drawText(xt + 2, yt + 10, QString::fromUtf8(buf));
    }
    /* 纬线（恒定 lat，基准下） */
    double lat = ceil(la0 / step - 1e-9) * step;
    for (; lat <= la1 + 1e-9; lat += step) {
        datum_to_wgs84(id, lat, lo0, 0, &lw0, &lo0w, NULL);
        datum_to_wgs84(id, lat, lo1, 0, &lw1, &lo1w, NULL);
        mv_project(&m_a->view, lw0, lo0w, &xb, &yb);
        mv_project(&m_a->view, lw1, lo1w, &xt, &yt);
        p.drawLine(xb, yb, xt, yt);
        snprintf(buf, sizeof(buf), "%.4f°%c", fabs(lat), lat >= 0 ? 'N' : 'S');
        p.drawText(xb + 2, yb - 3, QString::fromUtf8(buf));
    }

    /* 细分刻度线：每格 5 等分（step/5），浅色细线、不标标签；
     * 依附网格开关（外层 m_showGrid 已 gate），由设置项 grid_subdiv 控制。 */
    if (m_a->grid_subdiv) {
        QPen sp(QColor(70, 90, 125, 110)); /* 比主网格（alpha 160）浅一档，层次清晰且可见 */
        p.setPen(sp);
        auto subMer = [&](double slon) {
            double a0, b0, a1, b1;
            datum_to_wgs84(id, la0, slon, 0, &a0, &b0, NULL);
            datum_to_wgs84(id, la1, slon, 0, &a1, &b1, NULL);
            int x1, y1, x2, y2;
            mv_project(&m_a->view, a0, b0, &x1, &y1);
            mv_project(&m_a->view, a1, b1, &x2, &y2);
            p.drawLine(x1, y1, x2, y2);
        };
        auto subPar = [&](double slat) {
            double a0, b0, a1, b1;
            datum_to_wgs84(id, slat, lo0, 0, &a0, &b0, NULL);
            datum_to_wgs84(id, slat, lo1, 0, &a1, &b1, NULL);
            int x1, y1, x2, y2;
            mv_project(&m_a->view, a0, b0, &x1, &y1);
            mv_project(&m_a->view, a1, b1, &x2, &y2);
            p.drawLine(x1, y1, x2, y2);
        };
        const double f = step / 5.0;
        double base = ceil(lo0 / step - 1e-9) * step;
        for (int k = 1; k <= 4; k++) { double s = base - (5 - k) * f; if (s >= lo0 - 1e-9) subMer(s); }
        for (double m = base; m <= lo1 + 1e-9; m += step)
            for (int k = 1; k <= 4; k++) { double s = m + k * f; if (s <= lo1 + 1e-9) subMer(s); }
        double baseLat = ceil(la0 / step - 1e-9) * step;
        for (int k = 1; k <= 4; k++) { double s = baseLat - (5 - k) * f; if (s >= la0 - 1e-9) subPar(s); }
        for (double m = baseLat; m <= la1 + 1e-9; m += step)
            for (int k = 1; k <= 4; k++) { double s = m + k * f; if (s <= la1 + 1e-9) subPar(s); }
    }
}

/* 方里格（高斯公里网）：非 WGS84 坐标系用。datum_gk_forward/inverse 内部把 WGS84
 * 经纬度透明换到所选基准、再用该基准椭球做高斯投影（3° 带），底图几何
 * （WGS84 Web Mercator）不动 —— 与经纬网同一条"端点反算回 WGS84 再投影"的路子。
 * 画等 X 北向 / 等 Y 东向公里线，标公里整数；grid_subdiv 时每格 5 等分画浅色细线。 */
void MapCanvas::drawGraticuleGK(QPainter &p, DatumId id)
{
    double bbox[4];
    mv_visible_bbox(&m_a->view, bbox);   /* WGS84 [la0(底), lo0(左), la1(顶), lo1(右)] */
    const double lon0 = round(((bbox[1] + bbox[3]) * 0.5 / 3.0)) * 3.0;  /* 3° 带中央经线 */
    const int   zone = (int)round(lon0 / 3.0);                             /* 带号（如 39） */

    /* 可见区四角 WGS84 → 高斯平面坐标（米），取范围 */
    static const int corner[4][2] = { {0,1}, {0,3}, {2,1}, {2,3} };
    double Xmin = 1e30, Xmax = -1e30, Ymin = 1e30, Ymax = -1e30;
    for (int i = 0; i < 4; i++) {
        double X, Y;
        datum_gk_forward(id, bbox[corner[i][0]], bbox[corner[i][1]], lon0, &X, &Y);
        if (X < Xmin) Xmin = X;
        if (X > Xmax) Xmax = X;
        if (Y < Ymin) Ymin = Y;
        if (Y > Ymax) Ymax = Y;
    }
    double span = qMax(Xmax - Xmin, Ymax - Ymin);
    if (span <= 0) return;

    /* nice 公里步长：让屏幕约出现 6 条线 */
    double p10 = pow(10.0, floor(log10(span / 6.0)));
    double nn = (span / 6.0) / p10;
    double step = ((nn < 1.5) ? 1.0 : (nn < 3.0) ? 2.0 : (nn < 7.0) ? 5.0 : 10.0) * p10;

    QFont f = p.font();
    f.setPointSize(8);
    p.setFont(f);
    p.setPen(QColor(40, 60, 95, 160));

    char buf[32];
    int x1, y1, x2, y2;
    double lat1, lon1, lat2, lon2;

    /* 竖线（等 Y 东向）：每整 step 米一条，从 Xmin 到 Xmax */
    double Yk = ceil(Ymin / step - 1e-9) * step;
    for (; Yk <= Ymax + 1e-9; Yk += step) {
        datum_gk_inverse(id, Xmin, Yk, lon0, &lat1, &lon1);
        datum_gk_inverse(id, Xmax, Yk, lon0, &lat2, &lon2);
        mv_project(&m_a->view, lat1, lon1, &x1, &y1);
        mv_project(&m_a->view, lat2, lon2, &x2, &y2);
        p.drawLine(x1, y1, x2, y2);
        /* 标签放北端（顶边，y+10 内缩）避免落到画布外（与 WGS84 经纬网同套路）。
         * ★ 刻度 = 高斯坐标公里数（含 500km 假东偏移，国标写法）：Y/1000。
         *   不要写 (Y-500000)/1000 —— 那是相对中央经线的带符号偏移，北京在西侧会
         *   显示成负数（如 -51），与国标方里格读数（如 449）不符，看着像"刻度不准"。
         *   带号并入东坐标前两位（国标完整东坐标 = 带号×1000 + Y/1000，如 39 带→39449），
         *   这样只看网格线也能知道在哪一带。 ★ 与右下角读数 GK%d带 同一算法。 */
        snprintf(buf, sizeof(buf), "%d", (int)round(zone * 1000.0 + Yk / 1000.0));
        p.drawText(x2 + 2, y2 + 10, QString::fromUtf8(buf));
    }
    /* 横线（等 X 北向）：每整 step 米一条，从 Ymin 到 Ymax */
    double Xk = ceil(Xmin / step - 1e-9) * step;
    for (; Xk <= Xmax + 1e-9; Xk += step) {
        datum_gk_inverse(id, Xk, Ymin, lon0, &lat1, &lon1);
        datum_gk_inverse(id, Xk, Ymax, lon0, &lat2, &lon2);
        mv_project(&m_a->view, lat1, lon1, &x1, &y1);
        mv_project(&m_a->view, lat2, lon2, &x2, &y2);
        p.drawLine(x1, y1, x2, y2);
        /* 标签放西端（左边，x+2 内缩）避免落到画布外（与 WGS84 经纬网同套路）。 */
        snprintf(buf, sizeof(buf), "%d", (int)round(Xk / 1000.0));
        p.drawText(x1 + 2, y1 - 3, QString::fromUtf8(buf));
    }

    /* 细分刻度线：每格 5 等分（step/5），浅色细线、不标标签，依附 grid_subdiv */
    if (m_a->grid_subdiv) {
        QPen sp(QColor(70, 90, 125, 110));
        p.setPen(sp);
        const double f5 = step / 5.0;
        for (double base = floor(Xmin / step) * step; base <= Xmax + 1e-9; base += step)
            for (int k = 1; k <= 4; k++) {
                double Xs = base + k * f5;
                if (Xs <= Xmin - 1e-9 || Xs >= Xmax + 1e-9) continue;
                datum_gk_inverse(id, Xs, Ymin, lon0, &lat1, &lon1);
                datum_gk_inverse(id, Xs, Ymax, lon0, &lat2, &lon2);
                mv_project(&m_a->view, lat1, lon1, &x1, &y1);
                mv_project(&m_a->view, lat2, lon2, &x2, &y2);
                p.drawLine(x1, y1, x2, y2);
            }
        for (double base = floor(Ymin / step) * step; base <= Ymax + 1e-9; base += step)
            for (int k = 1; k <= 4; k++) {
                double Ys = base + k * f5;
                if (Ys <= Ymin - 1e-9 || Ys >= Ymax + 1e-9) continue;
                datum_gk_inverse(id, Xmin, Ys, lon0, &lat1, &lon1);
                datum_gk_inverse(id, Xmax, Ys, lon0, &lat2, &lon2);
                mv_project(&m_a->view, lat1, lon1, &x1, &y1);
                mv_project(&m_a->view, lat2, lon2, &x2, &y2);
                p.drawLine(x1, y1, x2, y2);
            }
    }

    /* 左上角不再单独画网格图例：当前坐标系 / 网格类型已由右下角坐标框承载
     * （经纬网显示经纬度、方里格显示高斯公里网坐标），避免"方里格"等字样冗余。 */
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
QString MapCanvas::cursorCoordsText() const
{
    double la, lo;
    const bool has_cursor = m_cursor_in;
    if (has_cursor) {
        mv_unproject(&m_a->view, m_cursor_x, m_cursor_y, &la, &lo);
    } else {
        la = m_a->view.lat;
        lo = m_a->view.lon;
    }
    /* 读数按所选基准 / 网格类型呈现（底图几何仍是 WGS84，只换读数/标注口径）。 */
    DatumId id = (DatumId)m_a->datum;

    /* 方里格（高斯-克吕格公里网）：与 drawGraticuleGK 用同一中央经线取样，
     * 读数直接给出高斯坐标（X 北 / Y 东，公里），和屏幕上画的方里格一一对应。 */
    if (m_a->grid_type == GRID_GK) {
        const double lon0 = round(m_a->view.lon / 3.0) * 3.0;   /* 3° 带中央经线 */
        DatumId gid = grid_gk_datum(id);
        double X, Y;
        datum_gk_forward(gid, la, lo, lon0, &X, &Y);
        int zone = (int)round(lon0 / 3.0);
        return QString::asprintf("%s %s GK%d带 X%.3f Y%.3f km",
                                 datum_short(gid),
                                 has_cursor ? "光标" : "中心",
                                 zone, X / 1000.0, Y / 1000.0);
    }

    /* 经纬网：按所选基准呈现经纬度 */
    double lad, lod;
    datum_wgs84_to(id, la, lo, 0, &lad, &lod, NULL);

    /* 前缀区分语义：基准短名 / 光标处 vs 视口中心（状态栏里已经有中心坐标了）。
       主流地图软件（OsmAnd / 高德）的做法也是鼠标一动就显示光标处坐标。 */
    return QString::asprintf("%s %s %.5f°%c  %.5f°%c",
                             datum_short(id),
                             has_cursor ? "光标" : "中心",
                             fabs(lad), lad >= 0 ? 'N' : 'S',
                             fabs(lod), lod >= 0 ? 'E' : 'W');
}

void MapCanvas::drawCursorCoords(QPainter &p)
{
    QString txt = cursorCoordsText();

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
        app_nav_clear(m_a);                  /* 右键：清起终点/路线 */
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
    /* 拖动与点击的判别：位移 ≤4px 算点击 */
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const QPointF ep = e->position();
#else
    const QPointF ep = e->pos();
#endif
    if (abs((int)ep.x() - m_downx) + abs((int)ep.y() - m_downy) <= 4) {
        double la, lo;
        mv_unproject(&m_a->view, (int)ep.x(), (int)ep.y(), &la, &lo);
        if (m_viaPick) {                     /* 途经点拾取模式：左键=加一个途经点 */
            app_nav_add_via(m_a, la, lo);
            if (m_onViaPicked) m_onViaPicked(la, lo);
        } else {
            app_nav_click(m_a, la, lo);      /* 先起点后终点，后台算路 */
        }
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
