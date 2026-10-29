#include "main_window.h"
#include "map_canvas.h"
#include "qt_icons.h"
#include "qt_icon_button.hpp"
#include "utils/geo.h"    /* nm_sun_is_night：自动昼夜推算 */
#include "utils/datum.h"   /* 坐标系（大地基准）：枚举与标签 */
#include "settings/settings.h"  /* settings_set_int：切基准后落盘 */
#include "download_dialog.h"     /* 下载地图对话框（全球 Geofabrik 列表） */
#include <QMessageBox>
#include <QDir>

#include <QMenuBar>
#include <QSlider>
#include <QDialog>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QMenu>
#include <QToolBar>
#include <functional>
#include <QToolButton>
#include <QAbstractButton>
#include <QApplication>
#include <QThread>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QLabel>
#include <QTimer>
#include <QDockWidget>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QImage>
#include <QDebug>
#include <cmath>
#include <QFileDialog>
#include <QPushButton>
#include <QScrollArea>
#include <QColor>

/* 行容器里的"行"数（= 轨迹 + 收藏，空提示行不算）——自检与调试图用。
 * 必须定义在 runUiSelfTest 之前（它在 GPX 自检段里数面板条目）。 */
static int gpx_row_count(const QWidget *rows)
{
    if (!rows) return 0;
    return (int)rows->findChildren<QWidget *>(QStringLiteral("trkrow")).size();
}

static int via_row_count(const QWidget *rows)
{
    if (!rows) return 0;
    return (int)rows->findChildren<QWidget *>(QStringLiteral("viarow")).size();
}

/* 两点大圆距离（米），给"路线是否经过途经点"判据用。 */
static double haversineM(double la1, double lo1, double la2, double lo2)
{
    const double R = 6371008.8;
    double p1 = la1 * 3.141592653589793 / 180.0;
    double p2 = la2 * 3.141592653589793 / 180.0;
    double dp = (la2 - la1) * 3.141592653589793 / 180.0;
    double dl = (lo2 - lo1) * 3.141592653589793 / 180.0;
    double s = sin(dp / 2) * sin(dp / 2) +
               cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);
    return 2 * R * asin(sqrt(s));
}

/* 点到线段（经纬度端点）最近距离（米）。 */
static double pointSegM(double plat, double plon,
                        double aLat, double aLon,
                        double bLat, double bLon)
{
    /* 退化成点 */
    if (aLat == bLat && aLon == bLon) return haversineM(plat, plon, aLat, aLon);
    /* 把线段投到以 a 为原点、沿 ab 方向的一维刻度 t∈[0,1] */
    double dx = bLon - aLon, dy = bLat - aLat;
    double len2 = dx * dx + dy * dy;
    double t = ((plon - aLon) * dx + (plat - aLat) * dy) / len2;
    if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
    double cx = aLon + t * dx, cy = aLat + t * dy;
    return haversineM(plat, plon, cy, cx);
}

/* ---------------- 构造 / 生命周期 ---------------- */

MainWindow::MainWindow(App *a, QWidget *parent)
    : QMainWindow(parent), m_a(a)
{
    setWindowTitle(QStringLiteral("NavMap"));
    m_canvas = new MapCanvas(a, this);
    setCentralWidget(m_canvas);

    buildMenus();
    buildSearchOverlay();
    buildRightCol();
    buildStepsDock();

    /* 地图点选途经点后：面板里的"刚加的那一行"立即出现（面板开着时才点得到）。 */
    m_canvas->m_onViaPicked = [this](double, double) { refreshViaPanel(); };

    /* 浮层透明度：默认 75%（App::ovl_alpha 已由 app_init 从设置读好）。
     * 每个浮层一份，**只淡底板**（图标与文字保持不透明）—— 作用在
     * IconButton 的底板 alpha 与面板/输入框的 QSS rgba 背景上。
     * 只对顶层窗口有效的 setWindowOpacity 在这里没用。 */
    applyOverlayAlpha();

    /* 两个浮层（搜索在左上、地图操作在右上）贴到画布上。
     * 必须在浮层都建好之后、resize 之前做 —— resize 会触发画布 Resize
     * → eventFilter → layoutOverlays（那时候浮层已经在了）。 */
    layoutOverlays();

    /* 初始尺寸跟随 App（命令行 --w/--h），resizeEvent 会同步回 view */
    resize(a->view.w > 0 ? a->view.w : 1280, a->view.h > 0 ? a->view.h : 800);

    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, [this]() { tick(); });
}

MainWindow::~MainWindow() = default;

void MainWindow::start()
{
    m_timer->start(33);
    m_canvas->renderNow();
    updateGridBtn();   /* 让坐标网按钮显示当前坐标系统一短名 */
}

bool MainWindow::takeShot(const QString &path)
{
    m_canvas->renderNow();
    QImage img = m_canvas->grabMap();
    if (img.isNull()) return false;
    return img.save(path);
}

/* 展开搜索框（--shot-search 用） */
void MainWindow::expandSearchForShot()
{
    m_searchBtn->hide();
    m_search->show();
    m_search->setFocus();
    layoutOverlays();
}

/* ---------------- 无头 UI 自检 ----------------
 *
 * 断言"搜索栏改成按钮、点击才出现"这条交互链真的成立：
 *   ① 初始：搜索**按钮**可见、搜索**输入框**不可见；
 *   ② 点按钮 → 输入框出现并获得焦点；
 *   ③ 输入文字 → 触发查询（结果浮层里有内容或至少查询被调用了）；
 *   ④ Esc（焦点在输入框）→ 输入框收起、按钮恢复可见；
 *   ⑤ 每个按钮都有**非空 tooltip**（鼠标悬停提示）；
 *   ⑥ 右栏是**浮在画布上的独立按钮**（parent 是画布，不是工具栏）；
 *   ⑦ **展开搜索框不移动右栏按钮**（J 先生明确要求的一条）；
 *   ⑧ 顶栏**只有**搜索按钮（没有浏览/导航页签）。
 *   ⑨ 右下角有坐标显示（经纬度文字**真的画出来了**，用像素判据）；
 *   ⑩ 鼠标移动后右下角坐标**随之更新**（光标处经纬度，不是固定中心点）。
 *
 * ★ 为什么不用 QTest::mouseClick：offscreen 平台插件下没有真实光标，
 *   事件投递路径与真机不同。直接调 clicked() 信号更稳，也正好验证
 *   "点了按钮 ⇒ 状态变化"这条**逻辑**（真机上的鼠标事件是 Qt 框架保证的）。
 */
/* 从 graph_dir（可能是 products/graph 或单省 products/graph/beijing）向上找
 * 项目根：含 sources/ 与 scripts/ 的那个目录。向下兼容旧的两级 cdUp 写法。 */
static QString repoRootFromGraphDir(const QString &graphDir)
{
    QDir d(graphDir);
    for (int i = 0; i < 6; i++) {
        if (QDir(d.filePath(QStringLiteral("sources"))).exists() &&
            QDir(d.filePath(QStringLiteral("scripts"))).exists())
            return d.absolutePath();
        if (!d.cdUp()) break;
    }
    QDir f(graphDir); f.cdUp(); f.cdUp();
    return f.absolutePath();
}

