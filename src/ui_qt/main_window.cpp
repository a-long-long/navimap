#include "main_window.h"
#include "map_canvas.h"

#include <QMenuBar>
#include <QMenu>
#include <QToolBar>
#include <functional>
#include <QToolButton>
#include <QLineEdit>
#include <QListWidget>
#include <QLabel>
#include <QTimer>
#include <QDockWidget>
#include <QStatusBar>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QDebug>

/* ---------------- 构造 / 生命周期 ---------------- */

MainWindow::MainWindow(App *a, QWidget *parent)
    : QMainWindow(parent), m_a(a)
{
    setWindowTitle(QStringLiteral("NavMap"));
    m_canvas = new MapCanvas(a, this);
    setCentralWidget(m_canvas);

    buildMenus();
    buildTopBar();
    buildRightCol();
    buildStepsDock();
    buildStatusBar();

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
               m_search->setFocus(); m_search->selectAll();
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
}

/* ---------------- 顶栏：搜索 + 模式页签 + 路线摘要 ---------------- */

void MainWindow::buildTopBar()
{
    QToolBar *tb = new QToolBar(QStringLiteral("顶栏"), this);
    tb->setMovable(false);
    tb->setFloatable(false);
    addToolBar(Qt::TopToolBarArea, tb);

    QWidget *wrap = new QWidget(this);
    QHBoxLayout *lay = new QHBoxLayout(wrap);
    lay->setContentsMargins(6, 4, 6, 4);
    lay->setSpacing(8);

    m_search = new QLineEdit(wrap);
    m_search->setPlaceholderText(QStringLiteral("搜索地点、道路…"));
    m_search->setClearButtonEnabled(true);
    m_search->setMinimumWidth(280);
    /* 原生中文输入法直接可用——这是换 Qt 最大的收益之一 */
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString &t) {
        doSearch(t);
    });
    connect(m_search, &QLineEdit::returnPressed, this, [this]() {
        if (m_a->su.n_hits > 0) commitHit(m_a->su.sel >= 0 ? m_a->su.sel : 0);
    });
    lay->addWidget(m_search, 1);

    m_browse = new QToolButton(wrap);
    m_browse->setText(QStringLiteral("浏览"));
    m_browse->setCheckable(true);
    m_nav = new QToolButton(wrap);
    m_nav->setText(QStringLiteral("导航"));
    m_nav->setCheckable(true);
    m_browse->setChecked(true);
    connect(m_browse, &QToolButton::clicked, this, [this]() {
        m_a->mode = 0; m_browse->setChecked(true); m_nav->setChecked(false);
    });
    connect(m_nav, &QToolButton::clicked, this, [this]() {
        m_a->mode = 1; m_nav->setChecked(true); m_browse->setChecked(false);
        if (m_a->have_rr) app_nav_fit(m_a);
    });
    lay->addWidget(m_browse);
    lay->addWidget(m_nav);

    m_summary = new QLabel(wrap);
    m_summary->setStyleSheet(QStringLiteral("color:#1a4fa0; font-weight:bold;"));
    lay->addWidget(m_summary);

    tb->addWidget(wrap);

    /* 搜索结果浮层：弹出在搜索框正下方 */
    m_hits = new QListWidget(this);
    m_hits->setWindowFlags(Qt::Popup);
    m_hits->setFixedWidth(420);
    connect(m_hits, &QListWidget::itemClicked, this, [this](QListWidgetItem *it) {
        commitHit(it->data(Qt::UserRole).toInt());
    });
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
    m_search->setText(QString::fromUtf8(m_a->su.hits[idx].name));
}

void MainWindow::closeSearch()
{
    m_hits->clear();
    m_hits->hide();
}

/* ---------------- 右栏：放大/缩小/定位/昼夜 ---------------- */

void MainWindow::buildRightCol()
{
    QToolBar *col = new QToolBar(QStringLiteral("右栏"), this);
    col->setMovable(false);
    col->setFloatable(false);
    col->setOrientation(Qt::Vertical);
    addToolBar(Qt::RightToolBarArea, col);

    auto bigbtn = [](const QString &txt, const QString &tip) {
        QToolButton *b = new QToolButton;
        b->setText(txt);
        b->setToolTip(tip);
        b->setFont([&]() { QFont f; f.setPointSize(14); f.setBold(true); return f; }());
        b->setFixedSize(36, 36);
        return b;
    };

    QToolButton *zin = bigbtn(QStringLiteral("＋"), QStringLiteral("放大"));
    QToolButton *zout = bigbtn(QStringLiteral("－"), QStringLiteral("缩小"));
    QToolButton *loc = bigbtn(QStringLiteral("◎"), QStringLiteral("定位到关注点"));
    QToolButton *day = bigbtn(QStringLiteral("◐"), QStringLiteral("昼/夜切换"));

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
    QToolButton *grid = bigbtn(QStringLiteral("▦"), QStringLiteral("坐标网（经纬网）"));
    grid->setCheckable(true);
    connect(grid, &QToolButton::toggled, this, [this](bool on) {
        m_canvas->setShowGrid(on);
    });

    /* 菜单按钮：弹出与菜单栏同一份 QMenu（SDLV 版 TopBar 的 ☰ 按钮行为）。
       Qt 5.11 的 popup 只有 (pos, at) 两参，用 sizeHint 把菜单定位到按钮左侧，
       避免从右栏最底钮向下弹出超出屏幕。 */
    QToolButton *menu = bigbtn(QStringLiteral("☰"), QStringLiteral("菜单"));
    connect(menu, &QToolButton::clicked, this, [this, menu]() {
        if (!m_menuMain) return;
        QPoint p = menu->mapToGlobal(menu->rect().topLeft());
        p.rx() -= m_menuMain->sizeHint().width();
        m_menuMain->popup(p);
    });

    col->addWidget(zin);
    col->addWidget(zout);
    col->addSeparator();
    col->addWidget(loc);
    col->addWidget(day);
    col->addSeparator();
    col->addWidget(grid);
    col->addWidget(menu);
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

/* ---------------- 状态栏 / 主循环 ---------------- */

void MainWindow::buildStatusBar()
{
    m_status = new QLabel(this);
    statusBar()->addWidget(m_status, 1);
}

void MainWindow::tick()
{
    m_canvas->tick();                    /* 收割/推进/按需渲染 */
    updateStatus();
    refreshSteps();
}

void MainWindow::updateStatus()
{
    m_status->setText(m_canvas->statusText());
    m_summary->setText(m_canvas->summaryText());
}

/* ---------------- 键盘（与 SDL 版同口径） ---------------- */

void MainWindow::handleKey(int key)
{
    switch (key) {
    case Qt::Key_Q:
        m_a->quit = 1; close();
        break;
    case Qt::Key_Escape:
        if (m_hits->isVisible())  closeSearch();
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
