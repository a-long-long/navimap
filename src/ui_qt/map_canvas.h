#ifndef NAVIMAP_QT_MAP_CANVAS_H
#define NAVIMAP_QT_MAP_CANVAS_H

/* Qt 前端的地图画布。
 *
 * 分层原则（Qt 迁移的核心约束）：**C 渲染核心一行不动**。每帧仍走
 * app_render_to_buf()——它把光栅/标注/路线全部画进调用方持有的 32bpp
 * 像素缓冲；本类只负责把这块缓冲包成 QImage 贴到 QWidget 上，再用
 * QPainter 画比例尺/指北针两个地图附件。
 *
 * ★ Qt 前端**不链 SDL**：画布缓冲是自己 malloc 的 uint32_t 数组，格式固定
 *   为 0x00RRGGBB（与 QImage::Format_RGB32 的内存布局一致，小端机上就是
 *   字节序 B,G,R,X——第 4 字节不用）。app_render_to_buf 收的是
 *   (rmask,gmask,bmask,amask)，这里传 0x00FF0000/0x0000FF00/0x000000FF/
 *   0x00000000（无 alpha 通道，alpha 恒为 0）。
 *   ⚠️ 踩过的坑：RGBA8888 ⇒ 整帧黑屏（alpha=0 全透明）；RGBX8888 ⇒
 *   红蓝互换（字节序想当然）。用 RGB32 + 忽略第 4 字节最稳。
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
    /* 立即渲染一帧（渲染进自有像素缓冲并请求重绘） */
    void renderNow();

    /* 状态文本：原先喂给 Qt 状态栏，现由画布浮层 drawRouteInfo 复用 */
    QString statusText() const;
    /* 路线摘要（里程 / 时间 / 模拟进度） */
    QString summaryText() const;
    /* 无人值守截图（--shot 用）：确保渲染最新并抓取整个画布 */
    QImage grabMap();
    /* 右下角读数的那一行文本（按所选基准换算 + 基准短名前缀）。
     * 单独抽成公开方法是为了让 --ui-test 能断言"切基准确实改了读数"——
     * 直接对绘制结果做 OCR/像素比对既脆又测不到文案。 */
    QString cursorCoordsText() const;

protected:
    void paintEvent(QPaintEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;

private:
    void ensureBuffer(int w, int h);
    void drawScaleBar(QPainter &p);
    void drawCompass(QPainter &p);
    void drawGraticule(QPainter &p);   /* 坐标网格（lat/lon 经纬网）叠加层 */
    void drawCursorCoords(QPainter &p); /* 右下角：光标处经纬度（无光标则显示中心点） */
    void drawRouteInfo(QPainter &p);   /* 左下角：路线里程 / 算路状态（替代被取消的状态栏） */

public:
    /* 坐标网开关：Qt 端叠加层，C 渲染核心不参与 */
    void setShowGrid(bool v);
    bool isGridOn() const { return m_showGrid; }   /* 坐标网是否开启（右栏按钮循环切换用） */

    App        *m_a;
    MainWindow *m_mw;

    /* 自持像素缓冲：app_render_to_buf 的目标，QImage 包住它（不复制） */
    uint32_t   *m_px = nullptr;
    int         m_px_w = 0, m_px_h = 0;
    QImage      m_img;             /* 包住 m_px，随缓冲重建 */
    bool        m_valid = false;   /* m_img 与 m_px 同步且内容可用 */
    PaintStats  m_st;
    double      m_frame_ms = 0.0;

    /* 鼠标状态 */
    bool m_panning  = false;
    bool m_rotating = false;
    int  m_lastx = 0, m_lasty = 0;
    int  m_downx = 0, m_downy = 0;

    /* 光标位置（右下角坐标显示用）。
     * m_cursor_in=false ⇒ 没收到过鼠标移动事件 ⇒ 显示视口中心坐标。 */
    bool m_cursor_in = false;
    int  m_cursor_x = 0, m_cursor_y = 0;

    /* 指北针命中区（paintEvent 里更新，mousePress 里用） */
    QRect m_compassRect;

    /* 坐标网叠加层开关（右栏「坐标网」按钮控制） */
    bool m_showGrid = false;
};

#endif