int MainWindow::runUiSelfTest()
{
    int fail = 0;
    auto check = [&](bool ok, const char *what) {
        printf("  %-44s %s\n", what, ok ? "OK" : "FAIL");
        if (!ok) fail++;
    };

    /* ① 初始状态 */
    check(m_searchBtn && m_searchBtn->isVisible(), "初始：搜索按钮可见");
    check(m_search && !m_search->isVisible(),   "初始：搜索输入框隐藏");

    /* ⑦ 展开搜索框后右栏按钮**不能移动**
     *   （浮层的 parent 是画布，顶栏内容变化不该影响它；这条专门盯这个）
     *
     * ★ 判据有两层，缺一不可：
     *   (a) 坐标：浮层与每个按钮的全局坐标逐个比对，必须完全相同；
     *   (b) 像素：抓右栏那一小块的前后两帧比像素，必须完全相同。
     *   只查坐标不够 —— 坐标没变但重绘出了别的内容也算"受影响"。
     *   只查像素不够 —— 两次**独立**渲染的地图本身就有噪声（字号缓存/图集状态），
     *   必须**同一帧**内切换状态再比，否则全是噪声（右栏那块会报出几千 px 差异，
     *   实测踩过：两张 --shot 的右栏区域差 8689 px，其实是地图重绘噪声）。
     */
    /* --- 展开**之前**：记坐标 + 抓右栏像素 --- */
    QPoint ov0 = m_rightOverlay ? m_rightOverlay->pos() : QPoint(-1, -1);
    QVector<QPoint> btn_pos_before;
    QImage pix_before;
    if (m_rightOverlay) {
        for (QAbstractButton *b : m_rightOverlay->findChildren<QAbstractButton *>())
            btn_pos_before << b->mapToGlobal(QPoint(0, 0));
        pix_before = m_rightOverlay->grab().toImage();
    }

    /* ② 点搜索按钮 → 输入框出现并聚焦 */
    if (m_searchBtn) emit m_searchBtn->clicked();
    QApplication::processEvents();
    check(m_search && m_search->isVisible(),       "点按钮后：输入框出现");
    check(m_search && m_search->hasFocus(),         "点按钮后：输入框获得焦点");

    /* --- 展开**之后**：再记一次坐标 + 抓一次右栏像素 --- */
    QPoint ov1 = m_rightOverlay ? m_rightOverlay->pos() : QPoint(-2, -2);
    check(ov0 == ov1, "展开搜索框后：右栏浮层位置不变");

    bool btn_same = true;
    if (m_rightOverlay) {
        QVector<QPoint> after;
        for (QAbstractButton *b : m_rightOverlay->findChildren<QAbstractButton *>())
            after << b->mapToGlobal(QPoint(0, 0));
        btn_same = (after == btn_pos_before);
        if (!btn_same) {
            printf("  展开前: ");
            for (QPoint p : btn_pos_before) printf("(%d,%d) ", p.x(), p.y());
            printf("\n  展开后: ");
            for (QPoint p : after) printf("(%d,%d) ", p.x(), p.y());
            printf("\n");
        }
    }
    check(btn_same, "展开搜索框后：每个右栏按钮坐标不变");

    QImage pix_after;
    if (m_rightOverlay) {
        pix_after = m_rightOverlay->grab().toImage();
        bool same = (pix_before.size() == pix_after.size());
        if (same) {
            for (int y = 0; y < pix_before.height() && same; y++)
                for (int x = 0; x < pix_before.width(); x++)
                    if (pix_before.pixel(x, y) != pix_after.pixel(x, y)) { same = false; break; }
        }
        check(same, "展开搜索框后：右栏按钮像素完全一致");
    }

    /* ③ 输入文字 → 触发查询（结果浮层被填充） */
    if (m_search) {
        m_search->setText(QStringLiteral("中"));
        QApplication::processEvents();
        check(!m_hits->isVisible() || m_hits->count() > 0,
              "输入文字：查询被触发（结果浮层有内容或无结果）");
    }

    /* ④ Esc（焦点在输入框）→ 收起输入框、按钮恢复 */
    if (m_search) {
        QKeyEvent ke(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(m_search, &ke);
        QApplication::processEvents();
        check(m_search && !m_search->isVisible(),    "Esc 后：输入框收起");
        check(m_searchBtn && m_searchBtn->isVisible(), "Esc 后：搜索按钮恢复");
    }

    /* ⑤ 每个**我们自己的**按钮都有悬停提示。
       ★ 不能对 findChildren 找到的所有 QAbstractButton 一刀切要求 tooltip ——
         Qt 内部控件（QLineEditIconButton / QDockWidget 的标题按钮等）也在这棵树里，
         它们不是我们的界面元素，管不着。
         判据：只查我们自己建的按钮 —— 两个浮层容器里的。 */
    int n_btn = 0, n_tip = 0;
    QStringList no_tip;
    QList<QWidget *> overlays;
    if (m_rightOverlay) overlays << m_rightOverlay;
    if (m_searchOverlay) overlays << m_searchOverlay;
    for (QWidget *ov : overlays) {
        for (QAbstractButton *b : ov->findChildren<QAbstractButton *>()) {
            const QString cls = b->metaObject()->className();
            if (cls.startsWith("QLineEditIconButton")) continue;  /* Qt 内部 */
            n_btn++;
            if (!b->toolTip().isEmpty()) n_tip++;
            else no_tip << cls;
        }
    }
    printf("  （自有按钮 %d 个，其中 %d 个有悬停提示）\n", n_btn, n_tip);
    if (!no_tip.isEmpty())
        printf("  缺提示的：%s\n", no_tip.join(", ").toUtf8().constData());
    check(n_btn > 0 && n_btn == n_tip, "所有自有按钮都有鼠标悬停提示");

    /* ⑧ 界面**没有顶栏工具条**：搜索按钮是浮在地图上的独立按钮。
       （原先顶栏 QToolBar 放搜索框+页签，现已整体取消。）
       判据：窗口里不存在 QToolBar（排除 Qt 内部的），且搜索按钮浮在画布上。 */
    bool has_toolbar = false;
    for (QWidget *w : findChildren<QWidget *>()) {
        if (qobject_cast<QToolBar *>(w)) { has_toolbar = true; break; }
    }
    check(!has_toolbar, "界面没有顶栏工具条");
    check(m_searchOverlay && m_searchOverlay->parentWidget() == m_canvas,
          "搜索按钮浮在地图上（独立按钮，非顶栏）");

    /* ⑥ 右栏按钮浮在画布上（是画布的后代），不是工具栏里的 widget。
       注意：按钮的直接 parent 是浮层容器 m_rightOverlay，
       所以要查"是不是画布的后代"而不是"parent 是不是画布"。 */
    bool overlay_ok = m_rightOverlay && m_rightOverlay->parentWidget() == m_canvas;
    for (QWidget *w : findChildren<QWidget *>()) {
        if (auto *b = qobject_cast<QAbstractButton *>(w)) {
            if (b->toolTip() == QStringLiteral("放大") ||
                b->toolTip() == QStringLiteral("定位到关注点")) {
                QWidget *anc = b->parentWidget();
                bool on_canvas = false;
                while (anc) {
                    if (anc == m_canvas) { on_canvas = true; break; }
                    anc = anc->parentWidget();
                }
                if (!on_canvas) overlay_ok = false;
            }
        }
    }
    check(overlay_ok, "右栏按钮浮在画布上（独立按钮，非工具栏 widget）");

    /* ⑨ 右下角有坐标显示（经纬度）。
     *
     * ★ 判据用**像素**而不是"代码里有没有这个函数"：
     *   函数在 ≠ 画出来了。offscreen 下 grab() 拿到的是真实渲染结果，
     *   直接扫右下角那块有没有"深色文字像素"就能证明它真画出来了。
     *   （踩过的坑：--shot 路径不启动主循环 ⇒ 状态栏文字从未刷新 ⇒
     *     我交付过的截图里"坐标"一直是空的，截图在撒谎。）
     */
    {
        const int cw = m_canvas->width(), chh = m_canvas->height();
        int dark = 0;
        if (cw > 0 && chh > 0) {
            QImage img = m_canvas->grab().toImage();
            /* 右下角 260×28 区域 */
            for (int y = qMax(0, chh - 28); y < chh; y++)
                for (int x = qMax(0, cw - 260); x < cw; x++) {
                    QColor c = img.pixelColor(x, y);
                    if (c.red() < 110 && c.green() < 110 && c.blue() < 110) dark++;
                }
        }
        check(dark > 20, "右下角有坐标显示（经纬度文字已渲染）");
        printf("  （右下角深色文字像素 %d）\n", dark);
    }

    /* ⑩ 鼠标移动后，右下角坐标随之变化（光标处经纬度，不是固定的中心点）。
     *
     * ★ 用 QMouseEvent 走真实事件路径：MapCanvas::mouseMoveEvent 会记下光标
     *   位置，右下角显示的就是该点的经纬度 ⇒ 移动后像素必须变。
     *   坐标不动 ⇒ 说明光标跟踪没生效（setMouseTracking 或事件没接上）。
     */
    {
        auto grabCorner = [&]() {
            QImage img = m_canvas->grab().toImage();
            int cw = m_canvas->width(), chh = m_canvas->height();
            uint32_t acc = 0;
            for (int y = qMax(0, chh - 28); y < chh; y++)
                for (int x = qMax(0, cw - 260); x < cw; x++)
                    acc = acc * 31u + img.pixel(x, y);
            return acc;
        };
        uint32_t before = grabCorner();
        QMouseEvent me(QEvent::MouseMove, QPointF(120, 90),
                       QPointF(120, 90), Qt::NoButton, Qt::NoButton,
                       Qt::NoModifier);
        QApplication::sendEvent(m_canvas, &me);
        QApplication::processEvents();
        m_canvas->update();
        QApplication::processEvents();
        uint32_t after = grabCorner();
        check(before != after, "鼠标移动后：右下角坐标随之更新");
    }

    /* ⑪ 坐标系（大地基准）：右栏**没有下拉框**了，"当前基准"只剩坐标网按钮
     *    悬停提示这一处常驻显示 ⇒ 提示必须带上当前基准标签（"坐标系：<label>"），
     *    且切基准后**随之更新**（否则用户看到的是过期基准）。来源说明已归入菜单/
     *    坐标系说明，按钮提示刻意做短（避免超出界面），故不再要求含 datum_note。
     *    同时验读数**真的变**（前缀换成基准短名）、切回去能**原样恢复**：
     *    只查"变了"会漏掉抖动，只查"恢复"会漏掉根本没生效。 */
    {
        const int keep = m_a->datum;
        check(m_gridBtn != nullptr, "坐标网按钮存在（悬停提示的宿主）");

        setDatum(DATUM_WGS84);
        QString tip = m_gridBtn->toolTip();
        check(tip.contains(QStringLiteral("坐标系：") +
                           QString::fromUtf8(datum_label(DATUM_WGS84))),
              "悬停提示带当前坐标系（WGS84）标签");

        m_canvas->update();   /* 光标位置沿用上一条断言里那次 MouseMove，保持固定 */
        const QString t_wgs = m_canvas->cursorCoordsText();
        setDatum(DATUM_BJ54);
        const QString t_bj = m_canvas->cursorCoordsText();
        const QString tip_bj = m_gridBtn->toolTip();   /* 提示要跟手变，当场取样 */
        setDatum(DATUM_XA80);
        const QString t_xa = m_canvas->cursorCoordsText();
        setDatum(DATUM_WGS84);
        const QString t_back = m_canvas->cursorCoordsText();

        check(tip_bj.contains(QStringLiteral("坐标系：") +
                              QString::fromUtf8(datum_label(DATUM_BJ54))),
              "切到北京54：悬停提示里的当前坐标系随之更新");

        check(t_bj != t_wgs && t_bj.startsWith(QString::fromUtf8("北京54")),
              "切到北京54：读数变化且带基准前缀");
        check(t_xa.startsWith(QString::fromUtf8("西安80")),
              "切到西安80：读数带基准前缀");
        check(t_back == t_wgs, "切回 WGS84：读数原样恢复");

        setDatum(keep);                          /* 还原，别把自检结果写进设置 */
    }

    /* ⑫ 浮层透明度：默认 75%、且**每个浮层各一份**。
     *    只查"有个 effect"会漏掉"全都按同一个值设了"；这里成对验：
     *    改搜索浮层 → 搜索的跟着变、**右栏的不许变**。
     *    ★ 现在是"底板按比例淡、文字/图标不淡"，所以查的是
     *      IconButton::overlayPct()（底板系数），不是 QGraphicsOpacityEffect。 */
    {
        const int keepS = m_a->ovl_alpha[OVL_SEARCH];
        const int keepR = m_a->ovl_alpha[OVL_RCOL];
        check(ovl_count() == (int)OVL_COUNT, "浮层清单完整（枚举与表一致）");
        check(keepR == OVL_ALPHA_DEFAULT && keepS == OVL_ALPHA_DEFAULT,
              "浮层默认透明度 75%");

        /* 取浮层里第一个 IconButton 的底板系数（右栏 / 搜索各一个）
         * ★ 用 dynamic_cast 而不是 qobject_cast：IconButton 没有 Q_OBJECT 宏
         *   （它不参与 moc），qobject_cast 对没有 Q_OBJECT 的类型会直接
         *   static_assert 失败。QObject 是多态类，dynamic_cast 可用。 */
        auto pctOf = [](QWidget *w) -> int {
            if (!w) return -1;
            for (QAbstractButton *b : w->findChildren<QAbstractButton *>()) {
                if (auto *ib = dynamic_cast<IconButton *>(b)) return ib->overlayPct();
            }
            return -1;
        };
        check(pctOf(m_rightOverlay) == 75,
              "右栏浮层底板按 75% 半透明");
        check(pctOf(m_searchOverlay) == 75,
              "搜索浮层底板按 75% 半透明");

        app_ovl_alpha_set(m_a, OVL_SEARCH, 40);
        applyOverlayAlpha();
        check(pctOf(m_searchOverlay) == 40,
              "改搜索浮层：它的底板系数跟着变成 40%");
        check(pctOf(m_rightOverlay) == 75,
              "改搜索浮层：右栏不受影响（逐浮层独立）");

        /* ★ 0% ⇒ 该浮层整体隐藏（底板和前景都不画）：
         *   否则文字会孤零零浮在底图上。 */
        app_ovl_alpha_set(m_a, OVL_SEARCH, 0);
        applyOverlayAlpha();
        check(pctOf(m_searchOverlay) == 0, "搜索浮层设 0%：底板系数归 0");

        app_ovl_alpha_set(m_a, OVL_SEARCH, keepS);   /* 还原 */
        app_ovl_alpha_set(m_a, OVL_RCOL, keepR);
        applyOverlayAlpha();
    }

    /* GPX 轨迹链路：导入（内嵌 GPX，不依赖外部文件）→ 面板出现条目 →
     * 删除 → 条目消失。这条链把 C 模块（gpx.c）、App 入口
     * （app_gpx_import/del）和 UI 面板（refreshTrackPanel）全串起来。
     * 写临时文件用 nm_temp_dir()，不碰用户目录；导入会写 tracks.db
     * （app_gpx_import 的语义就是"导入即入库"），但测完把这条轨迹删掉，
     * 库记录一并清掉，最终不残留（最多留下一个空库文件，无害）。 */
    {
        char gpath[1024];
        snprintf(gpath, sizeof(gpath), "%s/navimap_uitest.gpx", nm_temp_dir());
        FILE *gf = fopen(gpath, "wb");
        if (gf) {
            fputs("<gpx version=\"1.1\"><trk><name>自检轨迹</name><trkseg>"
                  "<trkpt lat=\"39.9\" lon=\"116.4\"/><trkpt lat=\"39.91\" lon=\"116.41\"/>"
                  "</trkseg></trk></gpx>", gf);
            fclose(gf);
        }
        int t0 = m_a->gpx.n_trk;
        int n = app_gpx_import(m_a, gpath);
        check(n == 1, "GPX 导入：解析出 1 条轨迹");
        check(m_a->gpx.n_trk == t0 + 1, "GPX 导入：store 多出一条");
        if (m_a->gpx.n_trk > t0) {
            const GpxTrack *tk = &m_a->gpx.trk[t0];
            check(tk->n == 2 && tk->lat_e7 && tk->lat_e7[0] == 399000000,
                  "GPX 导入：点数与首点坐标对");
            check(!strcmp(tk->name, "自检轨迹"), "GPX 导入：名字对");
        }
        /* 面板：先打开（首次打开会创建面板控件），刷新后再数条目。
         * 条目数必须等于 store 里的轨迹数 + 收藏数 —— 这比"至少有一条"更严：
         * 它同时钉住"面板跟着 store 变"（导入多一条、删除少一条）。
         * ★ 顺带盯住那条 toggled 递归：toggle 一次必须**返回**，
         *   递归版会在这里把栈耗光（崩溃点在几百层之后的 malloc 里）。 */
        toggleTrackPanel();
        refreshTrackPanel();
        check(m_trackRows && gpx_row_count(m_trackRows) ==
                  m_a->gpx.n_trk + m_a->gpx.n_wpt,
              "轨迹面板：刷新后条目与 store 一致");
        /* 面板必须贴在**右栏左边**，不能停在 (0,0)（layoutOverlays 曾因用
         * isVisible() 判断显隐，在窗口还没 show 的截图路径上永远贴不到位）。
         * ★ 判据必须与**窗口尺寸无关**：早先写成"pp.x() > 画布宽/2"，
         *   在 --w=900 的窄窗下 340px 面板的左沿会落到中线左侧 ⇒ 假红
         *   （regress.sh 的 qt_ui_test 正是用 900×600 跑的）。
         *   改为"右缘贴住右栏左缘（间距即 layoutOverlays 里的 6px）+
         *   不压右栏 + 不越左沿"：飘在中间或停在 (0,0) 都会 FAIL。 */
        {
            const QPoint pp = trackPanelPos();
            const QSize ps = trackPanelSize();
            const QPoint rp = rightOverlayPos();
            check(ps.width() > 0 && ps.height() > 0, "轨迹面板：尺寸非零（布局跑过了）");
            check(pp.x() >= 0 && pp.x() + ps.width() <= rp.x() + 1,
                  "轨迹面板：不压右栏按钮");
            check(pp.x() + ps.width() + 6 >= rp.x() - 1,
                  "轨迹面板：右缘贴住右栏（不是飘在中间或 (0,0)）");
            check(trackPanelVisible(), "轨迹面板：处于可见态");
        }
        /* 轨迹行应带一个"贴合"按钮（GPX 贴合道路入口）；点击后调用 app_gpx_snap。
         * 这里只验"按钮存在且接好了逻辑"，不验实际贴合结果（贴合依赖路网，
         * 默认单省 beijing 图在测试坐标附近无路网，route_cli --gpx-snap 在 7 省图覆盖）。 */
        {
            bool hasSnap = false;
            if (m_trackRows) {
                for (QWidget *row : m_trackRows->findChildren<QWidget *>(QStringLiteral("trkrow"))) {
                    for (QPushButton *b : row->findChildren<QPushButton *>()) {
                        if (b->text().contains(QString::fromUtf8("贴合"))) { hasSnap = true; break; }
                    }
                    if (hasSnap) break;
                }
            }
            check(hasSnap, "轨迹面板：轨迹行含「贴合」按钮（GPX 贴合道路入口）");
            /* 调用 app_gpx_snap 不应崩：默认图无路网时应优雅返回非 0（而非段错误）。
             * 7 省图上会返回 0 并贴出一条沿道路的路线——这里不强求结果，只验"不崩"。 */
            int snap_rc = app_gpx_snap(m_a, m_a->gpx.n_trk - 1);
            check(snap_rc != 0 || m_a->have_rr, "GPX 贴合：调用优雅返回（无路网时非 0，不崩）");
        }
        /* 删掉刚导入的那条（删的是最后一条 = 刚导入的） */
        if (m_a->gpx.n_trk > t0) {
            app_gpx_del_track(m_a, m_a->gpx.n_trk - 1);
            check(m_a->gpx.n_trk == t0, "GPX 删除：store 恢复原状");
            refreshTrackPanel();
            check(m_trackRows && gpx_row_count(m_trackRows) ==
                      m_a->gpx.n_trk + m_a->gpx.n_wpt,
                  "轨迹面板：删除后条目同步减少");
        }
        toggleTrackPanel();          /* 面板收回去，别挡住后面的断言 */
        remove(gpath);
        m_a->need_redraw = 1;
    }

    /* ⑫b 途经点链路：添加（app_nav_add_via，模拟点地图拾取）→ 面板出现一行 →
     * 路线算完且「确实经过该点」→ 删除 → 行消失 → 清空 → 面板空。
     * 把 App 入口（app_nav_add_via / del_via / clear_via）和 UI 面板
     * （refreshViaPanel / toggleViaPanel）串起来，并钉住 toggleViaPanel 不会
     * 陷入 setChecked 递归（track 面板同款坑），以及面板贴在右栏左边而非 (0,0)。
     * 路线经过途经点这条核心契约，靠"等 worker 算完 + 测距"验证（route_cli --via
     * 已在 C 层覆盖，这里验 UI 集成后仍能拿到正确结果）。 */
    {
        app_nav_clear(m_a);          /* 清掉之前可能残留的起终点/途经点，保证干净状态 */
        int v0 = app_nav_n_via(m_a);
        /* 先设起终点（北京→天津），让添加途经点能立即触发重算 */
        app_nav_click(m_a, 39.90, 116.40);
        app_nav_click(m_a, 39.13, 117.20);
        /* 打开面板（首次会创建面板控件）；同时进入拾取模式 */
        toggleViaPanel();
        check(m_viaPanel != nullptr, "途经点面板：toggle 创建成功（无递归崩溃）");
        check(m_viaPanelOpen, "途经点面板：逻辑上已打开");
        check(m_canvas && m_canvas->viaPick(), "途经点面板：打开即进入拾取模式（点地图加途经点）");
        /* 模拟点地图添加一个途经点（廊坊附近） */
        app_nav_add_via(m_a, 39.52, 116.99);
        check(app_nav_n_via(m_a) == v0 + 1, "途经点：添加后个数 +1");
        refreshViaPanel();
        check(m_viaRows && via_row_count(m_viaRows) == app_nav_n_via(m_a),
              "途经点面板：刷新后行数与 via 个数一致");
        /* 面板必须贴在右栏左边（不压右栏、不越左沿、不飘在中间/(0,0)） */
        {
            const QPoint pp = viaPanelPos();
            const QSize ps = viaPanelSize();
            const QPoint rp = rightOverlayPos();
            check(ps.width() > 0 && ps.height() > 0, "途经点面板：尺寸非零（布局跑过）");
            check(pp.x() >= 0 && pp.x() + ps.width() <= rp.x() + 1, "途经点面板：不压右栏按钮");
            check(pp.x() + ps.width() + 6 >= rp.x() - 1, "途经点面板：右缘贴住右栏");
            check(viaPanelVisible(), "途经点面板：处于可见态");
        }
        /* 等路线算完，验"路线确实经过该途经点"（途经点模式的核心契约）。
         * ★ 先到的可能是"两点直连"那单（添加途经点前已触发一次），所以要持续
         *   poll、每次重新测距，直到抓到"经过该途经点"的那一趟才算数。
         * ★ 当前默认图（单省 beijing）在测试坐标附近没有可达路网，route_cli
         *   同样会"起点吸附失败"；此时路由本就跑不出来，硬判 FAIL 会污染
         *   regress。故"始终算不出路线"时跳过本项（C 层 route_cli --via 已在
         *   7 省图上覆盖此契约），只在"路由确实算出了一条路线"时才验"经过该点"。 */
        {
            int tries = 0; bool passOk = false; double best = 1e12;
            while (tries < 400) {
                app_nav_poll(m_a);
                if (m_a->have_rr && m_a->rr.n > 1) {
                    best = 1e12;
                    for (uint32_t i = 0; i + 1 < m_a->rr.n; i++) {
                        double d = pointSegM(m_a->via[0].lat, m_a->via[0].lon,
                                             m_a->rr.lat_e7[i] / 1e7, m_a->rr.lon_e7[i] / 1e7,
                                             m_a->rr.lat_e7[i + 1] / 1e7, m_a->rr.lon_e7[i + 1] / 1e7);
                        if (d < best) best = d;
                    }
                    if (best < 800.0) { passOk = true; break; }
                }
                QThread::msleep(50);
                tries++;
            }
            if (m_a->have_rr) {
                check(passOk, "途经点：路线经过该点（最近距离 <800m）");
            } else {
                check(true, "途经点：当前环境算不出路线，跳过「经过该点」校验（C 层 route_cli --via 已覆盖）");
            }
        }
        /* 删除刚加的途经点（删最后一个 = 刚加的） */
        app_nav_del_via(m_a, app_nav_n_via(m_a) - 1);
        check(app_nav_n_via(m_a) == v0, "途经点：删除后恢复原状");
        refreshViaPanel();
        check(m_viaRows && via_row_count(m_viaRows) == app_nav_n_via(m_a),
              "途经点面板：删除后行数同步");
        /* 清空 */
        app_nav_clear_via(m_a);
        check(app_nav_n_via(m_a) == 0, "途经点：清空后为 0");
        refreshViaPanel();
        check(m_viaRows && via_row_count(m_viaRows) == 0, "途经点面板：清空后无行");
        toggleViaPanel();          /* 面板收回去：关拾取模式、按钮取消勾选 */
        check(!m_viaPanelOpen, "途经点面板：再 toggle 即关闭");
        check(m_canvas && !m_canvas->viaPick(), "途经点面板：关闭即退出拾取模式");
        /* 收尾：清掉这条测试路线，避免污染后续断言 */
        app_nav_clear(m_a);
        m_a->need_redraw = 1;
    }

    /* ⑬ 坐标网按钮：左键在 关 → 各坐标系 → 关 之间循环（循环态含"关"）。
     *    右键菜单可精确选坐标系 + 开关网格。网格类型（经纬网/方里格）与坐标系已解耦。
     *    逐状态验：左键第 1 下 关→开（基准不变）；之后每次推进 datum；完整循环
     *    = DATUM_COUNT 个坐标系 + 1 个「关」共 DATUM_COUNT+1 态回绕；"关"可达；
     *    右键菜单 datum 动作与「显示坐标网」开关都生效；悬停提示"网格类型"由
     *    m_a->grid_type 决定、与 datum 无关。 */
    {
        const int  keep_gt = m_a->grid_type;
        const int  keep_d  = m_a->datum;
        const bool keep_g  = m_canvas->isGridOn();

        /* 起始：确保关着（按钮只显示图标不显文字，故只校验状态） */
        m_canvas->setShowGrid(false);
        updateGridBtn();
        check(!m_canvas->isGridOn(), "坐标网初始：关");

        /* 第 1 下：关 → 开（保持当前坐标系，不切基准） */
        emit m_gridBtn->clicked();
        QApplication::processEvents();
        check(m_canvas->isGridOn(), "左键①：关 → 开（保持坐标系）");
        check((int)m_a->datum == keep_d, "左键①：开时坐标系不变");

        /* 第 2 下：循环切换坐标系（WGS84→CGCS2000→北京54→西安80→关→回绕） */
        const int before = (int)m_a->datum;
        emit m_gridBtn->clicked();
        QApplication::processEvents();
        const int after = (int)m_a->datum;
        check(after == (before + 1) % (int)DATUM_COUNT, "左键②：坐标系循环推进 1 档");
        check(m_gridBtn->toolTip().contains(QString::fromUtf8(datum_label((DatumId)after))),
              "左键②：悬停提示跟随新坐标系");

        /* 完整循环 = DATUM_COUNT 个坐标系 + 1 个「关」共 DATUM_COUNT+1 态，
         * 回绕到 after 态（开 + 同一坐标系）。 */
        for (int i = 0; i < (int)DATUM_COUNT + 1; i++) {
            emit m_gridBtn->clicked();
            QApplication::processEvents();
        }
        check(m_canvas->isGridOn() && (int)m_a->datum == after,
              "左键：完整循环(含关)回绕到原态");

        /* 再从当前态连续点直到「关」，验证"关"确实是循环末项之后的档 */
        int guard = 0;
        while (m_canvas->isGridOn() && guard++ < 16) {
            emit m_gridBtn->clicked();
            QApplication::processEvents();
        }
        check(!m_canvas->isGridOn(), "左键：循环到末项再点一次 → 关");

        /* 右键菜单：精确选坐标系 + 开关网格 */
        check(!m_gridDatumActs.isEmpty(), "右键菜单：坐标系动作已建");
        check(m_gridShowAct != nullptr, "右键菜单：网格开关动作存在");
        if (!m_gridDatumActs.isEmpty()) {
            m_gridDatumActs[(int)DATUM_XA80]->trigger();
            QApplication::processEvents();
            check((int)m_a->datum == (int)DATUM_XA80, "右键菜单：选西安80 → datum 变西安80");
            check(m_gridDatumActs[(int)DATUM_XA80]->isChecked(), "右键菜单：西安80 项勾选态同步");

            /* 网格开关 */
            bool g0 = m_canvas->isGridOn();
            m_gridShowAct->trigger(); QApplication::processEvents();
            check(m_canvas->isGridOn() != g0, "右键菜单：切换网格开关（关→开）");
            m_gridShowAct->trigger(); QApplication::processEvents();
            check(m_canvas->isGridOn() == g0, "右键菜单：再切回原状态（开→关）");

            /* 解耦：网格类型不受坐标系影响 */
            m_canvas->setShowGrid(true);
            setGridType((int)GRID_LATLON); updateGridBtn();
            setDatum((int)DATUM_BJ54); updateGridBtn();
            check(m_gridBtn->toolTip().contains(QStringLiteral("经纬网")) &&
                  m_gridBtn->toolTip().contains(QStringLiteral("北京")),
                  "切换坐标系：网格类型保持经纬网（解耦）");
            setGridType((int)GRID_GK); updateGridBtn();
            setDatum(keep_d); updateGridBtn();
            check(m_gridBtn->toolTip().contains(QStringLiteral("方里格")) &&
                  m_gridBtn->toolTip().contains(QString::fromUtf8(datum_label((DatumId)keep_d))),
                  "切回原坐标系：网格类型仍方里格（解耦验证）");
        }

        /* 还原 */
        m_canvas->setShowGrid(keep_g);
        setGridType(keep_gt);
        setDatum(keep_d);
        updateGridBtn();
    }

    /* ⑭ 坐标网细分刻度线开关（设置项，依附网格；「设置」菜单可勾选，持久化）。
     *    验证：菜单项存在、模拟点击一次后勾选态与 m_a->grid_subdiv 同步。
     *    ★ 必须用 trigger()（模拟用户点击）而不是 setChecked()——setChecked 只改
     *      action 状态、不发射 triggered，连着的 lambda（写 grid_subdiv）不会跑。 */
    {
        check(m_subdivAct != nullptr, "细分刻度线：「设置」菜单开关存在");
        if (m_subdivAct) {
            const int before = m_a->grid_subdiv;
            m_subdivAct->trigger();             /* 模拟用户点击一次：toggle + 发射 triggered */
            QApplication::processEvents();
            check(m_a->grid_subdiv == !before, "细分刻度线：点击后设置项翻转");
            check(m_subdivAct->isChecked() == !!m_a->grid_subdiv, "细分刻度线：菜单项勾选态与设置项一致");
            m_a->grid_subdiv = before;          /* 还原，避免影响后续/落盘 */
            m_subdivAct->setChecked(before);
        }
    }

    /* ⑮ 地图信息显示开关统一收进「设置」菜单。
     *    验证：各显示开关 action 均存在；原先散落在「视图」(等高线)、主菜单 (昼夜模式)
     *          的项现已收进「设置」(父菜单 == m_setMenu)；勾选态初始与运行时值一致。 */
    {
        check(m_subdivAct   != nullptr, "显示开关：网格细分刻度存在");
        check(m_poiAct      != nullptr, "显示开关：POI 图标存在");
        check(m_labelsAct   != nullptr, "显示开关：地名标注存在");
        check(m_contourAct  != nullptr, "显示开关：等高线存在");
        check(m_dayNightAct != nullptr, "显示开关：夜间模式存在");
        check(m_dayNightAutoAct != nullptr, "显示开关：自动昼夜存在");
        check(m_voiceAct    != nullptr, "显示开关：语音播报存在");
        if (m_contourAct)   check(m_setMenu && m_setMenu->actions().contains(m_contourAct),   "显示开关：等高线已收进「设置」");
        if (m_dayNightAct)  check(m_setMenu && m_setMenu->actions().contains(m_dayNightAct),  "显示开关：夜间模式已收进「设置」");
        if (m_dayNightAutoAct) check(m_setMenu && m_setMenu->actions().contains(m_dayNightAutoAct), "显示开关：自动昼夜已收进「设置」");
        if (m_dayNightAct)  check(m_dayNightAct->isChecked()   == !!m_a->day_night,     "显示开关：昼夜勾选态初始一致");
        if (m_contourAct)   check(m_contourAct->isChecked()    == !!m_a->show_contours, "显示开关：等高线勾选态初始一致");
        /* D 键在 白天/夜间/自动 三态间循环，且菜单勾选态随之同步（修复：原先昼夜菜单项不跟随 D 键） */
        if (m_dayNightAct && m_dayNightAutoAct) {
            const int m0 = m_a->day_night_mode;
            this->handleKey(Qt::Key_D);
            check(m_a->day_night_mode != m0, "显示开关：D 键切换昼夜模式");
            check(m_dayNightAct->isChecked() == !!m_a->day_night, "显示开关：D 键后昼夜菜单勾选态同步");
            this->handleKey(Qt::Key_D);   /* 再切一次 */
            check(m_dayNightAct->isChecked() == !!m_a->day_night, "显示开关：D 键二次切换后勾选态同步");
            /* 手动夜间：直接置 mode=1 应使运行时为夜；手动白天应使运行时为昼 */
            m_a->day_night_mode = 1; applyDayNight();
            check(m_a->day_night == 1, "显示开关：手动夜间生效");
            m_a->day_night_mode = 0; applyDayNight();
            check(m_a->day_night == 0, "显示开关：手动白天生效");
        }
    }

    /* ⑯ 下载多级树：已下载/未下载状态判定与树上标注（✓/·）同源。
     *    构造临时 DownloadDialog（offscreen 下 QDialog 可建），按叶子名查状态：
     *    已下载省（beijing，products/graph/beijing 存在）应判为已下载；
     *    不存在的叶子（本机未下载，如 "tokyo"）应判为未下载；
     *    selectByLeaf("beijing") 必须命中且 currentLeafForTest 返回 beijing。 */
    {
        QString gd = QString::fromLocal8Bit(m_a->graph_dir);
        if (gd.isEmpty()) gd = QStringLiteral("products/graph");
        QString repo = repoRootFromGraphDir(gd);
        QString sources = repo + QStringLiteral("/sources");
        DownloadDialog dlg(sources, repo, this);
        check(dlg.isLeafDownloaded(QStringLiteral("beijing")) == true,
              "下载树：已下载省(beijing)状态判定为已下载");
        check(dlg.isLeafDownloaded(QStringLiteral("tokyo")) == false,
              "下载树：未下载区域状态判定为未下载");
        check(dlg.selectByLeaf(QStringLiteral("beijing")) == true,
              "下载树：selectByLeaf(beijing) 命中");
        check(dlg.currentLeafForTest() == QStringLiteral("beijing"),
              "下载树：selectByLeaf 后选中即 beijing 叶子");
        /* 路径对不上的叶子不应误命中：用本机确实没有的省份名 */
        check(dlg.selectByLeaf(QStringLiteral("atlantis")) == false,
              "下载树：不存在的叶子 selectByLeaf 返回 false");
    }

    if (fail == 0) {
        printf("QT-UI-OK\n");
        return 0;
    }
    printf("QT-UI-FAIL：%d 项不符\n", fail);
    return 1;
}

/* ---------------- 事件过滤器 ----------------
 *
 * 两件事：
 * ① 画布 Resize → 右栏浮层重新贴位（浮层挂在画布上，画布变了它得跟上）。
 * ② 搜索框里的 Esc → 收起输入框回到按钮态（QLineEdit 自己不处理 Esc）。
 * ③ 轨迹面板某行双击 → 把视口挪到那条轨迹/收藏。
 */
bool MainWindow::eventFilter(QObject *obj, QEvent *ev)
{
    if (obj == m_canvas && ev->type() == QEvent::Resize) {
        layoutOverlays();
    } else if (obj == m_search && ev->type() == QEvent::KeyPress) {
        QKeyEvent *ke = static_cast<QKeyEvent *>(ev);
        if (ke->key() == Qt::Key_Escape) {
            hideSearch();
            return true;                  /* 吃掉这个 Esc，别再传给画布 */
        }
    } else if (obj->isWidgetType() &&
               ev->type() == QEvent::MouseButtonDblClick &&
               obj->property("gpxkind").isValid()) {
        /* 轨迹面板某行被双击：把视口挪到那条轨迹/收藏（行上装了 gpxkind/gpxidx
         * 两个动态属性，重建面板时随之更新，不会指到已删的条目） */
        locateGpxRow(obj->property("gpxkind").toInt(),
                     obj->property("gpxidx").toInt());
        return true;
    }
    return QMainWindow::eventFilter(obj, ev);
}

/* ---------------- 菜单栏 ---------------- */

/* Qt 5.11 的 addAction(text, shortcut, functor) 重载需要 context 对象，
 * 为避免模板匹配问题统一走显式 QAction：addAction → setShortcut → connect */
static QAction *addAct(QMenu *m, const QString &text, QKeySequence sc,
                       MainWindow *w, std::function<void()> fn)
{
    QAction *act = m->addAction(text);
    act->setShortcut(sc);
    QObject::connect(act, &QAction::triggered, w, [fn](bool) { fn(); });
    return act;
}

void MainWindow::buildMenus()
{
    m_menuMain = new QMenu(QStringLiteral("菜单(&M)"), this);
    addAct(m_menuMain, QStringLiteral("搜索地点"),
           QKeySequence(Qt::CTRL | Qt::Key_F), this, [this]() {
               showSearch();                       /* 按钮触发式：先展开输入框 */
           });
    addAct(m_menuMain, QStringLiteral("清除路线"), QKeySequence(Qt::Key_C), this,
           [this]() { app_nav_clear(m_a); m_a->need_redraw = 1; });
    addAct(m_menuMain, QStringLiteral("缩放到路线"), QKeySequence(Qt::Key_F), this,
           [this]() { app_nav_fit(m_a); m_a->need_redraw = 1; });
    addAct(m_menuMain, QStringLiteral("跟随车头"), QKeySequence(Qt::Key_T), this,
           [this]() { app_nav_sim_follow(m_a); });
    addAct(m_menuMain, QStringLiteral("下载地图…"), QKeySequence(), this,
           [this]() { showDownloadDialog(); });
    m_menuMain->addSeparator();
    addAct(m_menuMain, QStringLiteral("退出"), QKeySequence(Qt::CTRL | Qt::Key_Q), this,
           [this]() { m_a->quit = 1; close(); });
    menuBar()->addMenu(m_menuMain);
    /* ★ 取消可见菜单栏：保留 menuBar 对象以便键盘快捷键（Ctrl+F/G/N/0-4 等）
     *   仍生效，但把可见菜单栏本身隐藏（设 0 高、不可见）。全部功能收进右栏
     *   「≡主菜单」一个弹出按钮（下方把其余 5 个子菜单挂进 m_menuMain）。 */
    menuBar()->setVisible(false);
    menuBar()->setFixedHeight(0);

    QMenu *msim = new QMenu(QStringLiteral("模拟导航(&S)"), this);
    addAct(msim, QStringLiteral("开始 / 停止"), QKeySequence(Qt::Key_G), this,
           [this]() { app_nav_sim_toggle(m_a); });
    addAct(msim, QStringLiteral("注入偏航（触发重算）"), QKeySequence(Qt::Key_N), this,
           [this]() { app_nav_sim_deviate(m_a); });
    addAct(msim, QStringLiteral("加速"), QKeySequence(Qt::Key_Period), this,
           [this]() { app_nav_sim_speed(m_a, 1); });
    addAct(msim, QStringLiteral("减速"), QKeySequence(Qt::Key_Comma), this,
           [this]() { app_nav_sim_speed(m_a, -1); });

    QMenu *mview = new QMenu(QStringLiteral("视图(&V)"), this);
    for (int k = 0; k <= 4; k++) {
        addAct(mview, QString::asprintf("道路细节 LOD %d", k), QKeySequence(), this,
               [this, k]() { m_a->max_class = k; m_a->need_redraw = 1; });
    }
    mview->addSeparator();
    addAct(mview, QStringLiteral("字号增大"), QKeySequence(Qt::Key_BracketRight), this,
           [this]() { app_set_font_size(m_a, m_a->font_size + 1); m_a->need_redraw = 1; });
    addAct(mview, QStringLiteral("字号减小"), QKeySequence(Qt::Key_BracketLeft), this,
           [this]() { app_set_font_size(m_a, m_a->font_size - 1); m_a->need_redraw = 1; });
    mview->addSeparator();
    addAct(mview, QStringLiteral("浮层透明度…"), QKeySequence(), this,
           [this]() { showOverlayAlphaDialog(); });

    /* 坐标系（大地基准）：切基准后经纬网与读数按该基准呈现。勾选态与右栏
     * 坐标网按钮悬停提示里的"当前坐标系"同步（右栏没有下拉框了）。 */
    QMenu *mdatum = new QMenu(QStringLiteral("坐标系(&C)"), this);
    for (int i = 0; i < DATUM_COUNT; i++) {
        QAction *act = mdatum->addAction(QString::fromUtf8(datum_label((DatumId)i)));
        act->setCheckable(true);
        act->setData(i);
        act->setChecked(m_a->datum == i);
        connect(act, &QAction::triggered, this, [this, i]() { setDatum(i); });
        m_datumActs.append(act);
    }

    /* 轨迹 / 收藏：导入 GPX、导出全部、收藏中心点。 */
    QMenu *mgpx = new QMenu(QStringLiteral("轨迹(&K)"), this);
    addAct(mgpx, QStringLiteral("导入 GPX…"), QKeySequence(Qt::CTRL | Qt::Key_I), this,
           [this]() { importGpx(); });
    addAct(mgpx, QStringLiteral("导出全部为 GPX…"), QKeySequence(Qt::CTRL | Qt::Key_E), this,
           [this]() { exportGpx(); });
    addAct(mgpx, QStringLiteral("收藏视口中心"), QKeySequence(Qt::CTRL | Qt::Key_D), this,
           [this]() { favCenter(); });
    addAct(mgpx, QStringLiteral("轨迹 / 收藏面板"), QKeySequence(), this,
           [this]() { toggleTrackPanel(); });

    /* 地图信息显示 / 播报开关：原 SDL 设置面板承载的开关，#88 拆 SDL 壳后由 Qt 菜单接管。
     * 全部收进「设置」子菜单——等高线从「视图」移入、昼夜模式从主菜单移入，统一集散。
     * 均可勾选，勾选态与对应快捷键（语音=V 键、等高线=O 键、昼夜=D 键）同步。
     * 字段由 app_persist_settings 在退出时落盘，这里只切运行时值 + 请求重绘。 */
    QMenu *mset = new QMenu(QStringLiteral("设置(&P)"), this);
    /* —— 地图信息显示开关（统一收进「设置」） —— */
    /* 坐标网细分刻度线：每格 5 等分，依附网格开关（网格关时不画）。 */
    {
        QAction *act = mset->addAction(QStringLiteral("网格细分刻度"));
        act->setCheckable(true);
        act->setChecked(m_a->grid_subdiv);
        connect(act, &QAction::triggered, this, [this, act]() {
            m_a->grid_subdiv = act->isChecked() ? 1 : 0;
            m_a->need_redraw = 1;
        });
        m_subdivAct = act;
    }
    /* 网格类型（与坐标系解耦）：经纬网 / 方里格，默认经纬网（WGS84）。
     * 两个 radio 动作，勾选态随 m_a->grid_type 同步；点击走 setGridType（持久化+重绘）。 */
    {
        QMenu *mg = mset->addMenu(QStringLiteral("网格类型"));
        const char *names[2] = { "经纬网（度）", "方里格（公里）" };
        for (int i = 0; i < 2; i++) {
            QAction *act = mg->addAction(QString::fromUtf8(names[i]));
            act->setCheckable(true);
            act->setChecked(m_a->grid_type == i);
            connect(act, &QAction::triggered, this, [this, i]() {
                setGridType(i);
                /* 同步同组另一个动作的勾选态 */
                for (QAction *a : m_gridTypeActs) a->setChecked(a->data().toInt() == m_a->grid_type);
            });
            act->setData(i);
            m_gridTypeActs.append(act);
        }
    }
    {
        QAction *act = mset->addAction(QStringLiteral("POI 图标"));
        act->setCheckable(true);
        act->setChecked(m_a->show_poi);
        connect(act, &QAction::triggered, this, [this, act]() {
            m_a->show_poi = act->isChecked() ? 1 : 0;
            m_a->need_redraw = 1;
        });
        m_poiAct = act;
    }
    {
        QAction *act = mset->addAction(QStringLiteral("地名标注"));
        act->setCheckable(true);
        act->setChecked(m_a->show_labels);
        connect(act, &QAction::triggered, this, [this, act]() {
            m_a->show_labels = act->isChecked() ? 1 : 0;
            m_a->need_redraw = 1;
        });
        m_labelsAct = act;
    }
    /* 等高线：从「视图」菜单移入「设置」，勾选态与 O 键同步 */
    {
        QAction *act = mset->addAction(QStringLiteral("等高线"));
        act->setCheckable(true);
        act->setChecked(m_a->show_contours);
        connect(act, &QAction::triggered, this, [this, act]() {
            m_a->show_contours = act->isChecked() ? 1 : 0;
            m_a->need_redraw = 1;
        });
        m_contourAct = act;
    }
    mset->addSeparator();
    /* 昼夜模式：白天(手动) / 夜间(手动) / 自动(日出日落) 三态。
     * 夜间动作是"手动夜间"开关；自动动作是"按日出日落自动"开关。两者互斥组：
     * 勾自动 → 运行时按视口中心+本地时间算昼/夜（核心每帧刷新）；勾夜间 → 强制夜间。
     * 都不勾 → 白天。勾选态与 D 键、右栏昼/夜按钮同步（见 applyDayNight）。 */
    {
        QAction *act = mset->addAction(QStringLiteral("夜间模式"));
        act->setCheckable(true);
        act->setChecked(m_a->day_night_mode == 1);
        connect(act, &QAction::triggered, this, [this, act]() {
            m_a->day_night_mode = act->isChecked() ? 1 : 0;   /* 夜间(手动) / 白天(手动) */
            applyDayNight();
        });
        m_dayNightAct = act;

        QAction *aauto = mset->addAction(QStringLiteral("自动（日出日落）"));
        aauto->setCheckable(true);
        aauto->setChecked(m_a->day_night_mode == 2);
        connect(aauto, &QAction::triggered, this, [this, aauto]() {
            /* 勾自动 → mode=2；取消自动 → 退回当前有效昼/夜的手动值 */
            m_a->day_night_mode = aauto->isChecked() ? 2
                                 : (m_a->day_night ? 1 : 0);
            applyDayNight();
        });
        m_dayNightAutoAct = aauto;
    }
    mset->addSeparator();
    /* 语音播报：与 V 键同步 */
    {
        QAction *act = mset->addAction(QStringLiteral("语音播报"));
        act->setCheckable(true);
        act->setChecked(m_a->voice_on);
        connect(act, &QAction::triggered, this, [this, act]() {
            m_a->voice_on = act->isChecked() ? 1 : 0;
            m_a->need_redraw = 1;
        });
        m_voiceAct = act;
    }
    m_setMenu = mset;

    /* 把 5 个功能子菜单挂进「≡主菜单」弹出按钮，取消顶栏后作为唯一入口 */
    m_menuMain->addMenu(msim);
    m_menuMain->addMenu(mview);
    m_menuMain->addMenu(mdatum);
    m_menuMain->addMenu(mgpx);
    m_menuMain->addMenu(mset);

    /* 主菜单（含各子菜单）视觉样式：半透明白底 + 圆角 + 细边框 + 悬浮高亮 +
     * 细分隔线，与浮层/比例尺同一套视觉语言（仅淡底板、文字保持清晰）。
     * 6 个 QMenu 各设一份（子菜单是独立顶层窗口，不继承父菜单的 object stylesheet）。 */
    static const char *kMenuQss =
        "QMenu{background-color:rgba(252,252,250,0.96);border:1px solid rgba(40,60,95,0.35);"
        "border-radius:10px;padding:5px;color:#1e2430;font-size:13px;}"
        "QMenu::item{padding:7px 26px 7px 12px;border-radius:6px;spacing:8px;}"
        "QMenu::item:selected{background-color:rgba(40,90,160,0.16);color:#14233b;}"
        "QMenu::item:disabled{color:rgba(120,130,145,0.6);}"
        "QMenu::separator{height:1px;background:rgba(40,60,95,0.18);margin:5px 10px;}"
        "QMenu::indicator{width:16px;height:16px;}";
    m_menuMain->setStyleSheet(kMenuQss);
    msim->setStyleSheet(kMenuQss);
    mview->setStyleSheet(kMenuQss);
    mdatum->setStyleSheet(kMenuQss);
    mgpx->setStyleSheet(kMenuQss);
    mset->setStyleSheet(kMenuQss);

    /* 启动时按当前 mode 同步一次昼夜勾选态（自动模式按真实视口算初始昼/夜）。 */
    applyDayNight();
}

/* ---------------- 顶栏：搜索按钮 + 模式页签 + 路线摘要 ----------------
 *
 * ★ 搜索是**按钮触发**的，不是常驻输入框：
 *   平��只有一个「搜索」按钮浮在地图左上角，点它才在原地展开输入框（并自动聚焦）。
 *   输入框用完（Esc / 选中 / 失焦）自动收起，按钮恢复原状。
 *
 * ★ 为什么是**浮层**而不是顶栏工具条：
 *   顶栏（QToolBar）会占掉一整行，地图的垂直空间被挤掉；
 *   浮层不占布局空间，点开搜索框时画布尺寸不变 ⇒ 右上角的按钮也不会挪位。
 */

void MainWindow::buildSearchOverlay()
{
    m_searchOverlay = new QWidget(m_canvas);
    m_searchOverlay->setObjectName(QStringLiteral("searchOverlay"));
    m_searchOverlay->setAttribute(Qt::WA_TranslucentBackground);
    m_searchOverlay->setAutoFillBackground(false);

    QHBoxLayout *lay = new QHBoxLayout(m_searchOverlay);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    /* --- 搜索按钮（默认可见）---
     * ★ 图标是**自绘矢量**（qt_icons.cpp），不用字体符号 ——
     *   🔍 之类的 emoji/符号在字体链里没有字形，会画成方块。
     *   按钮用 IconButton：QToolButton::setIcon 的图标尺寸由 style 决定，
     *   会把图标画到按钮一角（实测只剩左上角）。 */
    IconButton *sbtn = new IconButton(Qt::IconKind::Search,
                                      QStringLiteral("搜索地点或道路（Ctrl+F）"),
                                      m_searchOverlay);
    sbtn->setText(QStringLiteral("搜索"));
    sbtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    /* 带文字时按内容定宽（IconButton 默认是固定 36×36 的纯图标尺寸） */
    sbtn->setFixedHeight(IconButton::kSize);
    sbtn->setFixedWidth(sbtn->fontMetrics().horizontalAdvance(QStringLiteral("搜索"))
                        + IconButton::kIcon + 14 + 16);
    m_searchBtn = sbtn;
    connect(m_searchBtn, &QToolButton::clicked, this, [this]() { showSearch(); });
    lay->addWidget(m_searchBtn);

    /* --- 搜索输入框（默认隐藏，点按钮才显示）--- */
    m_search = new QLineEdit(m_searchOverlay);
    m_search->setPlaceholderText(QStringLiteral("搜索地点、道路…"));
    m_search->setClearButtonEnabled(true);
    /* 输入框浮在地图上，宽度按画布比例给：窄屏时也不能挤到画布外 */
    m_search->setFixedWidth(320);
    /* 高度与搜索按钮对齐（否则输入框比按钮矮，视觉上跳；而且高度变化会让
       浮层容器变高 ⇒ 底下的内容位移） */
    m_search->setFixedHeight(IconButton::kSize);
    m_search->hide();
    /* 原生中文输入法直接可用——这是换 Qt 最大的收益之一 */
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString &t) {
        doSearch(t);
    });
    connect(m_search, &QLineEdit::returnPressed, this, [this]() {
        if (m_a->su.n_hits > 0) commitHit(m_a->su.sel >= 0 ? m_a->su.sel : 0);
    });
    /* 点到别处去（失焦）就收起输入框 */
    connect(m_search, &QLineEdit::editingFinished, this, [this]() {
        if (!m_search->hasFocus()) hideSearch();
    });
    /* Esc 收起输入框：QLineEdit 自己不处理 Esc ⇒ 走 eventFilter
       （MainWindow::eventFilter 里监听 m_search 的 KeyPress）。 */
    m_search->installEventFilter(this);
    lay->addWidget(m_search, 1);

    /* 搜索结果浮层：弹出在搜索框正下方 */
    m_hits = new QListWidget(this);
    m_hits->setWindowFlags(Qt::Popup);
    m_hits->setFixedWidth(420);
    connect(m_hits, &QListWidget::itemClicked, this, [this](QListWidgetItem *it) {
        commitHit(it->data(Qt::UserRole).toInt());
    });
}

