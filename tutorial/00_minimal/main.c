/* tutorial/00_minimal/main.c
 *
 * 第 0 课 · 最小框架（地基）
 * ---------------------------------------------------------------------------
 * 这一课不碰项目 src/ 一行代码，只搭出"一块像素缓冲 + 一个画点 + 一个画线 +
 * 一个 BMP 写出"的最小可运行程序。
 *
 * 后面每一课都在这块缓冲上【累加】一个新概念（坐标 → 投影 → 读 PBF → 建图 →
 * 渲染 → 路由 → Qt），直到它长成一个迷你离线地图查看器。真实 src/ 模块会在每课
 * 末尾作为"生产最终形态"对照。
 *
 * 为什么从这里开始：本项目 Qt 前端与离屏两套前端共用同一个 C 渲染核心，而那个
 * 核心最底层的"画布"就是一块 uint32_t 像素缓冲（见 src/ui/offscreen_shot.c）。
 * 先把这块缓冲摸透，后续所有概念才有地方"落笔"。
 *
 * 编译运行：
 *     gcc -std=c11 -O2 -o tut main.c
 *     ./tut            # 生成 out.bmp
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define W 320
#define H 240

/* 像素缓冲：每个像素 0x00RRGGBB（与项目 offscreen_shot.c 同约定） */
static uint32_t g_buf[W * H];

/* 清屏：整块缓冲填成同一颜色 */
static void cls(uint32_t c) {
    for (int i = 0; i < W * H; i++) g_buf[i] = c;
}

/* 画点：带边界裁剪（超出缓冲直接丢弃，避免越界写） */
static void put_pixel(int x, int y, uint32_t c) {
    if (x < 0 || x >= W || y < 0 || y >= H) return;
    g_buf[y * W + x] = c;
}

/* 画线：Bresenham。这是 raster 的最底层原子操作，
 * 后续 paint.c 的 raster_line 与它是同源思路（只是多了裁剪与 alpha）。 */
static void draw_line(int x0, int y0, int x1, int y1, uint32_t c) {
    int dx = abs(x1 - x0), dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        put_pixel(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* 写 24 位 BMP：行从下往上、像素 BGR、每行 4 字节对齐。
 * 与 offscreen_shot.c 的 write_bmp24 同套路——这是"无 SDL 也能出图"的基石。 */
static int write_bmp(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int row_bytes = W * 3;
    int pad = (4 - (row_bytes & 3)) & 3;
    int data_size = (row_bytes + pad) * H;
    int fs = 54 + data_size;
    uint8_t hdr[54];
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    *(uint32_t *)(hdr + 2)  = (uint32_t)fs;
    *(uint32_t *)(hdr + 10) = 54;
    *(uint32_t *)(hdr + 14) = 40;
    *(int32_t  *)(hdr + 18) = W;
    *(int32_t  *)(hdr + 22) = H;
    *(uint16_t *)(hdr + 26) = 1;
    *(uint16_t *)(hdr + 28) = 24;
    *(uint32_t *)(hdr + 34) = (uint32_t)data_size;
    fwrite(hdr, 1, 54, f);
    uint8_t row[W * 3 + 4];
    for (int y = H - 1; y >= 0; y--) {
        int p = 0;
        for (int x = 0; x < W; x++) {
            uint32_t v = g_buf[y * W + x];
            row[p++] = (uint8_t)(v & 0xFF);
            row[p++] = (uint8_t)((v >> 8) & 0xFF);
            row[p++] = (uint8_t)((v >> 16) & 0xFF);
        }
        for (int i = 0; i < pad; i++) row[p++] = 0;
        fwrite(row, 1, p, f);
    }
    fclose(f);
    return 0;
}

int main(void) {
    cls(0xFFF4F1EA);                            /* 背景：项目用 (244,241,234) 米白 */
    draw_line(0, H / 2, W - 1, H / 2, 0xFF2A2A2A); /* 横轴（暗示"纵坐标"） */
    draw_line(W / 2, 0, W / 2, H - 1, 0xFF2A2A2A); /* 纵轴（暗示"横坐标"） */
    draw_line(0, 0, W - 1, H - 1, 0xFF1F6FB0);     /* 一条对角线：演示 draw_line */

    if (write_bmp("out.bmp") != 0) {
        fprintf(stderr, "write failed\n");
        return 1;
    }
    printf("OK: wrote out.bmp (%dx%d)\n", W, H);
    return 0;
}
