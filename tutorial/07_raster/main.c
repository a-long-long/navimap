/* tutorial/07_raster/main.c — 第 7 课 · 软件光栅与性能旋钮
 *
 * 累加内容：自写光栅的动机与"画质↔帧时间"权衡。
 *   - Raster：像素缓冲 + 裁剪矩形（clip）。
 *   - raster_line：Cohen-Sutherland 裁剪 + Bresenham，用 int64 中间量防溢出。
 *   - raster_px_rgba：带 alpha 的像素混合。
 *   - AREA_DOT_PX 旋钮：面外接框小于阈值走"只点一个像素"快速路径（面 LOD）。
 *
 * 编译： gcc -std=c11 -O2 -o tut main.c && ./tut
 */
#include "../common.h"
#include <stdint.h>

#define W 320
#define H 240
static uint32_t buf[W * H];

typedef struct {
    uint32_t *px; int w, h, stride;
    int x0, y0, x1, y1;          /* 裁剪矩形（含端点） */
} Raster;

static void raster_bind(Raster *r, uint32_t *b, int w, int h) {
    r->px = b; r->w = w; r->h = h; r->stride = w;
    r->x0 = 0; r->y0 = 0; r->x1 = w - 1; r->y1 = h - 1;
}

/* —— alpha 混合 —— */
static uint32_t pk(uint8_t r, uint8_t g, uint8_t b) { return ((uint32_t)r<<16)|((uint32_t)g<<8)|b; }
static void una(uint32_t v, uint8_t *c) { c[0]=v>>16; c[1]=v>>8; c[2]=v; }

static void raster_px_rgba(Raster *r, int x, int y, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t a) {
    if (x < r->x0 || x > r->x1 || y < r->y0 || y > r->y1) return;
    uint32_t v = r->px[y * r->stride + x]; uint8_t d[3]; una(v, d);
    uint8_t o0 = d[0] + (cr - d[0]) * a / 255;
    uint8_t o1 = d[1] + (cg - d[1]) * a / 255;
    uint8_t o2 = d[2] + (cb - d[2]) * a / 255;
    r->px[y * r->stride + x] = pk(o0, o1, o2);
}

/* —— Cohen-Sutherland 线段裁剪（与项目 raster_clip_line 逐字同源） —— */
static int code(Raster *r, int x, int y) {
    int c = 0;
    if (x < r->x0) c |= 1; else if (x > r->x1) c |= 2;
    if (y < r->y0) c |= 4; else if (y > r->y1) c |= 8;
    return c;
}
static int clip_line(Raster *r, int *x0, int *y0, int *x1, int *y1) {
    int c0 = code(r, *x0, *y0), c1 = code(r, *x1, *y1);
    for (;;) {
        if (!(c0 | c1)) return 1;
        if (c0 & c1) return 0;
        int c = c0 ? c0 : c1; int x = 0, y = 0;
        if (c & 1)      { y = *y0 + (int64_t)(*y1 - *y0) * (r->x0 - *x0) / (*x1 - *x0); x = r->x0; }
        else if (c & 2) { y = *y0 + (int64_t)(*y1 - *y0) * (r->x1 - *x0) / (*x1 - *x0); x = r->x1; }
        else if (c & 4) { x = *x0 + (int64_t)(*x1 - *x0) * (r->y0 - *y0) / (*y1 - *y0); y = r->y0; }
        else            { x = *x0 + (int64_t)(*x1 - *x0) * (r->y1 - *y0) / (*y1 - *y0); y = r->y1; }
        if (c == c0) { *x0 = x; *y0 = y; c0 = code(r, *x0, *y0); }
        else          { *x1 = x; *y1 = y; c1 = code(r, *x1, *y1); }
    }
}

/* —— raster_line：裁剪 + Bresenham（int64 中间量防溢出） —— */
static void raster_line(Raster *r, int x0, int y0, int x1, int y1, uint32_t col) {
    if (!clip_line(r, &x0, &y0, &x1, &y1)) return;
    int64_t dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int64_t dy = -(y1 > y0 ? y1 - y0 : y0 - y1);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int64_t err = dx + dy;
    for (;;) {
        r->px[y0 * r->stride + x0] = col;
        if (x0 == x1 && y0 == y1) break;
        int64_t e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* —— 面 LOD 旋钮：外接框小于阈值 → 只点一个像素（快速路径） —— */
#define AREA_DOT_PX 30
static void paint_area(Raster *r, int x0, int y0, int x1, int y1, uint32_t col) {
    int bw = x1 - x0, bh = y1 - y0;
    if (bw < AREA_DOT_PX || bh < AREA_DOT_PX) {       /* 小面：一个点即可 */
        int cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
        if (cx >= r->x0 && cx <= r->x1 && cy >= r->y0 && cy <= r->y1)
            r->px[cy * r->stride + cx] = col;
        return;
    }
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++)
        if (x >= r->x0 && x <= r->x1 && y >= r->y0 && y <= r->y1)
            r->px[y * r->stride + x] = col;
}

int main(void) {
    tut_cls(buf, W, H, 0xFFF4F1EA);
    Raster r; raster_bind(&r, buf, W, H);

    /* 一个超出画布的线（演示裁剪） */
    raster_line(&r, -20, 30, 300, 90, 0xFFB03A1F);

    /* alpha 混合：半透明蓝色矩形盖在背景上 */
    for (int y = 120; y < 180; y++)
        for (int x = 40; x < 200; x++)
            raster_px_rgba(&r, x, y, 31, 111, 176, 120);

    /* 面 LOD：大面 → 填充；小面 → 只点 */
    paint_area(&r, 210, 40, 300, 120, 0xFF2E8B57);   /* 大面：填充 */
    paint_area(&r, 60, 200, 75, 212, 0xFF2E8B57);     /* 小面：单点 */

    printf("AREA_DOT_PX=%d：大面走填充、小面走单点快速路径\n", AREA_DOT_PX);
    if (tut_write_bmp("out.bmp", buf, W, H) != 0) { fprintf(stderr, "write failed\n"); return 1; }
    printf("OK: wrote out.bmp — 红线(裁剪)/半透明蓝矩形/绿面(大填小点)\n");
    return 0;
}