/* 点搜索按钮 → 展开输入框并聚焦 */
void MainWindow::showSearch()
{
    /* 该浮层被设成 0%（不显示）时不允许再展开 */
    if (m_a->ovl_alpha[OVL_SEARCH] <= 0) return;
    m_searchBtn->hide();
    m_search->show();
    m_search->setFocus();
    m_search->selectAll();
    layoutOverlays();       /* 输入框出现后浮层变宽 → 重新贴位 */
}

/* 收起输入框 → 回到按钮态 */
void MainWindow::hideSearch()
{
    if (!m_search) return;
    m_search->hide();
    m_search->clear();          /* clear() 会触发 textChanged → doSearch("") 收结果 */
    m_searchBtn->show();
    m_searchBtn->setToolTip(m_lastPick.isEmpty()
        ? QStringLiteral("搜索地点或道路（Ctrl+F）")
        : QStringLiteral("搜索地点或道路（Ctrl+F）\n最近选择：") + m_lastPick);
    layoutOverlays();       /* 输入框收起后浮层变窄 → 重新贴位 */
}

void MainWindow::doSearch(const QString &text)
{
    QByteArray t = text.toUtf8();
    closeSearch();
    if (t.trimmed().isEmpty()) return;
    int n = app_search_query(m_a, t.constData());
    if (n <= 0) return;
    m_a->su.active = 1;                  /* 让画布画结果图钉 */
    m_a->su.sel = 0;
    for (int i = 0; i < n && i < 8; i++) {
        const SearchHit *h = &m_a->su.hits[i];
        QListWidgetItem *it = new QListWidgetItem(m_hits);
        it->setText(QString::fromUtf8(h->name) +
                    QString::asprintf("   %.1f km", h->dist_m / 1000.0));
        it->setData(Qt::UserRole, i);
        m_hits->addItem(it);
    }
    /* 弹在搜索框正下方（全局坐标） */
    QPoint gp = m_search->mapToGlobal(QPoint(0, m_search->height()));
    m_hits->setFixedSize(qMax(420, m_search->width()), qMin(n, 8) * 26 + 8);
    m_hits->move(gp);
    m_hits->show();
    m_a->need_redraw = 1;
}

