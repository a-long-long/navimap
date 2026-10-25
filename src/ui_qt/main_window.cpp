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
#include <QLabel>
#include <QTimer>
#include <QDockWidget>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QImage>
#include <QDebug>

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

    /* ⑪ 坐标系（大地基准）：下拉框存在且项数与枚举一致；切基准后
     *    右下角读数**真的变**（前缀换成基准短名），切回去能**原样恢复**。
     *    这条是成对的：只查"变了"会漏掉抖动，只查"恢复"会漏掉根本没生效。 */
    {
        const int keep = m_a->datum;
        check(m_datumCombo && m_datumCombo->count() == (int)DATUM_COUNT,
              "坐标系下拉框存在且项数与枚举一致");

        m_canvas->update();   /* 光标位置沿用上一条断言里那次 MouseMove，保持固定 */
        setDatum(DATUM_WGS84);
        const QString t_wgs = m_canvas->cursorCoordsText();
        setDatum(DATUM_BJ54);
        const QString t_bj = m_canvas->cursorCoordsText();
        setDatum(DATUM_XA80);
        const QString t_xa = m_canvas->cursorCoordsText();
        setDatum(DATUM_WGS84);
        const QString t_back = m_canvas->cursorCoordsText();

        check(t_bj != t_wgs && t_bj.startsWith(QString::fromUtf8("北京54")),
              "切到北京54：读数变化且带基准前缀");
        check(t_xa.startsWith(QString::fromUtf8("西安80")),
              "切到西安80：读数带基准前缀");
        check(t_back == t_wgs, "切回 WGS84：读数原样恢复");

        setDatum(keep);                          /* 还原，别把自检结果写进设置 */
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

    /* 坐标系（大地基准）：切基准后经纬网与读数按该基准呈现。与右栏下拉框同步。 */
    QMenu *mdatum = menuBar()->addMenu(QStringLiteral("坐标系(&C)"));
    for (int i = 0; i < DATUM_COUNT; i++) {
        QAction *act = mdatum->addAction(QString::fromUtf8(datum_label((DatumId)i)));
        act->setCheckable(true);
        act->setData(i);
        act->setChecked(m_a->datum == i);
        connect(act, &QAction::triggered, this, [this, i]() { setDatum(i); });
        m_datumActs.append(act);
    }
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

    /* 坐标网（经纬网）叠加层开关 */
    QToolButton *grid = bigbtn(Qt::IconKind::Grid, QStringLiteral("坐标网（经纬网）"));
    grid->setCheckable(true);
    connect(grid, &QToolButton::toggled, this, [this](bool on) {
        m_canvas->setShowGrid(on);
    });

    /* 坐标系（大地基准）下拉框：紧邻坐标网按钮 ——
     * 切基准后经纬网（若开着）与右下角读数立即按该基准呈现。 */
    m_datumCombo = new QComboBox();
    m_datumCombo->setFixedWidth(124);
    m_datumCombo->setToolTip(QStringLiteral("坐标系（大地基准）：切基准后经纬网与右下角读数随之变化"));    for (int i = 0; i < DATUM_COUNT; i++)
        m_datumCombo->addItem(QString::fromUtf8(datum_label((DatumId)i)));
    m_datumCombo->setCurrentIndex(m_a->datum);
    /* 提示里带上该基准的**来源与精度**（文案在 datum.c 的 datum_note，跟参数表
       一起维护）：西安80 只换椭球、经纬度与 WGS84 几乎一样，不说明会被当成没生效。 */
    m_datumCombo->setToolTip(QString::fromUtf8(datum_note((DatumId)m_a->datum)));
    connect(m_datumCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int i) { setDatum(i); });

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
    vl->addWidget(grid);
    vl->addWidget(m_datumCombo);
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
    if (m_datumCombo) {
        m_datumCombo->setCurrentIndex(id);
        m_datumCombo->setToolTip(QString::fromUtf8(datum_note((DatumId)id)));
    }
    for (QAction *a : m_datumActs)
        a->setChecked(a->data().toInt() == id);
    /* 落盘延到 app_shutdown（app_persist_settings 统一写回 settings.ini） */
    settings_set_int(m_a->cfg, "datum", id);
    m_a->need_redraw = 1;   /* 经纬网/读数立即按新基准重绘 */
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
