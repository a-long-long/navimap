#ifndef NAVIMAP_QT_MAIN_WINDOW_H
#define NAVIMAP_QT_MAIN_WINDOW_H

/* Qt 前端主窗口：nav-desktop 同款界面元素，全部用真控件——
 *   取消顶栏菜单栏：全部功能收进右栏「≡主菜单」一个弹出按钮（Qt 弹出菜单）。
 *   **浮在地图上的独立按钮**（不进任何工具栏）：
 *       左上角：搜索按钮（点击才展开搜索框）
 *       右上角：放大 / 缩小 / 定位 / 昼夜 / 坐标网(点击循环切换坐标系) / 轨迹 / ≡主菜单
 *   左侧步骤面板（QDockWidget + QListWidget）：转向指令列表
 * 地图画布（MapCanvas）作为 centralWidget，负责渲染与地图交互。
 *
 * ★ 没有底部状态栏：原先的 QStatusBar（经纬度/缩放/帧耗时/省与边数/导航
 *   信息 + 路线摘要）已取消。坐标信息改由画布上的浮层承担——
 *   右下角是光标/中心经纬度（drawCursorCoords），左下角是路线里程/算路
 *   状态（drawRouteInfo），视觉与比例尺一致，且 UI 自检 ⑨⑩ 扫的正是画布。
 *
 * ★ 为什么**没有顶栏**（原先有一个 QToolBar 放搜索按钮和页签）：
 *   地图 app 的惯例是"控件浮在地图上"，不占一条独立的横栏。
 *   一条常驻工具栏会挤掉地图的垂直空间，也让界面显得像桌面软件而不是导航 app。
 *   现在只有：地图（浮着按钮 + 一个「≡主菜单」弹出按钮）。底部状态栏已取消。
 *
 * ★ 两个浮层都挂在 MapCanvas 上（parent=m_canvas），随画布尺寸变化重新定位
 *   （靠 MainWindow::eventFilter 监听画布的 Resize）：
 *       m_searchOverlay —— 左上角的搜索按钮/搜索框
 *       m_rightOverlay —— 右上角的地图操作按钮
 *
 * ★ 浮层**不占布局空间**，所以"展开搜索框"既不会改变画布尺寸，也不会让
 *   右上角的按钮挪位（这两条都有 UI 自检断言）。
 */
#include <QMainWindow>
#include <QMenu>
#include <QList>

extern "C" {
#include "ui/window.h"
}

class QLineEdit;
class QListWidget;
class QLabel;
class QTimer;
class QToolButton;
class QWidget;
class MapCanvas;
class DownloadDialog;   /* 下载地图对话框（全球 Geofabrik 列表）*/

class MainWindow : public QMainWindow {
public:
    MainWindow(App *a, QWidget *parent = nullptr);
    ~MainWindow() override;

    void start();                        /* 启动主循环定时器（33ms 一拍） */
    /* 无人值守截图：渲染一帧 → 存 PNG（--shot=路径 用，跑在 offscreen 平台） */
    bool takeShot(const QString &path);
    /* 坐标网叠加层开关（右栏「坐标网」按钮 / --grid 用） */
    void setShowGrid(bool v);

    /* 坐标系（大地基准）切换：☰ 菜单 / 菜单栏「坐标系(C)」；右栏只在下拉框
     * 撤掉后留下的坐标网按钮悬停提示里显示"当前坐标系"（updateDatumTip） */
    void setDatum(int id);
    void updateDatumTip();             /* 刷新坐标网按钮悬停提示里的"当前坐标系" */
    void cycleGrid();                  /* 坐标网按钮：点击循环切换坐标系（关→WGS84→CGCS2000→北京54→西安80→关） */
    void updateGridBtn();              /* 同步按钮上的当前坐标系短名 + 悬停提示 */
    void applyOverlayAlpha();          /* 把 App::ovl_alpha 作用到本前端的浮层上 */
    QString trackPanelStyle(int pct) const; /* 轨迹面板样式表：底板按 pct 淡、文字不透明 */
    void showOverlayAlphaDialog();     /*「浮层透明度…」对话框：每个浮层一根滑杆 */

