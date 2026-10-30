/* tutorial/common.h — 公共光栅小工具（头文件 only，每课 #include 后 gcc main.c 即可跑）
 *
 * 提供：清屏 / 画点 / 画线(Bresenham) / 写 24 位 BMP。
 * 像素缓冲约定 0x00RRGGBB，与项目 src/ui/offscreen_shot.c 一致。
 * 这些函数是后面每一课的地基，避免每课重复粘贴。
 */
#ifndef TUT_COMMON_H
#define TUT_COMMON_H

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* 清屏：整块缓冲填成同一颜色 */
static inline void tut_cls(uint32_t *buf, int W, int H, uint32_t c) {
    for (int i = 0; i < W * H; i++) buf[i] = c;
}

/* 画点：带边界裁剪 */
static inline void tut_pp(uint32_t *buf, int W, int H, int x, int y, uint32_t c) {
    if (x < 0 || x >= W || y < 0 || y >= H) return;
    buf[y * W + x] = c;
}

/* 画线：Bresenham（raster 最底层原子操作，与项目 raster_line 同源思路） */
static inline void tut_line(uint32_t *buf, int W, int H,
                     int x0, int y0, int x1, int y1, uint32_t c) {
    int dx = abs(x1 - x0), dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        tut_pp(buf, W, H, x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* 填充矩形 */
static inline void tut_fill_rect(uint32_t *buf, int W, int H,
                          int x, int y, int w, int h, uint32_t c) {
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            tut_pp(buf, W, H, i, j, c);
}

/* 写 24 位 BMP：行从下往上、像素 BGR、每行 4 字节对齐。返回 0 成功。 */
static inline int tut_write_bmp(const char *path, const uint32_t *buf, int W, int H) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int row_bytes = W * 3;
    int pad = (4 - (row_bytes & 3)) & 3;
    int data_size = (row_bytes + pad) * H;
    int fs = 54 + data_size;
    uint8_t hdr[54];
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2,  &fs, 4);
    uint32_t off = 54;  memcpy(hdr + 10, &off, 4);
    uint32_t hs = 40;   memcpy(hdr + 14, &hs, 4);
    int32_t  w = W;     memcpy(hdr + 18, &w, 4);
    int32_t  h = H;     memcpy(hdr + 22, &h, 4);
    uint16_t planes = 1; memcpy(hdr + 26, &planes, 2);
    uint16_t bpp = 24;   memcpy(hdr + 28, &bpp, 2);
    memcpy(hdr + 34, &data_size, 4);
    fwrite(hdr, 1, 54, f);
    uint8_t *row = (uint8_t *)malloc((size_t)row_bytes + 4);
    for (int y = H - 1; y >= 0; y--) {
        int p = 0;
        for (int x = 0; x < W; x++) {
            uint32_t v = buf[y * W + x];
            row[p++] = (uint8_t)(v & 0xFF);
            row[p++] = (uint8_t)((v >> 8) & 0xFF);
            row[p++] = (uint8_t)((v >> 16) & 0xFF);
        }
        for (int i = 0; i < pad; i++) row[p++] = 0;
        fwrite(row, 1, p, f);
    }
    free(row);
    fclose(f);
    return 0;
}

#endif /* TUT_COMMON_H */
