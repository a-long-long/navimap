#ifndef NAVIMAP_QT_MAIN_WINDOW_H
#define NAVIMAP_QT_MAIN_WINDOW_H

/* Qt 前端主窗口：nav-desktop 同款界面元素，全部用真控件——
 *   菜单栏（QMenuBar）：搜索/清除路线/缩放到路线/跟随车头/昼夜/模拟导航/退出
 *   顶栏（QToolBar）：搜索框（QLineEdit 原生中文输入法）+ 浏览/导航页签 + 路线摘要
 *   右栏（QToolBar 竖排）：放大 / 缩小 / 定位 / 昼夜 / 坐标网 / 菜单
 *   左侧步骤面板（QDockWidget + QListWidget）：转向指令列表
 *   状态栏（QStatusBar）：经纬度/缩放/帧耗时/省与边数/导航信息
 * 地图画布（MapCanvas）作为 centralWidget，负责渲染与地图交互。
 */
#include <QMainWindow>
#include <QMenu>

extern "C" {
#include "ui/window.h"
}

class QLineEdit;
class QListWidget;
class QLabel;
class QTimer;
class QToolButton;
class MapCanvas;

class MainWindow : public QMainWindow {
public:
    MainWindow(App *a, QWidget *parent = nullptr);
    ~MainWindow() override;

    void start();                        /* 启动主循环定时器（33ms 一拍） */
    /* 无人值守截图：渲染一帧 → 存 PNG（--shot=路径 用，跑在 offscreen 平台） */
    bool takeShot(const QString &path);
    /* 坐标网叠加层开关（右栏「坐标网」按钮 / --grid 用） */
    void setShowGrid(bool v);

    /* MapCanvas 键盘事件转发到这里（焦点在画布上） */
    void handleKey(int key);

private:
    void buildMenus();
    void buildTopBar();
    void buildRightCol();
    void buildStepsDock();
    void buildStatusBar();

    void doSearch(const QString &text);  /* 文本变化 → 查询 → 弹结果列表 */
    void commitHit(int idx);             /* 选中某条结果：跳过去 */
    void closeSearch();
    void tick();                         /* 主循环一拍：画布 tick + UI 刷新 */
    void refreshSteps();
    void updateStatus();
    void locate();                       /* 定位按钮：回到关注点 */

    App          *m_a;
    MapCanvas    *m_canvas;
    QLineEdit    *m_search = nullptr;
    QListWidget  *m_hits   = nullptr;    /* 搜索结果浮层 */
    QListWidget  *m_steps  = nullptr;
    QLabel       *m_summary = nullptr;   /* 顶栏右侧：路线 里程/时间 */
    QLabel       *m_status  = nullptr;   /* 状态栏 */
    QTimer       *m_timer  = nullptr;
    QToolButton  *m_browse = nullptr;
    QToolButton  *m_nav    = nullptr;
    QMenu        *m_menuMain = nullptr;  /* 右栏「菜单」按钮弹出的主菜单（同时挂在菜单栏） */
    int           m_step_count = -1;     /* 步骤面板增量刷新哨兵 */
};

#endif
