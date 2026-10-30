// tutorial/12_qt/main.cpp — 第 12 课 · Qt 前端与边界原则
//
// 累加内容：遗留 C 系统如何渐进式 Qt 化——C 渲染核心一行不动，只把像素缓冲包成 QImage。
// 本课用 Qt 的 QImage 直接"接住"一块 0xFFRRGGBB 的 C 缓冲（与项目 offscreen_shot.c 同约定），
// 并存成 PNG 验证"Qt 只是缓冲的搬运工"。真实 GUI 里这步变成 QLabel::setPixmap / QMainWindow 显示。
//
// 构建（mingw64 / ucrt64 编译器均可，需链接 Qt6Gui）：
//   g++ -std=c++17 \
//     -I/d/Qt/6.11.2/mingw_64/include -I/d/Qt/6.11.2/mingw_64/include/QtGui -I/d/Qt/6.11.2/mingw_64/include/QtCore \
//     -I/d/Qt/6.11.2/mingw_64/mkspecs/win32-g++ \
//     main.cpp -o tut12.exe \
//     -L/d/Qt/6.11.2/mingw_64/lib -lQt6Gui -lQt6Core -luser32 -lkernel32 -lshell32 -lgdi32
// 运行： ./tut12.exe   （生成 out.png）

#include <QImage>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

int main() {
    // —— 这块缓冲就来自第 0~8 课的 C 渲染核心（此处用渐变+对角线演示）——
    const int W = 320, H = 240;
    uint32_t *px = (uint32_t *)malloc((size_t)W * H * sizeof(uint32_t));
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint8_t r = (uint8_t)(x * 255.0 / (W - 1));
            uint8_t g = (uint8_t)(y * 255.0 / (H - 1));
            uint8_t b = 0xEA;
            // 约定 0xFFRRGGBB —— 与 QImage::Format_RGB32 的内存布局一致（alpha 必须为 0xFF）
            px[y * W + x] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        }
    // 对角线：演示"C 画的东西被 Qt 原样展示"
    for (int i = 0; i < W; i++) {
        int x = i, y = i * H / W;
        if (x < W && y < H) px[y * W + x] = 0xFFFF0000u;
    }

    // ★ 关键一行：把 C 缓冲包成 QImage（无拷贝、只指过去）
    QImage img(reinterpret_cast<uchar *>(px), W, H, W * 4, QImage::Format_RGB32);

    // 真实 GUI 里是： label.setPixmap(QPixmap::fromImage(img)); 本课为可无头验证，存成 PNG
    if (img.save("out.png"))
        printf("OK: C 缓冲经 Qt QImage 存成 out.png（Qt 只搬运，不重绘）\n");
    else
        printf("save failed\n");

    free(px);
    return 0;
}