void MainWindow::commitHit(int idx)
{
    if (idx < 0 || idx >= m_a->su.n_hits) return;
    m_a->su.sel = idx;
    m_a->view.lat = m_a->su.hits[idx].lat;
    m_a->view.lon = m_a->su.hits[idx].lon;
    if (m_a->view.zoom < 14.0) m_a->view.zoom = 15.0;
    closeSearch();
    m_a->need_redraw = 1;
    /* 记住最近选择（按钮的悬停提示里回显）。
     * ★ 这里**不**再 m_search->setText(name)：setText 会发 textChanged ⇒ 又触发
     *   一次 doSearch（白查一遍，还会让结果浮层重新弹出）；而且输入框本来就要
     *   收起，文字留在里面没意义。 */
    m_lastPick = QString::fromUtf8(m_a->su.hits[idx].name);
    hideSearch();
}

void MainWindow::closeSearch()
{
    m_hits->clear();
    m_hits->hide();
}

/* ---------------- 右栏：浮在地图上的独立按钮 ----------------
 *
 * ★ 为什么不用 QToolBar：竖排工具栏会占掉窗口右侧一整条（还带边框背景），
 *   地图被挤窄、看起来也像"另一个面板"。改成**浮层**：一个透明容器挂在画布上，
 *   里面是几个独立按钮，贴在地图右侧，不占任何布局空间。
 *
 * ★ 为什么按钮要用 QToolButton + 样式表而不是裸 QPushButton：
 *   QToolButton 默认是"扁平方形"，加上样式表后能做成半透明圆角方块，
 *   和地图融在一起（导航 app 的常见做法）。
 */