    /* ---- GPX 轨迹 / 收藏 ----
     * 面板是右栏左侧的浮层（和右栏按钮同一层，不参与布局 ⇒ 不影响按钮坐标）。
     * importGpx / exportGpx 弹 QFileDialog；favCenter 收藏视口中心。 */
    void toggleTrackPanel();
    void importGpx();
    void exportGpx();
    void favCenter();
    void gpxDelTrack(int idx);     /* 删一条轨迹（库 + 内存） */
    void gpxDelFav(int idx);       /* 删一个收藏 */
    void refreshTrackPanel();      /* 轨迹/收藏列表内容与 App 同步后重画面板 */

    /* MapCanvas 键盘事件转发到这里（焦点在画布上） */
    void handleKey(int key);

    /* 无头 UI 自检（--ui-test 用）：断言"搜索按钮→点击→输入框出现→Esc 收起"
     * 这条交互链，以及每个按钮都有悬停提示。
     * 全部通过返回 0，任一条不符返回非 0（并在 stderr 打印失败项）。
     * ★ 为什么要有：截图只能证明**静态**样子对，证明不了"点了真的会出现" ——
     *   这类"注释写了 ≠ 路径落实"的洞本项目踩过好几次（窗口最小化按钮）。 */
    int runUiSelfTest();

    /* 展开搜索框（--shot-search 用）：截图前调它，能看到输入框展开态的样子。
     * 不走点击事件，直接调内部方法 —— 截图要的是**确定的**展开态，
     * 而不是"点一下之后希望它恰好展开了"。 */
    void expandSearchForShot();

    /* 打开轨迹/收藏面板（--shot-track 用）：同上，截图前拿到确定的面板展开态。
     * 导入的 GPX 会先进 store（--gpx 在 main 里先跑），所以面板里就是有内容的。 */
    void openTrackPanelForShot();

    /* 无图区弹窗 + 预选下载（--shot-nomap 用）：不问"有没有弹窗"这种靠眼睛的
     * 问题，而是把三件事变成返回值：
     *   ① 反查结果里有没有"该下哪一块"（dl_name/dl_label）；
     *   ② 弹窗文案里有没有出现反查到的省/市/区（防两处文案漂移）；
     *   ③ DownloadDialog::selectByLeaf 能不能真的命中那个叶子并选中它。
     * 返回 0 = 全对；非 0 = 有位号失败（调用方直接用退出码）。 */
    int runNoMapPromptForShot();

    /* 无图区弹窗的文案（--shot-nomap 断言用）：抽成函数才能检查"弹窗里到底写了
     * 什么"，而不是靠截图肉眼确认。 */
    QString nomapPromptTextForTest() const;

    /* 几何读取器（UI 自检 / 截图用）：浮层贴对了地方没有，只能问几何。
     * 曾因 layoutOverlays 里用 isVisible() 判断面板显隐，导致 --shot-track
     * （窗口还没 show）时面板永远停在 (0,0) —— 这条就是钉那个 bug 的。 */
    QSize  canvasSize() const;
    QPoint rightOverlayPos() const;
    QPoint trackPanelPos() const;
    QSize  trackPanelSize() const;
    bool   trackPanelVisible() const;

protected:
    /* 监听画布 Resize（右栏浮层重新定位）+ 搜索框的 Esc（收起搜索框） */
    bool eventFilter(QObject *obj, QEvent *ev) override;

private:
    void buildMenus();
    void buildSearchOverlay();            /* 左上角：搜索按钮 + 搜索框浮层 */
    void buildRightCol();
    void buildStepsDock();

