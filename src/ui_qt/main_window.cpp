#include "main_window.h"
#include "map_canvas.h"
#include "qt_icons.h"
#include "qt_icon_button.hpp"
#include "utils/datum.h"   /* 坐标系（大地基准）：枚举与标签 */
#include "settings/settings.h"  /* settings_set_int：切基准后落盘 */

#include <QMenuBar>
#include <QMenu>
#include <QToolBar>
#include <functional>
#include <QToolButton>
#include <QAbstractButton>
#include <QApplication>
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
     *    悬停提示这一处常驻显示 ⇒ 提示必须带上当前基准标签与来源说明，
     *    且切基准后**随之更新**（否则用户看到的是过期基准）。
     *    同时验读数**真的变**（前缀换成基准短名）、切回去能**原样恢复**：
     *    只查"变了"会漏掉抖动，只查"恢复"会漏掉根本没生效。 */
    {
        const int keep = m_a->datum;
        check(m_gridBtn != nullptr, "坐标网按钮存在（悬停提示的宿主）");

        setDatum(DATUM_WGS84);
        QString tip = m_gridBtn->toolTip();
        check(tip.contains(QStringLiteral("当前坐标系：") +
                           QString::fromUtf8(datum_label(DATUM_WGS84))) &&
              tip.contains(QString::fromUtf8(datum_note(DATUM_WGS84))),
              "悬停提示带当前坐标系（WGS84）标签与来源说明");

        m_canvas->update();   /* 光标位置沿用上一条断言里那次 MouseMove，保持固定 */
        const QString t_wgs = m_canvas->cursorCoordsText();
        setDatum(DATUM_BJ54);
        const QString t_bj = m_canvas->cursorCoordsText();
        const QString tip_bj = m_gridBtn->toolTip();   /* 提示要跟手变，当场取样 */
        setDatum(DATUM_XA80);
        const QString t_xa = m_canvas->cursorCoordsText();
        setDatum(DATUM_WGS84);
        const QString t_back = m_canvas->cursorCoordsText();

        check(tip_bj.contains(QStringLiteral("当前坐标系：") +
                              QString::fromUtf8(datum_label(DATUM_BJ54))),
              "切到北京54：悬停提示里的当前坐标系随之更新");

        check(t_bj != t_wgs && t_bj.startsWith(QString::fromUtf8("北京54")),
              "切到北京54：读数变化且带基准前缀");
        check(t_xa.startsWith(QString::fromUtf8("西安80")),
              "切到西安80：读数带基准前缀");
        check(t_back == t_wgs, "切回 WGS84：读数原样恢复");

        setDatum(keep);                          /* 还原，别把自检结果写进设置 */
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
    addAct(m_menuMain, QStringLiteral("昼 / 夜 切换"), QKeySequence(Qt::Key_D), this,
           [this]() { m_a->day_night = !m_a->day_night; m_a->need_redraw = 1; });
    m_menuMain->addSeparator();
    addAct(m_menuMain, QStringLiteral("退出"), QKeySequence(Qt::CTRL | Qt::Key_Q), this,
           [this]() { m_a->quit = 1; close(); });
    menuBar()->addMenu(m_menuMain);

    QMenu *msim = menuBar()->addMenu(QStringLiteral("模拟导航(&S)"));
    addAct(msim, QStringLiteral("开始 / 停止"), QKeySequence(Qt::Key_G), this,
           [this]() { app_nav_sim_toggle(m_a); });
    addAct(msim, QStringLiteral("注入偏航（触发重算）"), QKeySequence(Qt::Key_N), this,
           [this]() { app_nav_sim_deviate(m_a); });
    addAct(msim, QStringLiteral("加速"), QKeySequence(Qt::Key_Period), this,
           [this]() { app_nav_sim_speed(m_a, 1); });
    addAct(msim, QStringLiteral("减速"), QKeySequence(Qt::Key_Comma), this,
           [this]() { app_nav_sim_speed(m_a, -1); });

    QMenu *mview = menuBar()->addMenu(QStringLiteral("视图(&V)"));
    for (int k = 0; k <= 4; k++) {
        addAct(mview, QString::asprintf("道路细节 LOD %d", k), QKeySequence(), this,
               [this, k]() { m_a->max_class = k; m_a->need_redraw = 1; });
    }
    mview->addSeparator();
    addAct(mview, QStringLiteral("字号增大"), QKeySequence(Qt::Key_BracketRight), this,
           [this]() { app_set_font_size(m_a, m_a->font_size + 1); m_a->need_redraw = 1; });
    addAct(mview, QStringLiteral("字号减小"), QKeySequence(Qt::Key_BracketLeft), this,
           [this]() { app_set_font_size(m_a, m_a->font_size - 1); m_a->need_redraw = 1; });

    /* 坐标系（大地基准）：切基准后经纬网与读数按该基准呈现。勾选态与右栏
     * 坐标网按钮悬停提示里的"当前坐标系"同步（右栏没有下拉框了）。 */
    QMenu *mdatum = menuBar()->addMenu(QStringLiteral("坐标系(&C)"));
    for (int i = 0; i < DATUM_COUNT; i++) {
        QAction *act = mdatum->addAction(QString::fromUtf8(datum_label((DatumId)i)));
        act->setCheckable(true);
        act->setData(i);
        act->setChecked(m_a->datum == i);
        connect(act, &QAction::triggered, this, [this, i]() { setDatum(i); });
        m_datumActs.append(act);
    }

    /* 轨迹 / 收藏：导入 GPX、导出全部、收藏中心点。 */
    QMenu *mgpx = menuBar()->addMenu(QStringLiteral("轨迹(&K)"));
    addAct(mgpx, QStringLiteral("导入 GPX…"), QKeySequence(Qt::CTRL | Qt::Key_I), this,
           [this]() { importGpx(); });
    addAct(mgpx, QStringLiteral("导出全部为 GPX…"), QKeySequence(Qt::CTRL | Qt::Key_E), this,
           [this]() { exportGpx(); });
    addAct(mgpx, QStringLiteral("收藏视口中心"), QKeySequence(Qt::CTRL | Qt::Key_D), this,
           [this]() { favCenter(); });
    addAct(mgpx, QStringLiteral("轨迹 / 收藏面板"), QKeySequence(), this,
           [this]() { toggleTrackPanel(); });
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
        m_a->day_night = !m_a->day_night; m_a->need_redraw = 1;
    });

    /* 坐标网（经纬网）叠加层开关。悬停提示里**顺带显示当前坐标系**：
     * 基准只影响经纬网与右下角读数（不重投底图），所以它和坐标网是同一件事，
     * 提示放这儿最顺。切换入口在菜单栏「坐标系(C)」/ ☰ 菜单，右栏不再放下拉框
     * （曾经放过一个 124px 的 QComboBox，占地方且和菜单重复）。 */
    m_gridBtn = bigbtn(Qt::IconKind::Grid, QStringLiteral("坐标网（经纬网）"));
    m_gridBtn->setCheckable(true);
    connect(m_gridBtn, &QToolButton::toggled, this, [this](bool on) {
        m_canvas->setShowGrid(on);
    });
    updateDatumTip();

    /* 轨迹 / 收藏面板开关（可勾选：亮起 = 面板开着）。
     * 和缩放/定位/昼夜那组"操作地图"的分开，与坐标网之间空一档。 */
    m_trackBtn = bigbtn(Qt::IconKind::Track, QStringLiteral("轨迹 / 收藏（导入 GPX）"));
    m_trackBtn->setCheckable(true);
    connect(m_trackBtn, &QToolButton::toggled, this, [this](bool) {
        toggleTrackPanel();
    });

    /* 菜单按钮：弹出与菜单栏同一份 QMenu（SDLV 版 TopBar 的 ☰ 按钮行为）。
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
    vl->addWidget(menu);

    /* 外观：底板/边框/悬停高亮全部由 IconButton::paintEvent 自绘
       （所以这里**不设样式表** —— 样式表会盖掉自绘的底板）。 */
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
}