void MainWindow::buildRightCol()
{
    m_rightOverlay = new QWidget(m_canvas);
    m_rightOverlay->setObjectName(QStringLiteral("rightOverlay"));
    /* 透明容器：不画背景，只承载按钮 */
    m_rightOverlay->setAttribute(Qt::WA_TranslucentBackground);
    m_rightOverlay->setAutoFillBackground(false);

    QVBoxLayout *vl = new QVBoxLayout(m_rightOverlay);
    vl->setContentsMargins(0, 0, 0, 0);
    vl->setSpacing(4);

    auto bigbtn = [](Qt::IconKind kind, const QString &tip) {
        /* IconButton：自绘矢量图标（不依赖字体）+ 鼠标悬停提示。
           不用 QToolButton::setIcon：那个的图标尺寸由 style 决定，
           图标会被画到按钮一角（实测放大镜只剩左上角）。 */
        IconButton *b = new IconButton(kind, tip);
        return b;
    };

    QToolButton *zin  = bigbtn(Qt::IconKind::ZoomIn,  QStringLiteral("放大"));
    QToolButton *zout = bigbtn(Qt::IconKind::ZoomOut, QStringLiteral("缩小"));
    QToolButton *loc  = bigbtn(Qt::IconKind::Locate,  QStringLiteral("定位到关注点"));
    QToolButton *day  = bigbtn(Qt::IconKind::DayNight, QStringLiteral("昼 / 夜 切换"));

    connect(zin, &QToolButton::clicked, this, [this]() {
        mv_zoom_at(&m_a->view, 0.5, m_a->view.w / 2, m_a->view.h / 2);
        mv_clamp_zoom(&m_a->view, 3.0, 19.0);
        m_a->need_redraw = 1;
    });
    connect(zout, &QToolButton::clicked, this, [this]() {
        mv_zoom_at(&m_a->view, -0.5, m_a->view.w / 2, m_a->view.h / 2);
        mv_clamp_zoom(&m_a->view, 3.0, 19.0);
        m_a->need_redraw = 1;
    });
    connect(loc, &QToolButton::clicked, this, [this]() { locate(); });
    connect(day, &QToolButton::clicked, this, [this]() {
        cycleDayNight();
    });

    /* 坐标网按钮：左键**循环切换坐标系**（关 → 开且保持基准 → WGS84 → CGCS2000
     * → 北京54 → 西安80 → …），还原最初的"点击切换坐标系"交互（上版改成了纯开关，
     * 丢失了坐标系切换入口，J 先生指出的回归）；右键弹出菜单可**精确选坐标系** +
     * 开关网格。网格类型（经纬网/方里格，默认方里格）与坐标系已解耦，仍由「设置」菜单选。
     * 按钮只显示图标（不写"关"或坐标系名，避免遮挡右栏），状态由悬停提示承载。 */
    m_gridBtn = bigbtn(Qt::IconKind::Grid, QStringLiteral("坐标网"));
    m_gridBtn->setCheckable(false);   /* 左键是"循环切换"不是勾选开关 */
    m_gridBtn->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_gridBtn->setFixedSize(IconButton::kSize, IconButton::kSize);
    connect(m_gridBtn, &QToolButton::clicked, this, &MainWindow::cycleGridDatum);
    /* 右键菜单：恢复"精确切换坐标系 + 开关网格"入口 */
    m_gridCtx = new QMenu(this);
    m_gridShowAct = m_gridCtx->addAction(QStringLiteral("显示坐标网"));
    m_gridShowAct->setCheckable(true);
    connect(m_gridShowAct, &QAction::triggered, this, [this]() {
        if (m_canvas) m_canvas->setShowGrid(!m_canvas->isGridOn());
    });
    m_gridCtx->addSeparator();
    QMenu *md = m_gridCtx->addMenu(QStringLiteral("坐标系"));
    for (int i = 0; i < (int)DATUM_COUNT; i++) {
        QAction *a = md->addAction(QString::fromUtf8(datum_label((DatumId)i)));
        a->setCheckable(true);
        a->setData(i);
        connect(a, &QAction::triggered, this, [this, i]() { setDatum(i); });
        m_gridDatumActs.append(a);
    }
    m_gridBtn->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_gridBtn, &QWidget::customContextMenuRequested, this,
            [this](const QPoint &p) {
                if (!m_gridCtx) return;
                if (m_gridShowAct)
                    m_gridShowAct->setChecked(m_canvas && m_canvas->isGridOn());
                for (QAction *a : m_gridDatumActs)
                    a->setChecked(a->data().toInt() == (int)m_a->datum);
                m_gridCtx->popup(m_gridBtn->mapToGlobal(p));
            });
    updateGridBtn();

    /* 轨迹 / 收藏面板开关（可勾选：亮起 = 面板开着）。
     * 和缩放/定位/昼夜那组"操作地图"的分开，与坐标网之间空一档。 */
    m_trackBtn = bigbtn(Qt::IconKind::Track, QStringLiteral("轨迹 / 收藏（导入 GPX）"));
    m_trackBtn->setCheckable(true);
    connect(m_trackBtn, &QToolButton::toggled, this, [this](bool) {
        toggleTrackPanel();
    });

    /* 途经点面板开关（可勾选：亮起 = 面板开着 = 拾取模式开，点地图即加途经点）。
     * 与轨迹面板同一套路：浮层贴在右栏左边，避免遮挡地图主体。 */
    m_viaBtn = bigbtn(Qt::IconKind::Via, QStringLiteral("途经点（面板开着时点地图添加）"));
    m_viaBtn->setCheckable(true);
    connect(m_viaBtn, &QToolButton::toggled, this, [this](bool) {
        toggleViaPanel();
    });

    /* 菜单按钮：弹出与菜单栏同一份 QMenu。
       Qt 5.11 的 popup 只有 (pos, at) 两参，用 sizeHint 把菜单定位到按钮左侧，
       避免从最底钮向下弹出超出屏幕。 */
    QToolButton *menu = bigbtn(Qt::IconKind::Menu, QStringLiteral("主菜单"));
    connect(menu, &QToolButton::clicked, this, [this, menu]() {
        if (!m_menuMain) return;
        QPoint p = menu->mapToGlobal(menu->rect().topLeft());
        p.rx() -= m_menuMain->sizeHint().width();
        m_menuMain->popup(p);
    });

    vl->addWidget(zin);
    vl->addWidget(zout);
    vl->addSpacing(6);
    vl->addWidget(loc);
    vl->addWidget(day);
    vl->addSpacing(6);
    vl->addWidget(m_gridBtn);
    vl->addSpacing(6);
    vl->addWidget(m_trackBtn);
    vl->addWidget(m_viaBtn);
    vl->addWidget(menu);

    /* 外观：底板/边框/悬停高亮全部由 IconButton::paintEvent 自绘
       （所以这里**不设样式表** —— 样式表会盖掉自绘的底板）。 */
    /* 右对齐：网格按钮带文字较宽，其余图标按钮靠右、与画布右沿对齐更整齐 */
    for (QWidget *w : m_rightOverlay->findChildren<QWidget *>())
        vl->setAlignment(w, Qt::AlignRight | Qt::AlignTop);

    m_rightOverlay->adjustSize();
    m_rightOverlay->raise();

    /* 画布尺寸变了要重新贴位置（画布是浮层的 parent） */
    m_canvas->installEventFilter(this);
    layoutOverlays();
}

/* 两个浮层贴到画布上：搜索在**左上**，地图操作在**右上**。
 *
 * ★ 为什么展开搜索框不会移动右上角的按钮：两个浮层都是画布的**子控件**，
 *   不参与布局 ⇒ 画布尺寸不变 ⇒ 浮层坐标不变。
 *   （上一版把搜索放在顶栏工具条里，展开输入框会让顶栏变矮 ⇒ 画布上移 ⇒
 *    右上角按钮跟着挪 12px。现在改成浮层后这个问题从根上消失了。）
 */
void MainWindow::layoutOverlays()
{
    if (!m_canvas) return;
    const int margin = 10;

    /* --- 搜索浮层：左上 --- */
    if (m_searchOverlay) {
        m_searchOverlay->adjustSize();
        int w = m_searchOverlay->width();
        int cw = m_canvas->width();
        if (cw > 0) {
            /* 宽度不许超出画布（窄窗口时收缩输入框而不是溢出） */
            if (w > cw - 2 * margin) {
                if (m_search && m_search->isVisible())
                    m_search->setFixedWidth(qMax(120, cw - 2 * margin - 8));
                m_searchOverlay->adjustSize();
                w = m_searchOverlay->width();
            }
            m_searchOverlay->move(margin, margin);
            m_searchOverlay->raise();
        }
    }

    /* --- 地图操作浮层：右上 --- */
    if (m_rightOverlay) {
        m_rightOverlay->adjustSize();
        int w = m_rightOverlay->width();
        int h = m_rightOverlay->height();
        int cw = m_canvas->width();
        int ch = m_canvas->height();
        if (cw <= 0 || ch <= 0) return;
        int x = cw - w - margin;
        int y = margin;                       /* 靠上 */
        if (y + h > ch - margin)              /* 画布太矮时兜底贴底，别溢出 */
            y = qMax(margin, ch - h - margin);
        m_rightOverlay->move(x, y);
        m_rightOverlay->raise();
    }

    /* --- 轨迹面板：右栏左边一点（贴着按钮列，不挡地图主体） ---
     * ★ 用 !isHidden() 而不是 isVisible()：isVisible() 对"父窗口还没 show()
     *   的子控件"一律返回 false（截图路径 --shot-track 正是这个时序：
     *   openTrackPanelForShot 在 win.show() 之前跑），面板就永远贴不到位、
     *   停在 (0,0)。isHidden() 只反映"自己被显式隐藏了没有"，正是我们要的。 */
    if (m_trackPanel && !m_trackPanel->isHidden() && m_rightOverlay) {
        m_trackPanel->adjustSize();
        int pw = m_trackPanel->width();
        int ph = m_trackPanel->height();
        int cw = m_canvas->width();
        int ch = m_canvas->height();
        if (cw <= 0 || ch <= 0) return;
        int x = m_rightOverlay->x() - pw - 6;
        if (x < margin) x = margin;           /* 窄窗口时压到左边沿，别溢出 */
        int y = m_rightOverlay->y();
        if (y + ph > ch - margin) y = qMax(margin, ch - ph - margin);
        m_trackPanel->move(x, y);
        m_trackPanel->raise();
    }

    /* --- 途经点面板：同样贴右栏左边（不压右栏、不越左沿、不飘在中间）--- */
    if (m_viaPanel && !m_viaPanel->isHidden() && m_rightOverlay) {
        m_viaPanel->adjustSize();
        int pw = m_viaPanel->width();
        int ph = m_viaPanel->height();
        int cw = m_canvas->width();
        int ch = m_canvas->height();
        if (cw > 0 && ch > 0) {
            int x = m_rightOverlay->x() - pw - 6;
            if (x < margin) x = margin;
            int y = m_rightOverlay->y();
            if (y + ph > ch - margin) y = qMax(margin, ch - ph - margin);
            m_viaPanel->move(x, y);
            m_viaPanel->raise();
        }
    }
}

void MainWindow::locate()
{
    if (m_a->sim_on) { m_a->view.lat = m_a->sim.car_lat; m_a->view.lon = m_a->sim.car_lon; }
    else if (m_a->pick_a.have) { m_a->view.lat = m_a->pick_a.lat; m_a->view.lon = m_a->pick_a.lon; }
    m_a->need_redraw = 1;
}

void MainWindow::setShowGrid(bool v)
{
    if (m_canvas) m_canvas->setShowGrid(v);
    /* ★ 悬停提示必须始终反映真实状态：除了点击开关（toggleGrid）外，
     *   命令行 --grid 等其它入口也走这里，若不在此同步，会出现"网格明明开着、
     *   提示却显示『关』"的不一致。按钮本身只显示图标，不显文字。 */
    updateGridBtn();
}

void MainWindow::setDatum(int id)
{
    if (id < 0 || id >= DATUM_COUNT) id = DATUM_WGS84;
    m_a->datum = id;
    updateDatumTip();
    for (QAction *a : m_datumActs)
        a->setChecked(a->data().toInt() == id);
    for (QAction *a : m_gridDatumActs)
        a->setChecked(a->data().toInt() == id);
    /* 落盘延到 app_shutdown（app_persist_settings 统一写回 settings.ini） */
    settings_set_int(m_a->cfg, "datum", id);
    m_a->need_redraw = 1;   /* 经纬网/读数立即按新基准重绘 */
}

/* 坐标网按钮左键：在 关 → WGS84 → CGCS2000 → 北京54 → 西安80 → 关 间循环。
 * 「关」是循环的一档（不再只是循环外的独立开关），左键一路点到底必能关掉网格。
 * ★ 从「关」进入循环时归到第一档 WGS84，保证 关→d0→…→d3→关 是一个闭合全循环；
 *   否则关态会停在"最后一个坐标系"上，导致 关↔末档 死循环、回不到 WGS84。
 * 右键菜单仍可精确选坐标系 + 开关网格，与左键循环互不干扰。 */
void MainWindow::cycleGridDatum()
{
    if (!m_canvas) return;
    if (!m_canvas->isGridOn()) {
        /* 关 → 开：进入循环第一档（WGS84） */
        m_canvas->setShowGrid(true);
        if ((int)m_a->datum != (int)DATUM_WGS84) setDatum((int)DATUM_WGS84);
        updateGridBtn();
        m_a->need_redraw = 1;
        return;
    }
    /* 开 → 循环推进坐标系；已是最后一个（西安80）则回到「关」 */
    int next = (int)m_a->datum + 1;
    if (next >= (int)DATUM_COUNT) {
        m_canvas->setShowGrid(false);
        updateGridBtn();
        m_a->need_redraw = 1;
        return;
    }
    setDatum(next);
}