    void showSearch();                   /* 点搜索按钮 → 展开输入框并聚焦 */
    void hideSearch();                   /* 收起输入框 → 回到按钮态 */
    void doSearch(const QString &text);  /* 文本变化 → 查询 → 弹结果列表 */
    void commitHit(int idx);             /* 选中某条结果：跳过去 */
    void closeSearch();
    void tick();                         /* 主循环一拍：画布 tick + UI 刷新 */
    void refreshSteps();
    void locateGpxRow(int kind, int idx); /* 双击面板某行：把视口挪到那条轨迹/收藏 */
    void locate();                       /* 定位按钮：回到关注点 */
    void layoutOverlays();               /* 两个浮层贴到画布上（画布尺寸变了要重贴） */

    /* ---- 下载地图（菜单「下载地图…」+ 浏览到区域外自动提示）---- */
    void showDownloadDialog();           /* 打开下载对话框（全球 Geofabrik 列表） */
    void promptDownloadOutside();        /* 浏览到已下载区域外时弹出的提示框 */
    void onMapsDownloaded(const QString &name);  /* 下载+转换完成后重载图集 */

    App          *m_a;
    MapCanvas    *m_canvas;
    QWidget      *m_searchOverlay = nullptr; /* 左上角浮层容器（挂在画布上） */
    QToolButton  *m_searchBtn = nullptr; /* 「搜索」按钮（点它才出输入框） */
    QLineEdit    *m_search = nullptr;    /* 搜索输入框（默认隐藏） */
    QListWidget  *m_hits   = nullptr;    /* 搜索结果浮层 */
    QListWidget  *m_steps  = nullptr;
    QTimer       *m_timer  = nullptr;
    QMenu        *m_menuMain = nullptr;  /* 右栏「菜单」按钮弹出的主菜单（同时挂在菜单栏） */
    QMenu        *m_setMenu  = nullptr;   /* 「设置」子菜单（地图信息显示开关集中地） */
    QWidget      *m_rightOverlay = nullptr; /* 右上角浮层容器（挂在画布上） */
    QToolButton  *m_gridBtn = nullptr;    /* 右栏「坐标网」按钮（悬停提示带当前坐标系） */
    QList<QAction*> m_datumActs;           /* ☰ 菜单里的坐标系动作（与悬停提示同步） */
    QAction      *m_contourAct = nullptr;  /* 设置菜单「等高线」勾选项（与 O 键同步） */
    QAction      *m_dayNightAct = nullptr; /* 设置菜单「昼夜模式」勾选项（与 D 键、右栏昼/夜按钮同步） */
    /* 设置菜单里的地图信息显示 / 播报开关（原 SDL 设置面板承载的开关，#88 拆 SDL 壳后由 Qt 菜单接管） */
    QAction      *m_voiceAct  = nullptr;   /* 「语音播报」（与 V 键同步） */
    QAction      *m_poiAct    = nullptr;   /* 「POI 图标」 */
    QAction      *m_labelsAct = nullptr;   /* 「地名标注」 */
    QAction      *m_subdivAct = nullptr;   /* 「网格细分刻度」（无快捷键，仅『设置』菜单切换） */
    /* GPX 轨迹/收藏面板（右栏「轨迹」按钮弹出；挂在画布上的浮层） */
    QWidget      *m_trackPanel = nullptr;
    QWidget      *m_trackRows  = nullptr;   /* 滚动区里的行容器（每条轨迹/收藏一行） */
    QToolButton  *m_trackBtn   = nullptr;   /* 右栏「轨迹」按钮（可勾选，亮起=面板开） */
    bool          m_trackPanelOpen = false;  /* 轨迹面板是否"逻辑上打开"（0% 时不真的显示） */
    QString       m_lastPick;            /* 最近一次选中的地点（按钮提示用） */
    int           m_step_count = -1;     /* 步骤面板增量刷新哨兵 */
    DownloadDialog *m_dlDlg = nullptr;    /* 下载地图对话框（懒创建） */
    bool          m_covPrompted = false; /* 区域外提示是否已弹过（回到有图才复位）*/
};

#endif
