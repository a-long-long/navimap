/* tutorial/08_pipeline/main.c — 第 8 课 · 渲染管线编排
 *
 * 累加内容：看懂一帧怎么从"图集 + 视口"变成"像素缓冲"。本课实现 paint_frame 的多阶段编排，
 * 并用 TICK 给每个阶段计时——这是定位帧时间瓶颈的唯一手段（没有它就只能去优化"投影贵"这种猜错的方向）。
 *
 * 编译： gcc -std=c11 -O2 -o tut main.c && ./tut
 */
#include "../common.h"
#include <time.h>

#define W 360
#define H 280
static uint32_t buf[W * H];

static long long now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* —— 六个阶段（每个都往同一块缓冲上画，后画的盖先画的） —— */
static void stage_bg(void)    { tut_cls(buf, W, H, 0xFFF4F1EA); }                 /* 背景 */
static void stage_areas(void) {                                                   /* 面（公园/水域） */
    tut_fill_rect(buf, W, H, 30, 40, 140, 90, 0xFFCDEBD2);
    tut_fill_rect(buf, W, H, 220, 150, 110, 80, 0xFFCFE3F0);
}
static void stage_roads(void) {                                                   /* 路 */
    tut_line(buf, W, H, 20, 120, 340, 110, 0xFF5A5A5A);
    tut_line(buf, W, H, 120, 10, 130, 270, 0xFF8A8A8A);
    tut_line(buf, W, H, 60, 260, 300, 60, 0xFFB0B0B0);
}
static void stage_labels(void) {                                                  /* 地名锚点（真实是文字） */
    for (int i = 0; i < 5; i++)
        tut_fill_rect(buf, W, H, 40 + i * 60, 30 + (i % 2) * 40, 4, 4, 0xFF1F1F1F);
}
static void stage_overlay(void) {                                                /* 叠加层：规划路线（亮蓝芯白边） */
    int rx[] = {40, 120, 200, 300}, ry[] = {240, 150, 120, 60};
    for (int i = 0; i < 3; i++) {
        tut_line(buf, W, H, rx[i], ry[i], rx[i+1], ry[i+1], 0xFFFFFFFF);
        tut_line(buf, W, H, rx[i], ry[i], rx[i+1], ry[i+1], 0xFF1F6FB0);
    }
}
static void stage_chrome(void) {                                                  /* 自绘标题栏 / 右栏（真实是 Qt 控件） */
    tut_fill_rect(buf, W, H, 0, 0, W, 18, 0xFF2A3B52);
    tut_fill_rect(buf, W, H, W - 16, 18, 16, H - 18, 0xFF3A4B62);
}

/* TICK：包住一个阶段，打印耗时 */
#define TICK(fn) do { long long _t = now_ms(); fn(); printf("  %-12s %lld ms\n", #fn, now_ms() - _t); } while (0)

/* paint_frame：编排一帧（与项目 window.c 的 paint_frame 同顺序） */
static void paint_frame(void) {
    TICK(stage_bg);
    TICK(stage_areas);
    TICK(stage_roads);
    TICK(stage_labels);
    TICK(stage_overlay);
    TICK(stage_chrome);
}

int main(void) {
    printf("paint_frame 分阶段耗时（定位瓶颈用确定性计时，不是猜）：\n");
    paint_frame();
    if (tut_write_bmp("out.bmp", buf, W, H) != 0) { fprintf(stderr, "write failed\n"); return 1; }
    printf("OK: wrote out.bmp — 背景/面/路/锚点/路线(亮蓝)/标题栏 分层绘制\n");
    return 0;
}