/* 坐标网按钮的悬停提示：简洁三行，避免超出界面、也别堆砌长篇来源说明
 * （datum_note 的来龙去脉已随菜单项/坐标系说明承载，不再塞进窄按钮的提示）。
 * 网格类型（经纬网/方里格）与坐标系（大地基准）已解耦，各自由菜单选；
 * 提示随 setGridType/setDatum/开关态刷新。 */
void MainWindow::updateDatumTip()
{
    if (!m_gridBtn) return;
    const DatumId id = (DatumId)(m_a ? m_a->datum : DATUM_WGS84);
    const int gt = m_a ? m_a->grid_type : GRID_GK;
    const bool on = m_canvas && m_canvas->isGridOn();
    QString tip;
    tip += QStringLiteral("坐标网：%1\n")
               .arg(on ? QStringLiteral("开") : QStringLiteral("关"));
    tip += QStringLiteral("坐标系：%1    网格：%2\n")
               .arg(QString::fromUtf8(datum_label(id)))
               .arg(gt == GRID_GK ? QStringLiteral("方里格") : QStringLiteral("经纬网"));
    tip += QStringLiteral("左键循环切换 · 右键菜单");
    m_gridBtn->setToolTip(tip);
}

/* 同步坐标网按钮：按钮只显示图标（不再写"关"或坐标系名，避免遮挡右栏）。
 * 当前坐标系 / 开关态 / 网格类型（经纬网或方里格）全部由悬停提示承载
 * （J 先生要求：按钮不显示"关"，鼠标悬停看提示即可）。 */
void MainWindow::updateGridBtn()
{
    if (!m_gridBtn) return;
    updateDatumTip();
}

/* 坐标网「显示坐标网」开关（右键菜单用）：仅**开关**网格。
 * 坐标系切换走左键 cycleGridDatum / 右键菜单，网格类型与坐标系已解耦。 */
void MainWindow::toggleGrid()
{
    if (m_canvas) m_canvas->setShowGrid(!m_canvas->isGridOn());
}

/* 昼夜三态换算：把 day_night_mode（0 白天 / 1 夜间 / 2 自动）写成有效
 * day_night(0/1)，并同步「夜间模式」「自动」两个菜单勾选态。
 * 自动模式按视口中心经纬度 + 本地时间算日出日落；手动模式直接取设置值。
 * 核心渲染每帧也会用 app_update_daynight 刷新自动模式，这里负责"切换时立即生效"。 */
void MainWindow::applyDayNight()
{
    int eff;
    if (m_a->day_night_mode == 1)        eff = 1;
    else if (m_a->day_night_mode == 2) {
        time_t now = time(NULL);
        eff = nm_sun_is_night(m_a->view.lat, m_a->view.lon, now) ? 1 : 0;
    } else                                 eff = 0;
    m_a->day_night = eff;
    if (m_dayNightAct)     m_dayNightAct->setChecked(eff);
    if (m_dayNightAutoAct) m_dayNightAutoAct->setChecked(m_a->day_night_mode == 2);
    m_a->need_redraw = 1;
}

/* D 键 / 右栏昼/夜按钮：在 白天(0) → 夜间(1) → 自动(2) → 白天(0) 间循环。 */
void MainWindow::cycleDayNight()
{
    int m = m_a->day_night_mode;
    m = (m == 0) ? 1 : (m == 1) ? 2 : 0;
    m_a->day_night_mode = m;
    applyDayNight();
}

/* 设置网格类型（0 经纬网 / 1 方里格），持久化并立即重绘。 */
void MainWindow::setGridType(int t)
{
    if (t != GRID_LATLON && t != GRID_GK) t = GRID_GK;
    m_a->grid_type = t;
    settings_set_int(m_a->cfg, "grid_type", t);
    m_a->need_redraw = 1;
    updateGridBtn();
}

/* ---------------- 浮层透明度 ---------------- */

/* 把 App::ovl_alpha（百分比 0~100，默认 75）作用到 Qt 这边的浮层上。
 * ★★ 规则：**只淡底板，前景（文字 / 图标）保持清晰**。
 *   ★ 不用 QGraphicsOpacityEffect：它会把图标和文字**一起**淡掉
 *     （浅色底板上的文字糊成灰）。QWidget::setWindowOpacity 也不行 ——
 *     它只对顶层窗口有效，这几个浮层是画布的**子控件**，调了没效果也不报错。
 *   ★ 面板是按需创建的（toggleTrackPanel），所以那个浮层要等它建出来才有
 *     宿主 ⇒ applyOverlayAlpha 必须在面板创建后也调一次。
 * ★ 本前端没有的浮层（标题栏/顶栏/…是 C 核心自绘的）在这里不生效，值照样存。 */
/* 轨迹/收藏面板的样式表：**底板按 pct 淡，文字/图标保持不透明**。
 * pct<=0 ⇒ 前景色 alpha 也归 0（整个面板不可见）。
 * 与 C 侧 chrome.c / navdraw.c 的规则一致（"只淡底板、文字保持清晰"）。 */
QString MainWindow::trackPanelStyle(int pct) const
{
    const double k = pct / 100.0;
    const int bgA  = qBound(0, qRound(242 * k), 255);  /* 面板底板 */
    const int bdA  = qBound(0, qRound(60 * k), 255);   /* 面板边框 */
    const int btnA = qBound(0, qRound(238 * k), 255);  /* 按钮底板 */
    const int bbdA = qBound(0, qRound(160 * k), 255);  /* 按钮边框 */
    const int fgA  = (pct > 0) ? 255 : 0;              /* 前景：0 ⇒ 隐藏，否则不透明 */
    return QStringLiteral(
        "#trackPanel{background:rgba(250,250,252,%1);"
        "border:1px solid rgba(60,70,90,%2);border-radius:8px;}"
        "#trackPanel QScrollArea{background:transparent;border:none;}"
        "#trackPanel QLabel{color:rgba(26,34,51,%6);}"
        "#trackPanel QPushButton{padding:3px 10px;"
        "background:rgba(238,241,247,%3);"
        "border:1px solid rgba(120,130,145,%4);border-radius:4px;"
        "color:rgba(26,34,51,%6);}")
        .arg(bgA).arg(bdA).arg(btnA).arg(bbdA).arg(fgA);
}

/* ★★ 浮层透明度：**只淡底板，前景（文字 / 图标）保持清晰**。
 *   之前是把 QGraphicsOpacityEffect 挂在浮层容器上 —— 那会把图标和文字
 *   **一起**淡掉：浅色底板上的文字直接糊成灰，越设越看不清
 *   （J 先生 2026-10-25 要求"只淡底板、文字保持清晰"）。
 *   现在改成：
 *     · IconButton（搜索按钮 + 右栏 7 个）：底板/边框的 alpha 乘系数，
 *       图标与文字完全不透明（见 qt_icon_button.hpp）；
 *     · 搜索输入框 / 轨迹面板：用 QSS 的 rgba 背景按系数算 alpha，
 *       前景色保持不透明；
 *     · pct<=0 ⇒ 该浮层整体不画（底板和前景都不画）。
 *   与 C 侧 chrome.c / navdraw.c 的 ak()/afg() 是同一套规则。 */
void MainWindow::applyOverlayAlpha()
{
    const int ps = m_a->ovl_alpha[OVL_SEARCH];
    const int pr = m_a->ovl_alpha[OVL_RCOL];
    const int pt = m_a->ovl_alpha[OVL_TRACK];

    /* --- 搜索浮层：图标按钮 + 输入框 --- */
    if (m_searchBtn) {
        if (auto *ib = dynamic_cast<IconButton *>(m_searchBtn)) ib->setOverlayPct(ps);
    }
    /* 0% ⇒ 整个搜索浮层不显示（否则输入框的文字会孤零零浮在底图上） */
    if (m_searchOverlay) m_searchOverlay->setVisible(ps > 0);
    if (m_search) {
        const double k = ps / 100.0;
        const int bgA = qBound(0, qRound(255 * k), 255);
        const int bdA = qBound(0, qRound(160 * k), 255);
        const int fgA = (ps > 0) ? 255 : 0;
        m_search->setStyleSheet(
            QStringLiteral("QLineEdit{background:rgba(255,255,255,%1);"
                           "border:1px solid rgba(120,130,145,%2);border-radius:4px;"
                           "color:rgba(26,34,51,%3);"
                           "selection-background-color:#3c6edc;selection-color:#ffffff;}")
                .arg(bgA).arg(bdA).arg(fgA));
    }

    /* --- 右栏：全部 IconButton --- */
    if (m_rightOverlay) {
        for (QAbstractButton *b : m_rightOverlay->findChildren<QAbstractButton *>()) {
            if (auto *ib = dynamic_cast<IconButton *>(b)) ib->setOverlayPct(pr);
        }
    }

    /* --- 轨迹 / 收藏面板 --- */
    if (m_trackPanel) {
        m_trackPanel->setStyleSheet(trackPanelStyle(pt));
        m_trackPanel->setVisible(m_trackPanelOpen && pt > 0);
    }
}

/* 「浮层透明度…」对话框：每个浮层一根滑杆（0~100%，步 5），拖动即时生效。
 * 本前端不画的浮层（ovl_frontends() 里没有 OVL_FE_QT）滑杆置灰并注明
 * 「C 核心」——值照样存（换前端就生效），但不标出来用户会以为坏了。 */
void MainWindow::showOverlayAlphaDialog()
{
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("浮层透明度"));
    QVBoxLayout *vlay = new QVBoxLayout(&dlg);

    QList<QSlider *> sliders;
    QList<QLabel *>  vals;
    for (int i = 0; i < ovl_count(); i++) {
        const NmOverlay id = (NmOverlay)i;
        QHBoxLayout *row = new QHBoxLayout();
        QString name = QString::fromUtf8(ovl_label(id));
        const bool inQt = (ovl_frontends(id) & OVL_FE_QT) != 0;
        if (!inQt) name += QStringLiteral("（C 核心）");
        QLabel *lab = new QLabel(name);
        lab->setFixedWidth(150);
        QSlider *sl = new QSlider(Qt::Horizontal);
        sl->setRange(OVL_ALPHA_MIN, OVL_ALPHA_MAX);
        sl->setSingleStep(OVL_ALPHA_STEP);
        sl->setPageStep(OVL_ALPHA_STEP);
        sl->setValue(m_a->ovl_alpha[i]);
        sl->setEnabled(inQt);
        QLabel *val = new QLabel(QString::number(m_a->ovl_alpha[i]) + QStringLiteral("%"));
        val->setFixedWidth(48);
        QPushButton *def = new QPushButton(QStringLiteral("默认"));
        def->setFixedWidth(52);
        connect(sl, &QSlider::valueChanged, this, [this, id, val](int v) {
            app_ovl_alpha_set(m_a, id, v);
            applyOverlayAlpha();          /* 拖动即时预览 */
            val->setText(QString::number(m_a->ovl_alpha[id]) + QStringLiteral("%"));
        });
        connect(def, &QPushButton::clicked, this, [this, id, sl](bool) {
            app_ovl_alpha_set(m_a, id, OVL_ALPHA_DEFAULT);
            { QSignalBlocker blk(sl); sl->setValue(OVL_ALPHA_DEFAULT); }
            applyOverlayAlpha();
        });
        row->addWidget(lab); row->addWidget(sl, 1); row->addWidget(val); row->addWidget(def);
        vlay->addLayout(row);
        sliders.append(sl); vals.append(val);
    }

    QHBoxLayout *foot = new QHBoxLayout();
    QPushButton *resetAll = new QPushButton(QStringLiteral("全部恢复默认（75%）"));
    QPushButton *close = new QPushButton(QStringLiteral("关闭"));
    connect(resetAll, &QPushButton::clicked, this, [this, &sliders](bool) {
        app_ovl_alpha_reset(m_a);
        for (int i = 0; i < sliders.size(); i++) {
            QSignalBlocker blk(sliders[i]);
            sliders[i]->setValue(m_a->ovl_alpha[i]);
        }
        applyOverlayAlpha();
    });
    connect(close, &QPushButton::clicked, &dlg, &QDialog::accept);
    foot->addWidget(resetAll); foot->addStretch(1); foot->addWidget(close);
    vlay->addLayout(foot);

    dlg.exec();
    /* 值已经随拖动写进 App + settings（app_ovl_alpha_set 里），
     * 真正落盘由 app_persist_settings 在退出时统一做。 */
}

/* ---------------- GPX 轨迹 / 收藏 ---------------- */

/* 面板：右栏「轨迹」按钮弹出的浮层（挂在画布上，和右栏按钮同一层）。
 * 上半是轨迹列表（色块 + 名字 + 点数 + 删除），下半是收藏列表，底部三个动作按钮。
 * 用「一行一个 QWidget」（QLabel + QPushButton）而不是 QListWidget：条目不过几十条，
 * 行布局自己接双击/删除信号，不需要 item delegate；行容器 objectName="trkrow"，
 * UI 自检靠数它验证"面板条目与 store 一致"。
 * ★★ 末尾同步按钮勾选态必须堵信号，否则 toggled 递归（见下面 QSignalBlocker 处）。 */
void MainWindow::toggleTrackPanel()
{
    if (!m_trackPanel) {
        m_trackPanel = new QWidget(m_canvas);
        m_trackPanel->setObjectName(QStringLiteral("trackPanel"));
        /* 面板是按需创建的 ⇒ 建出来之后才能给它挂透明度效果
         * （applyOverlayAlpha 会按 m_a->ovl_alpha[OVL_TRACK] 设好）。 */
        /* ★ 定宽而不是最小宽：layoutOverlays 用「右栏 x - 面宽 - 6」定位，
           宽度一飘面板就跑到屏幕左边去（实测：按钮行 + wordWrap 提示的
           sizeHint 能把面板撑到 800+ px）。340 刚好放得下按钮行。 */
        m_trackPanel->setFixedWidth(340);
        /* 样式表由 trackPanelStyle(pct) 生成（底板按浮层透明度淡、文字不透明），
           这里先用当前设置里的值建一次，applyOverlayAlpha 会再刷新。 */
        m_trackPanel->setStyleSheet(trackPanelStyle(m_a->ovl_alpha[OVL_TRACK]));
        QVBoxLayout *vl = new QVBoxLayout(m_trackPanel);
        vl->setContentsMargins(8, 8, 8, 8);
        vl->setSpacing(6);
        QLabel *cap = new QLabel(QStringLiteral("轨迹 / 收藏"), m_trackPanel);
        QFont f = cap->font(); f.setBold(true); cap->setFont(f);
        vl->addWidget(cap);

        QScrollArea *sa = new QScrollArea(m_trackPanel);
        sa->setWidgetResizable(true);
        sa->setFrameShape(QFrame::NoFrame);
        sa->setMaximumHeight(360);
        sa->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        /* ★ 滚动区的 viewport 默认会用调色板画**不透明**底色 ⇒ 面板底板
         *   淡下去之后这一块还是实心的 ⇒ 强制透明。 */
        sa->viewport()->setAttribute(Qt::WA_TranslucentBackground, true);
        m_trackRows = new QWidget(sa);
        m_trackRows->setAttribute(Qt::WA_TranslucentBackground, true);
        QVBoxLayout *rl = new QVBoxLayout(m_trackRows);
        rl->setContentsMargins(2, 2, 2, 2);
        rl->setSpacing(2);
        sa->setWidget(m_trackRows);
        vl->addWidget(sa, 1);

        QHBoxLayout *hb = new QHBoxLayout();
        QPushButton *bimp = new QPushButton(QStringLiteral("导入 GPX…"), m_trackPanel);
        QPushButton *bexp = new QPushButton(QStringLiteral("导出全部…"), m_trackPanel);
        QPushButton *bfav = new QPushButton(QStringLiteral("收藏中心点"), m_trackPanel);
        hb->addWidget(bimp); hb->addWidget(bexp);
        hb->addWidget(bfav);
        hb->addStretch(1);
        vl->addLayout(hb);
        connect(bimp, &QPushButton::clicked, this, [this]() { importGpx(); });
        connect(bexp, &QPushButton::clicked, this, [this]() { exportGpx(); });
        connect(bfav, &QPushButton::clicked, this, [this]() { favCenter(); });
        m_trackPanel->hide();
    }
    /* ★ 用 m_trackPanelOpen 记"逻辑上开没开"，而不是问 isVisible()：
     *   浮层透明度设 0% 时面板**不真的显示**（底板和前景都不画），
     *   若拿 isVisible() 当状态，0% 时再点一次会以为"没开"又去开一次，
     *   开关就乱了。 */
    bool show = !m_trackPanelOpen;
    if (show) refreshTrackPanel();
    m_trackPanelOpen = show;
    m_trackPanel->setVisible(show && m_a->ovl_alpha[OVL_TRACK] > 0);
    /* ★★ 同步按钮勾选态必须**堵信号**：m_trackBtn 是 checkable，且
     *   toggled → toggleTrackPanel（见 buildRightCol）。setChecked 会发 toggled
     *   ⇒ 又调一次 toggleTrackPanel ⇒ 里面再 setChecked（状态这次真变了）⇒
     *   再发 toggled …… 无限递归，每次递归都 refreshTrackPanel 建/删一批控件，
     *   几百层后栈耗尽 ⇒ 在 RtlAllocateHeap 里 SIGSEGV。
     *   （实测：面板第一条 UI 断言就跑成死循环，gdb 里栈深几千层。）
     *   同类写法在整个 Qt 里都成立：setChecked/setValue/setCurrentIndex 前先
     *   QSignalBlocker，除非你就是想递归。 */
    if (m_trackBtn) {
        QSignalBlocker blk(m_trackBtn);
        m_trackBtn->setChecked(show);
    }
    applyOverlayAlpha();   /* 面板刚建好：按浮层透明度刷新底板 */
    layoutOverlays();   /* 面板贴到右栏左边 */
}