void MainWindow::setDatum(int id)
{
    if (id < 0 || id >= DATUM_COUNT) id = DATUM_WGS84;
    m_a->datum = id;
    updateDatumTip();
    for (QAction *a : m_datumActs)
        a->setChecked(a->data().toInt() == id);
    /* 落盘延到 app_shutdown（app_persist_settings 统一写回 settings.ini） */
    settings_set_int(m_a->cfg, "datum", id);
    m_a->need_redraw = 1;   /* 经纬网/读数立即按新基准重绘 */
}

/* 坐标网按钮的悬停提示：按钮本身管"开/关经纬网"，提示里额外告诉用户
 * **当前**用哪个坐标系（大地基准）读这张图，以及去哪儿切换。
 * 提示内容随 setDatum 更新 —— 右栏没有下拉框了，这条提示是唯一的
 * "当前基准"常驻显示（切换入口见提示最后一行：菜单栏「坐标系(C)」）。
 * 精度/来源文案取 datum_note()：它和 datum.c 的参数表同文件维护，
 * 西安80 只换椭球、经纬度与 WGS84 几乎一样，不说明会被当成没生效。 */
void MainWindow::updateDatumTip()
{
    if (!m_gridBtn) return;
    const DatumId id = (DatumId)(m_a ? m_a->datum : DATUM_WGS84);
    QString tip = QStringLiteral("坐标网（经纬网）叠加层\n");
    tip += QStringLiteral("当前坐标系：%1\n%2")
               .arg(QString::fromUtf8(datum_label(id)),
                    QString::fromUtf8(datum_note(id)));
    tip += QStringLiteral("\n切换：菜单栏「坐标系(C)」或 ☰ 主菜单");
    m_gridBtn->setToolTip(tip);
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
        /* ★ 定宽而不是最小宽：layoutOverlays 用「右栏 x - 面宽 - 6」定位，
           宽度一飘面板就跑到屏幕左边去（实测：按钮行 + wordWrap 提示的
           sizeHint 能把面板撑到 800+ px）。340 刚好放得下按钮行。 */
        m_trackPanel->setFixedWidth(340);
        m_trackPanel->setStyleSheet(
            QStringLiteral("#trackPanel{background:rgba(250,250,252,242);"
                           "border:1px solid rgba(60,70,90,60%);border-radius:8px;}"
                           "#trackPanel QPushButton{padding:3px 10px;}"
                           "#trackPanel QLabel{color:#1a2233;}"));
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
        m_trackRows = new QWidget(sa);
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
    bool show = !m_trackPanel->isVisible();
    if (show) refreshTrackPanel();
    m_trackPanel->setVisible(show);
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
    if (m_trackPanel) m_trackPanel->setVisible(true);
    if (m_trackBtn) {
        QSignalBlocker blk(m_trackBtn);
        m_trackBtn->setChecked(true);
    }
    layoutOverlays();
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
}

/* ---------------- 键盘（与 SDL 版同口径） ---------------- */

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
        m_a->day_night = !m_a->day_night; m_a->need_redraw = 1;
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
    default:
        if (key >= Qt::Key_0 && key <= Qt::Key_4) {
            m_a->max_class = key - Qt::Key_0;
            m_a->need_redraw = 1;
        }
        break;
    }
}
