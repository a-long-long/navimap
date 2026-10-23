#ifndef NAVIMAP_QT_MAP_CANVAS_H
#define NAVIMAP_QT_MAP_CANVAS_H

/* Qt 前端的地图画布。
 *
 * 分层原则（Qt 迁移的核心约束）：**C 渲染核心一行不动**。每帧仍走
 * app_render_to_surface()——它内部 SDL_CreateSoftwareRenderer + raster_bind，
 * 把光栅/标注/路线全部画进一张 SDL_Surface；本类只负责把这块像素包成
 * QImage 贴到 QWidget 上，再用 QPainter 画比例尺/指北针两个地图附件。
 *
 * 与像素格式的关系：surface 用 SDL_CreateRGBSurface(0,…,32,0,0,0,0)
 * 创建（与 map_shot 相同的默认 RGBA8888 字节序），QImage 侧对应
 * Format_RGBA8888（两者都定义"内存字节序 R,G,B,A"，与机器端无关）。
 */
#include <QWidget>
#include <QImage>

extern "C" {
#include "ui/window.h"      /* App / app_* / mv_* 全在这里 */
}

class MainWindow;

class MapCanvas : public QWidget {
public:
    MapCanvas(App *a, MainWindow *mw, QWidget *parent = nullptr);
    ~MapCanvas() override;

    /* 主循环节奏：收割算路结果 / 推进模拟车头 / 按需重渲染 */
    void tick();
    /* 立即渲染一帧（渲染进 SDL_Surface 并请求重绘） */
    void renderNow();

    /* 状态栏文本（经纬度/缩放/帧耗时/省与边数/导航信息） */
    QString statusText() const;
    /* 顶栏摘要（路线里程时间 / 计算中） */
    QString summaryText() const;
    /* 无人值守截图（--shot 用）：确保渲染最新并抓取整个画布 */
    QImage grabMap();

protected:
    void paintEvent(QPaintEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;

private:
    void ensureSurface(int w, int h);
    void drawScaleBar(QPainter &p);
    void drawCompass(QPainter &p);

    App        *m_a;
    MainWindow *m_mw;

    SDL_Surface *m_surf = nullptr;
    QImage       m_img;             /* 包住 m_surf->pixels，随 surface 重建 */
    bool         m_valid = false;   /* m_img 与 m_surf 同步且内容可用 */
    PaintStats   m_st;
    double       m_frame_ms = 0.0;

    /* 鼠标状态 */
    bool m_panning  = false;
    bool m_rotating = false;
    int  m_lastx = 0, m_lasty = 0;
    int  m_downx = 0, m_downy = 0;

    /* 指北针命中区（paintEvent 里更新，mousePress 里用） */
    QRect m_compassRect;
};

#endif