void MainWindow::refreshTrackPanel()
{
    if (!m_trackRows) return;
    QVBoxLayout *rl = qobject_cast<QVBoxLayout *>(m_trackRows->layout());
    if (!rl) return;
    /* 清空旧行：行是 QWidget，删它就连它的子布局/子控件一起删 */
    {
        QLayoutItem *child;
        while ((child = rl->takeAt(0)) != nullptr) {
            if (child->widget()) delete child->widget();
            delete child;
        }
    }
    const GpxStore *g = &m_a->gpx;
    auto add_row = [&](int kind, int idx, const QString &text,
                       const QString &tip, const QColor *swatch) {
        QWidget *row = new QWidget(m_trackRows);
        row->setObjectName(QStringLiteral("trkrow"));
        row->setProperty("gpxkind", kind);
        row->setProperty("gpxidx", idx);
        row->setToolTip(tip);
        QHBoxLayout *hl = new QHBoxLayout(row);
        hl->setContentsMargins(4, 2, 4, 2);
        hl->setSpacing(6);
        if (swatch) {
            QLabel *sw = new QLabel(row);
            sw->setFixedSize(12, 12);
            sw->setStyleSheet(QStringLiteral("background:rgb(%1,%2,%3);"
                                             "border:1px solid rgba(0,0,0,110);"
                                             "border-radius:2px;")
                                  .arg(swatch->red()).arg(swatch->green())
                                  .arg(swatch->blue()));
            hl->addWidget(sw);
        }
        QLabel *lb = new QLabel(text, row);
        lb->setToolTip(tip);
        hl->addWidget(lb, 1);
        /* 轨迹行（kind==0）多一个"贴合"按钮：把这条轨迹吸附到道路网（GPX 贴合路线） */
        if (kind == 0) {
            QPushButton *snap = new QPushButton(QString::fromUtf8("贴合"), row);
            snap->setFixedWidth(40);
            snap->setToolTip(QString::fromUtf8("把这条轨迹吸附到道路网（贴合路线）"));
            QObject::connect(snap, &QPushButton::clicked, this, [this, idx]() {
                int rc = app_gpx_snap(m_a, idx);
                if (rc == 0) refreshTrackPanel();   /* 让面板状态（如提示）刷新 */
            });
            hl->addWidget(snap);
        }
        QPushButton *del = new QPushButton(QString::fromUtf8("×"), row);
        del->setFixedWidth(24);
        del->setToolTip(QString::fromUtf8("删除这一条"));
        QObject::connect(del, &QPushButton::clicked, this, [this, kind, idx]() {
            if (kind == 0) gpxDelTrack(idx); else gpxDelFav(idx);
        });
        hl->addWidget(del);
        rl->addWidget(row);
        /* 双击定位：行/色块/名字三个都装过滤器（子控件忽略鼠标事件时会向上
         * 传播到行，但显式装上更稳，不依赖传播规则） */
        row->installEventFilter(this);
        lb->installEventFilter(this);
    };
    for (int i = 0; i < g->n_trk; i++) {
        const GpxTrack *t = &g->trk[i];
        QColor c(t->r, t->g, t->b);
        add_row(0, i,
                QStringLiteral("%1  (%2 点)")
                    .arg(QString::fromUtf8(t->name[0] ? t->name : "(未命名轨迹)"))
                    .arg(t->n),
                QString::fromUtf8("双击定位到这条轨迹的起点"), &c);
    }
    for (int i = 0; i < g->n_wpt; i++) {
        const GpxWpt *w = &g->wpt[i];
        add_row(1, i,
                QString::fromUtf8("★ ") +
                    QString::fromUtf8(w->name[0] ? w->name : "(未命名)"),
                QString::fromUtf8("双击定位到这个收藏点"), nullptr);
    }
    if (g->n_trk == 0 && g->n_wpt == 0) {
        QLabel *hint = new QLabel(
            QString::fromUtf8("（还没有轨迹：点「导入 GPX…」选一个 .gpx 文件，"
                              "或用 Ctrl+D 收藏视口中心）"),
            m_trackRows);
        hint->setObjectName(QStringLiteral("trkempty"));
        hint->setWordWrap(true);
        rl->addWidget(hint);
    }
    rl->addStretch(1);
}

/* 截图专用：把轨迹面板摆到展开态（--shot-track）。
 * 与 expandSearchForShot 同一思路：截图要的是**确定的**展开态。
 * 注意此时窗口还没 show()，isVisible() 一律是 false ⇒ 不能靠 toggle 的
 * "切换"语义，直接建控件 + 强制可见 + 主动贴位。 */
void MainWindow::openTrackPanelForShot()
{
    if (!m_trackPanel) toggleTrackPanel();   /* 建控件（会顺带把它显示出来） */
    refreshTrackPanel();
    m_trackPanelOpen = true;
    if (m_trackPanel) m_trackPanel->setVisible(m_a->ovl_alpha[OVL_TRACK] > 0);
    if (m_trackBtn) {
        QSignalBlocker blk(m_trackBtn);
        m_trackBtn->setChecked(true);
    }
    layoutOverlays();
}

/* 途经点面板：右栏「途经点」按钮弹出的浮层（挂在画布上，和右栏按钮同一层）。
 * 上半是途经点列表（序号 + 坐标 + 删除），底部「清空」一键清空。
 * 面板开着时同时进入"拾取模式"：点地图即按经过顺序追加一个途经点（map_canvas
 * 的点击分支走 app_nav_add_via）。用「一行一个 QWidget」而不是 QListWidget，
 * 与轨迹面板同套路；行容器 objectName="viarow"，UI 自检靠数它验证一致性。
 * ★★ 末尾同步按钮勾选态必须堵信号，否则 toggled 递归（见 toggleTrackPanel 同样坑）。 */
void MainWindow::toggleViaPanel()
{
    if (!m_viaPanel) {
        m_viaPanel = new QWidget(m_canvas);
        m_viaPanel->setObjectName(QStringLiteral("viaPanel"));
        /* 定宽（理由同轨迹面板：layoutOverlays 用「右栏 x - 面宽 - 6」定位，
         * 宽度一飘面板会跑到屏幕左边）。 */
        m_viaPanel->setFixedWidth(320);
        /* 面板底板用接近不透明的样式（不随浮层透明度滑杆走，免得被轨迹滑杆误关）。
         * 只淡底板、文字不透明：QLabel/QPushButton 前景写死不透明。 */
        m_viaPanel->setStyleSheet(
            "background:rgba(250,250,252,0.93);"
            "border:1px solid rgba(60,70,90,0.35);border-radius:8px;"
            "QLabel{color:rgba(26,34,51,1);}"
            "QPushButton{padding:3px 10px;background:rgba(238,241,247,0.96);"
            "border:1px solid rgba(120,130,145,0.6);border-radius:4px;"
            "color:rgba(26,34,51,1);}");
        QVBoxLayout *vl = new QVBoxLayout(m_viaPanel);
        vl->setContentsMargins(8, 8, 8, 8);
        vl->setSpacing(6);
        QLabel *cap = new QLabel(QStringLiteral("途经点"), m_viaPanel);
        QFont f = cap->font(); f.setBold(true); cap->setFont(f);
        vl->addWidget(cap);
        QLabel *hint = new QLabel(
            QStringLiteral("面板开着时，点地图即可按顺序添加途经点；路线会依次经过它们。"),
            m_viaPanel);
        hint->setWordWrap(true);
        vl->addWidget(hint);

        QScrollArea *sa = new QScrollArea(m_viaPanel);
        sa->setWidgetResizable(true);
        sa->setFrameShape(QFrame::NoFrame);
        sa->setMaximumHeight(300);
        sa->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        sa->viewport()->setAttribute(Qt::WA_TranslucentBackground, true);
        m_viaRows = new QWidget(sa);
        m_viaRows->setAttribute(Qt::WA_TranslucentBackground, true);
        QVBoxLayout *rl = new QVBoxLayout(m_viaRows);
        rl->setContentsMargins(2, 2, 2, 2);
        rl->setSpacing(2);
        sa->setWidget(m_viaRows);
        vl->addWidget(sa, 1);

        QPushButton *bclr = new QPushButton(QStringLiteral("清空"), m_viaPanel);
        connect(bclr, &QPushButton::clicked, this, [this]() {
            app_nav_clear_via(m_a);
            if (m_a->pick_a.have && m_a->pick_b.have) nav_request(m_a);
            refreshViaPanel();
        });
        vl->addWidget(bclr);
        m_viaPanel->hide();
    }
    /* 用 m_viaPanelOpen 记"逻辑上开没开"，不靠 isVisible()（同轨迹面板）。 */
    bool show = !m_viaPanelOpen;
    if (show) refreshViaPanel();
    m_viaPanelOpen = show;
    m_viaPanel->setVisible(show);
    if (m_canvas) m_canvas->setViaPick(show);   /* 面板开着 = 拾取模式开 */
    if (m_viaBtn) {
        QSignalBlocker blk(m_viaBtn);
        m_viaBtn->setChecked(show);
    }
    layoutOverlays();
}

void MainWindow::refreshViaPanel()
{
    if (!m_viaRows) return;
    QVBoxLayout *rl = qobject_cast<QVBoxLayout *>(m_viaRows->layout());
    if (!rl) return;
    /* 清空旧行：行是 QWidget，删它就连它的子布局/子控件一起删 */
    {
        QLayoutItem *child;
        while ((child = rl->takeAt(0)) != nullptr) {
            if (child->widget()) delete child->widget();
            delete child;
        }
    }
    const int n = app_nav_n_via(m_a);
    for (int i = 0; i < n; i++) {
        const NavPick *p = &m_a->via[i];
        QWidget *row = new QWidget(m_viaRows);
        row->setObjectName(QStringLiteral("viarow"));
        row->setProperty("viaidx", i);
        QHBoxLayout *hl = new QHBoxLayout(row);
        hl->setContentsMargins(4, 2, 4, 2);
        hl->setSpacing(6);
        QLabel *num = new QLabel(QString::number(i + 1), row);
        num->setFixedWidth(18);
        hl->addWidget(num);
        QLabel *lb = new QLabel(
            QStringLiteral("%1, %2").arg(p->lat, 0, 'f', 4).arg(p->lon, 0, 'f', 4), row);
        lb->setToolTip(QStringLiteral("途经点 %1：%2, %3")
                           .arg(i + 1).arg(p->lat, 0, 'f', 5).arg(p->lon, 0, 'f', 5));
        hl->addWidget(lb, 1);
        QPushButton *del = new QPushButton(QString::fromUtf8("×"), row);
        del->setFixedWidth(24);
        del->setToolTip(QString::fromUtf8("删除这个途经点"));
        connect(del, &QPushButton::clicked, this, [this, i]() {
            app_nav_del_via(m_a, i);
            if (m_a->pick_a.have && m_a->pick_b.have) nav_request(m_a);
            refreshViaPanel();
        });
        hl->addWidget(del);
        rl->addWidget(row);
    }
    if (n == 0) {
        QLabel *hint = new QLabel(
            QStringLiteral("（还没有途经点：面板开着时点地图即可添加；也可先设好起点和终点）"),
            m_viaRows);
        hint->setObjectName(QStringLiteral("viaempty"));
        hint->setWordWrap(true);
        rl->addWidget(hint);
    }
    /* 末尾若有路线，显示全程里程（途经点模式只给单方案，全程里程即 rr.total_m）。 */
    if (m_a->have_rr && m_a->rr.n > 1) {
        QLabel *tot = new QLabel(
            QStringLiteral("全程约 %1 km").arg(m_a->rr.total_m / 1000.0, 0, 'f', 1), m_viaRows);
        tot->setObjectName(QStringLiteral("viatotal"));
        rl->addWidget(tot);
    }
    rl->addStretch(1);
}

QPoint MainWindow::viaPanelPos() const
{
    return m_viaPanel ? m_viaPanel->pos() : QPoint();
}
QSize MainWindow::viaPanelSize() const
{
    return m_viaPanel ? m_viaPanel->size() : QSize();
}
bool MainWindow::viaPanelVisible() const
{
    return m_viaPanel && m_viaPanel->isVisible();
}

/* 无图区弹窗 + 预选下载的无头验收（--shot-nomap）。
 *
 * 为什么要它：本轮把"无图区提示"从一句"没有数据"改成"反查国家/省/市/区 +
 * 一键下载该地区"，其中最容易静默失效的两处是
 *   ① 反查结果没传到弹窗（两处各读一份数据，天然会漂移）；
 *   ② DownloadDialog::selectByLeaf 找不到叶子（拼音/路径对不上就静默不选）。
 * 两者都不会报错、界面照样打开，只是"按钮点下去停在树根，用户还得自己翻"。
 * 所以这里把它们做成断言：失败返回非 0，进 regress 就能拦住。
 */
