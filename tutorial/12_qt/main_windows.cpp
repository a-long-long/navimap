// tutorial/12_qt/main_windows.cpp — 第 12 课的真实 GUI 版（窗口显示，非本课验证用）
//
// 与 main.cpp 唯一差别：把同一块 C 缓冲包成 QImage 后，放进 QLabel 显示在 QMainWindow。
// 这就是项目 navmap_qt 的边界原则：C 渲染核心一行不动，Qt 只负责"搬运 + 叠加层"。

#include <QApplication>
#include <QMainWindow>
#include <QLabel>
#include <QImage>
#include <QPixmap>
#include <cstdint>
#include <cstdlib>

int main(int argc, char **argv) {
    QApplication app(argc, argv);

    const int W = 320, H = 240;
    uint32_t *px = (uint32_t *)malloc((size_t)W * H * sizeof(uint32_t));
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            px[y * W + x] = 0xFF000000u
                | ((uint32_t)(x * 255.0 / (W - 1)) << 16)
                | ((uint32_t)(y * 255.0 / (H - 1)) << 8) | 0xEA;

    QImage img(reinterpret_cast<uchar *>(px), W, H, W * 4, QImage::Format_RGB32);

    QMainWindow win;
    QLabel *label = new QLabel(&win);
    label->setPixmap(QPixmap::fromImage(img));
    win.setCentralWidget(label);
    win.resize(W, H);
    win.show();

    return app.exec();   // 事件循环；真实程序里此刻用户开始缩放/平移/导航
}