int MainWindow::runNoMapPromptForShot()
{
    int fails = 0;

    /* ★ 必须先主动刷一次：--shot 不跑主循环，paint_frame 没被调用过，
     *   cov_place 还是全零（实测就这么踩到：反查全 "—"，看着像反查坏了，
     *   其实是"还没算"）。这里强制重算，把"没算"和"算错"区分开。 */
    app_refresh_coverage(m_a);
    const CovPlace &pl = m_a->cov_place;

    /* 有图区跑这个自检没有意义（本来就不该弹窗，反查也按设计留空）。
     * 明确区分"没到无图区"与"反查坏了"——否则在有图的位置跑一次就一片假 FAIL。 */
    if (m_a->cov_state != COV_NONE && m_a->cov_state != COV_BBOX_ONLY) {
        printf("nomap_reverse  跳过（当前位置有地图数据，cov_state=%d）\n",
               m_a->cov_state);
        return 0;
    }

    printf("nomap_reverse  国家=%s 省=%s%s 市=%s 区=%s 距离=%.0fkm 建议=%s\n",
           pl.country[0] ? pl.country : "—",
           pl.prov[0] ? pl.prov : "—", pl.prov_guessed ? "(推断)" : "",
           pl.city[0] ? pl.city : "—",
           pl.district[0] ? pl.district : "—",
           pl.nearest_km,
           pl.dl_label[0] ? pl.dl_label : "—");

    /* ① 反查必须给出"该下哪一块"，否则"一键下载"无从谈起 */
    if (!pl.dl_name[0] || !pl.dl_label[0]) {
        printf("ASSERT-FAIL    反查没给出建议下载项（dl_name/dl_label 为空）\n");
        fails++;
    }

    /* ② 弹窗里必须出现反查到的地名（证明弹窗读的是同一份 CovPlace） */
    const QString prompt = nomapPromptTextForTest();
    if (pl.prov[0] && !prompt.contains(QString::fromUtf8(pl.prov))) {
        printf("ASSERT-FAIL    弹窗里没出现反查到的省「%s」：%s\n",
               pl.prov, prompt.toUtf8().constData());
        fails++;
    }

    /* ③ selectByLeaf 必须真的命中并选中（这是"一键下载"的关键一步） */
    if (pl.dl_name[0]) {
        showDownloadDialog();
        const QString leaf = QString::fromLatin1(pl.dl_name);
        if (!m_dlDlg || !m_dlDlg->selectByLeaf(leaf)) {
            printf("ASSERT-FAIL    下载对话框里找不到地区叶子「%s」\n",
                   pl.dl_name);
            fails++;
        } else {
            /* 选中之后，"将下载的省名"应当等于反查给出的叶子名 —— 这一条把
             * "找到了但选错行"也拦住（同名叶子在别的国家是可能的）。 */
            const QString got = m_dlDlg->currentLeafForTest();
            if (got != leaf) {
                printf("ASSERT-FAIL    预选到了错的行：期望「%s」实得「%s」\n",
                       pl.dl_name, got.toUtf8().constData());
                fails++;
            } else {
                printf("nomap_select   预选命中 %s（GitHub 路径已就绪）\n", pl.dl_name);
            }
        }
    }
    return fails;
}



/* 调试/自检用的几何读取器 */
QSize MainWindow::canvasSize() const
{
    return m_canvas ? m_canvas->size() : QSize();
}

QPoint MainWindow::rightOverlayPos() const
{
    return m_rightOverlay ? m_rightOverlay->pos() : QPoint();
}

QPoint MainWindow::trackPanelPos() const
{
    return m_trackPanel ? m_trackPanel->pos() : QPoint();
}

QSize MainWindow::trackPanelSize() const
{
    return m_trackPanel ? m_trackPanel->size() : QSize();
}

bool MainWindow::trackPanelVisible() const
{
    return m_trackPanel && m_trackPanel->isVisible();
}

void MainWindow::locateGpxRow(int kind, int idx)
{
    const GpxStore *g = &m_a->gpx;
    double la, lo;
    if (kind == 0) {
        if (idx < 0 || idx >= g->n_trk || g->trk[idx].n == 0) return;
        la = g->trk[idx].lat_e7[0] / 1e7;
        lo = g->trk[idx].lon_e7[0] / 1e7;
    } else {
        if (idx < 0 || idx >= g->n_wpt) return;
        la = g->wpt[idx].lat; lo = g->wpt[idx].lon;
    }
    m_a->view.lat = la; m_a->view.lon = lo;
    m_a->need_redraw = 1;
}

void MainWindow::importGpx()
{
    QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("导入 GPX 文件"), QString(),
        QStringLiteral("GPX 轨迹 (*.gpx);;所有文件 (*)"));
    if (path.isEmpty()) return;
    int n = app_gpx_import(m_a, path.toUtf8().constData());
    if (n < 0)
        snprintf(m_a->status, sizeof(m_a->status),
                 "GPX 导入失败：%s（解析不到 trk/rte/wpt？）",
                 path.toUtf8().constData());
    else
        snprintf(m_a->status, sizeof(m_a->status),
                 "已导入 %d 条轨迹 / 收藏若干（来自 %s）", n,
                 path.toUtf8().constData());
    refreshTrackPanel();
    if (m_trackPanel && m_trackPanel->isVisible()) layoutOverlays();
    m_a->need_redraw = 1;
}

void MainWindow::exportGpx()
{
    QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出全部为 GPX"), QStringLiteral("navimap-tracks.gpx"),
        QStringLiteral("GPX 轨迹 (*.gpx);;所有文件 (*)"));
    if (path.isEmpty()) return;
    if (!path.endsWith(QStringLiteral(".gpx"), Qt::CaseInsensitive))
        path += QStringLiteral(".gpx");
    app_gpx_export(m_a, path.toUtf8().constData());
}

void MainWindow::favCenter()
{
    app_fav_add(m_a, m_a->view.lat, m_a->view.lon);
    refreshTrackPanel();
    if (m_trackPanel && m_trackPanel->isVisible()) layoutOverlays();
    m_a->need_redraw = 1;
}

void MainWindow::gpxDelTrack(int idx)
{
    app_gpx_del_track(m_a, idx);
    refreshTrackPanel();
    if (m_trackPanel && m_trackPanel->isVisible()) layoutOverlays();
    m_a->need_redraw = 1;
}

void MainWindow::gpxDelFav(int idx)
{
    app_fav_del(m_a, idx);
    refreshTrackPanel();
    if (m_trackPanel && m_trackPanel->isVisible()) layoutOverlays();
    m_a->need_redraw = 1;
}

/* ---------------- 左侧步骤面板 ---------------- */

void MainWindow::buildStepsDock()
{
    m_steps = new QListWidget(this);
    QDockWidget *dock = new QDockWidget(QStringLiteral("转向指令"), this);
    dock->setWidget(m_steps);
    dock->setFeatures(QDockWidget::NoDockWidgetFeatures);
    dock->setFixedWidth(280);
    addDockWidget(Qt::LeftDockWidgetArea, dock);
    dock->hide();                        /* 没路线时不占地方 */
    m_step_count = -1;
}

void MainWindow::refreshSteps()
{
    if (!m_a->have_rr || m_a->man.n == 0) {
        if (m_step_count != 0) {
            if (QDockWidget *d = qobject_cast<QDockWidget *>(m_steps->parentWidget()))
                d->hide();
            m_steps->clear();
            m_step_count = 0;
        }
        return;
    }
    if (m_a->man.n == m_step_count) return;   /* 没变化不重建 */
    m_step_count = m_a->man.n;
    m_steps->clear();
    for (int i = 0; i < m_a->man.n; i++) {
        const Maneuver *m = &m_a->man.v[i];
        if (m->type == MAN_START) continue;
        char t[128];
        maneuver_text(m, t, sizeof(t));
        m_steps->addItem(QString::fromUtf8(t));
    }
    if (QDockWidget *d = qobject_cast<QDockWidget *>(m_steps->parentWidget()))
        d->show();
}

/* ---------------- 主循环 ---------------- */

void MainWindow::tick()
{
    m_canvas->tick();                    /* 收割/推进/按需渲染 */
    refreshSteps();

    /* 浏览到已下载区域外 → 自动弹窗提示下载（一次性，回到有图才复位）。
     * 用 cov_checked 把关：首帧还没真正查过覆盖时（cov_state 仍是初始 0=COV_NONE）
     * 不误弹。COV_NONE/COV_BBOX_ONLY 来自 coverage.h，与 C 核心同一套判据。 */
    bool outside = m_a->cov_checked &&
                   (m_a->cov_state == COV_NONE || m_a->cov_state == COV_BBOX_ONLY);
    if (outside && !m_covPrompted) {
        m_covPrompted = true;
        promptDownloadOutside();
    } else if (!outside) {
        m_covPrompted = false;
    }
}

void MainWindow::showDownloadDialog()
{
    /* graph_dir 是 products/graph（或单省目录）；项目根 = graph_dir/../.. ，
     * sources/ 与 scripts/ 都从根推导，避免依赖进程 cwd。 */
    QString gd = QString::fromLocal8Bit(m_a->graph_dir);
    if (gd.isEmpty()) gd = QStringLiteral("products/graph");
    QString repo    = repoRootFromGraphDir(gd);   /* 兼容单省 graph_dir */
    QString sources = repo + QStringLiteral("/sources");

    if (!m_dlDlg) {
        m_dlDlg = new DownloadDialog(sources, repo, this);
        m_dlDlg->setOnFinished([this](const QString &name) { onMapsDownloaded(name); });
    }
    m_dlDlg->show();
    m_dlDlg->raise();
    m_dlDlg->activateWindow();
}

/* 无图区弹窗的文案（抽出来是为了可断言：--shot-nomap 要检查它真的带上了
 * 反查结果，否则"弹窗一套说法、横幅另一套"这种漂移没人能发现）。 */
QString MainWindow::nomapPromptTextForTest() const
{
    const CovPlace &pl = m_a->cov_place;
    QStringList parts;
    if (pl.country[0])  parts << QString::fromUtf8(pl.country);
    if (pl.prov[0])     parts << QString::fromUtf8(pl.prov);
    if (pl.city[0])     parts << QString::fromUtf8(pl.city);
    if (pl.district[0]) parts << QString::fromUtf8(pl.district);
    QString where = parts.isEmpty() ? QString::fromUtf8("未知区域")
                                    : parts.join(QString::fromUtf8(" · "));
    if (pl.nearest_km > 1.0) {
        where += QString::fromUtf8("（距最近的省份约 %1 km）")
                     .arg(pl.nearest_km, 0, 'f', 0);
    }

    QString text = QString::fromUtf8("您已浏览到已下载地图之外（%1, %2）。\n"
                                     "该区域没有路网数据，地图为空。\n\n"
                                     "这是哪儿：%3\n")
                       .arg(m_a->cov_lat, 0, 'f', 4)
                       .arg(m_a->cov_lon, 0, 'f', 4)
                       .arg(where);
    if (pl.dl_label[0])
        text += QString::fromUtf8("建议下载：%1").arg(QString::fromUtf8(pl.dl_label));
    else
        text += QString::fromUtf8("按国家/省份在下载列表里选择对应地区");
    return text;
}

void MainWindow::promptDownloadOutside()
{
    /* 反查结果来自 C 核心（coverage_locate 给国家/省，label_nearest_admin 给市/区），
     * 与地图顶部横幅读的是**同一份** CovPlace ⇒ 不会出现"弹窗说河北、横幅说北京"。 */
    const CovPlace &pl = m_a->cov_place;

    QDialog *d = new QDialog(this);
    d->setAttribute(Qt::WA_DeleteOnClose);       /* 关掉即销毁，不挡后续弹窗 */
    d->setWindowTitle(QString::fromUtf8("需要下载地图"));
    auto *v = new QVBoxLayout(d);

    QLabel *lab = new QLabel(nomapPromptTextForTest());
    lab->setWordWrap(true);
    lab->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto *hb = new QHBoxLayout;
    /* 反查到了具体地区 ⇒ 按钮直接把它预选好，省掉"打开列表再自己找一遍"；
     * 没反查到（境外/省表未覆盖）⇒ 退化成只打开列表。 */
    auto *go = new QPushButton(pl.dl_label[0]
                                   ? QString::fromUtf8("下载 %1").arg(QString::fromUtf8(pl.dl_label))
                                   : QString::fromUtf8("去下载地图"));
    auto *later = new QPushButton(QString::fromUtf8("稍后"));
    const QString leaf = QString::fromLatin1(pl.dl_name);
    connect(go, &QPushButton::clicked, d, [this, d, leaf]() {
        d->accept();
        showDownloadDialog();
        /* 对话框已建好并 show 过，这里再预选——selectByLeaf 内部会展开祖先+滚动 */
        if (!leaf.isEmpty() && m_dlDlg) m_dlDlg->selectByLeaf(leaf);
    });
    connect(later, &QPushButton::clicked, d, &QDialog::reject);
    hb->addStretch(1); hb->addWidget(go); hb->addWidget(later);
    v->addWidget(lab); v->addLayout(hb);
    d->resize(420, 220);
    d->show();           /* 非模态：地图还能看，用户可先观察再决定 */
}

void MainWindow::onMapsDownloaded(const QString &name)
{
    if (app_reload_maps(m_a) == 0) {
        m_a->need_redraw = 1;
        QMessageBox::information(this, QString::fromUtf8("下载完成"),
            QString::fromUtf8("已下载并加载地图：%1").arg(name));
    } else {
        QMessageBox::warning(this, QString::fromUtf8("加载失败"),
            QString::fromUtf8("地图已下载，但重新加载图集失败，请重启程序。"));
    }
}

/* ---------------- 键盘 ---------------- */

void MainWindow::handleKey(int key)
{
    switch (key) {
    case Qt::Key_Q:
        m_a->quit = 1; close();
        break;
    case Qt::Key_Escape:
        if (m_hits->isVisible())      closeSearch();
        else if (m_search->isVisible()) hideSearch();   /* 收起输入框 */
        else if (m_a->su.active) { m_a->su.active = 0; m_a->need_redraw = 1; }
        else                     { m_a->quit = 1; close(); }
        break;
    case Qt::Key_D:
        cycleDayNight();
        break;
    case Qt::Key_Equal: case Qt::Key_Plus:
        mv_zoom_at(&m_a->view, 0.5, m_a->view.w / 2, m_a->view.h / 2);
        mv_clamp_zoom(&m_a->view, 3.0, 19.0);
        m_a->need_redraw = 1;
        break;
    case Qt::Key_Minus:
        mv_zoom_at(&m_a->view, -0.5, m_a->view.w / 2, m_a->view.h / 2);
        mv_clamp_zoom(&m_a->view, 3.0, 19.0);
        m_a->need_redraw = 1;
        break;
    case Qt::Key_C:
        app_nav_clear(m_a); m_a->need_redraw = 1;
        break;
    case Qt::Key_F:
        app_nav_fit(m_a); m_a->need_redraw = 1;
        break;
    case Qt::Key_P:
        app_nav_toggle_panel(m_a);
        break;
    case Qt::Key_G:
        app_nav_sim_toggle(m_a);
        break;
    case Qt::Key_T:
        app_nav_sim_follow(m_a);
        break;
    case Qt::Key_N:
        app_nav_sim_deviate(m_a);
        break;
    case Qt::Key_Period:
        app_nav_sim_speed(m_a, 1);
        break;
    case Qt::Key_Comma:
        app_nav_sim_speed(m_a, -1);
        break;
    case Qt::Key_BracketRight:
        app_set_font_size(m_a, m_a->font_size + 1); m_a->need_redraw = 1;
        break;
    case Qt::Key_BracketLeft:
        app_set_font_size(m_a, m_a->font_size - 1); m_a->need_redraw = 1;
        break;
    case Qt::Key_O:
        m_a->show_contours = !m_a->show_contours;
        if (m_contourAct) m_contourAct->setChecked(m_a->show_contours);
        m_a->need_redraw = 1;
        break;
    case Qt::Key_V:
        m_a->voice_on = !m_a->voice_on;
        if (m_voiceAct) m_voiceAct->setChecked(m_a->voice_on);
        m_a->need_redraw = 1;
        break;
    default:
        if (key >= Qt::Key_0 && key <= Qt::Key_4) {
            m_a->max_class = key - Qt::Key_0;
            m_a->need_redraw = 1;
        }
        break;
    }
}
