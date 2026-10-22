#ifndef NAVIMAP_RASTER_H
#define NAVIMAP_RASTER_H

#include <SDL.h>
#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * 自己写像素的光栅原语（面填充 / 线 / 点）
 *
 * 为什么要有它：SDL 软渲染器每次 DrawLine 有 ≈65 ns 的固定开销（本机实测，
 * 与画多少像素无关），z9 一帧 91 万次调用 ≈ 60 ms，占整帧 39%（上限是用
 * LD_PRELOAD 把绘制调用变成空操作量出来的，见 PLAN.md §29）。面/路/线三条
 * 热点换成自己的光栅化后，这笔"每次调用的税"直接消失。
 *
 * ★★ 与 SDL 逐像素等价是硬要求，不是"看着差不多"。SDL 2.30 软渲染器的真实
 *    管线（照着 src/render/software/SDL_drawline.c 复刻，并用 4000 条随机线段
 *    + 1500 条随机折线逐像素验证过 0 差异）：
 *
 *    ① 先用 SDL_IntersectRectAndLine 把线段裁到 clip_rect —— **不是逐像素丢弃**，
 *       端点是被真的移动过的，斜率都可能变。只做"越界像素不写"会在整条线越界
 *       时差出一大片（第一版就错了 832/4000）。
 *    ② 再按 BytesPerPixel 分派：水平 HLINE / 垂直 VLINE / |dx|==|dy| 的 DLINE /
 *       其余走 BLINE（带 d/dinc1/dinc2 的整数 Bresenham，见下面 draw_line）。
 *    ③ SDL_RenderDrawLine 内部走的是 DrawLines(count=2)：draw_end 只有"终点被
 *       裁剪过"才为真，末尾再补一次 SDL_DrawPoint(原终点)。因为我们的 draw_line
 *       总是画满两端，那次补点恒等于重复写同一个像素，可以省掉（已验证）。
 *    ④ ★ 退化成一个点的线段（两端坐标相等）**画 1 个像素**（走 HLINE，min==max
 *       照样落一个点）；点落在裁剪框外才什么都不画。
 *       ★★ 这条踩过坑，别凭直觉改：第一版写成"什么都不画"，z9 差 216 个像素、
 *       逐段隔离量出 27 万个退化段各差 1 像素。另：DrawPoint 与"零长 DrawLine"
 *       在 SDL 里结果相同，所以两者可以互换。
 *    ⑤ 32bpp 无 alpha 的表面（本机是 SDL_PIXELFORMAT_RGB888，Amask=0）走
 *       SDL_DrawLine4 的非混合分支，写的是整整 32 位，alpha 字节恒为 0。
 *
 * 只替换"热点"：文字（字形纹理）和 UI 面板继续用 SDL。两者可以混在同一张
 * surface 上——软件渲染器没有双缓冲，就是直接写 surface->pixels，也不需要
 * RenderPresent 同步（实测过）。
 * ------------------------------------------------------------------------- */

typedef struct {
    uint32_t *px;      /* 像素首地址（surface->pixels 或自己的缓冲） */
    int       w, h;
    size_t    stride;  /* 每行多少个 uint32_t（= surface->pitch / 4） */
    SDL_Rect  clip;    /* 与 surface->clip_rect 一致 */
    uint32_t  amask;   /* 打包颜色时补上满 alpha（没有 alpha 通道时是 0） */
    uint32_t  rmask, gmask, bmask;
    uint8_t   rsh, gsh, bsh;
} Raster;

/* 绑定到一张 surface。只支持 32bpp（4 字节/像素）、不需要锁的表面；
   其它情况返回 -1，调用方应当退回 SDL 路径。 */
int raster_bind(Raster *rs, SDL_Surface *s);

/* 绑定到自己的一块内存（窗口路径：画完再上传成纹理）。masks 由调用方给。 */
int raster_bind_mem(Raster *rs, void *px, int w, int h, size_t stride,
                    uint32_t rmask, uint32_t gmask, uint32_t bmask, uint32_t amask);

static inline uint32_t raster_rgb(const Raster *rs, uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint32_t)r << rs->rsh) |
           ((uint32_t)g << rs->gsh) |
           ((uint32_t)b << rs->bsh) |
           rs->amask;      /* 与 SDL_MapRGBA(...,255) 一致：有 alpha 通道就填满 */
}

static inline void px_write(Raster *rs, int x, int y, uint32_t c)
{
    rs->px[(size_t)y * rs->stride + x] = c;   /* 调用方已保证在画布内 */
}

static inline void raster_point(Raster *rs, int x, int y, uint32_t c)
{
    if ((unsigned)x < (unsigned)rs->w && (unsigned)y < (unsigned)rs->h)
        px_write(rs, x, y, c);
}

/* 水平填充 [x0, x1) 一行，等价于 SDL_RenderDrawLine(x0, y, x1 - 1, y)。
   越界的 y 直接丢弃，x 裁进 [0, w)。 */
static inline void raster_hspan(Raster *rs, int y, int x0, int x1, uint32_t c)
{
    if ((unsigned)y >= (unsigned)rs->h) return;
    if (x0 < 0) x0 = 0;
    if (x1 > rs->w) x1 = rs->w;
    if (x0 >= x1) return;
    uint32_t *p = rs->px + (size_t)y * rs->stride;
    for (int x = x0; x < x1; x++) p[x] = c;
}

/* 裁剪后的线段光栅化：调用方保证坐标已在 clip 内，且两端不相等。
   HLINE / VLINE / DLINE / BLINE 与 SDL_draw.h 里的宏一一对应。 */
static inline void raster_line_clipped(Raster *rs, int x1, int y1, int x2, int y2,
                                       uint32_t color)
{
    uint32_t *px = rs->px;
    size_t stride = rs->stride;

    if (y1 == y2) {                                   /* HLINE */
        int xlo = x1 <= x2 ? x1 : x2, xhi = x1 <= x2 ? x2 : x1;
        uint32_t *p = px + (size_t)y1 * stride;
        for (int x = xlo; x <= xhi; x++) p[x] = color;
        return;
    }
    if (x1 == x2) {                                   /* VLINE */
        int ylo = y1 <= y2 ? y1 : y2, yhi = y1 <= y2 ? y2 : y1;
        for (int y = ylo; y <= yhi; y++) px[(size_t)y * stride + x1] = color;
        return;
    }
    int dx = x2 - x1, dy = y2 - y1;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    if (adx == ady) {                                 /* DLINE */
        /* |dx|==|dy| 时 x1 != x2（x1==x2 已被上面的 VLINE 接走），
           所以 dx 与 dy 必定同号：从 y 小的那一端出发，单向走。 */
        int x, y, n, sx;
        if (y1 <= y2) { x = x1; y = y1; n = y2 - y1 + 1; sx = (x2 > x1) ? 1 : -1; }
        else          { x = x2; y = y2; n = y1 - y2 + 1; sx = (x1 > x2) ? 1 : -1; }
        for (int i = 0; i < n; i++) {
            px[(size_t)y * stride + x] = color;
            x += sx; y++;
        }
        return;
    }
    /* BLINE */
    int numpixels, d, dinc1, dinc2, xinc1, xinc2, yinc1, yinc2;
    if (adx >= ady) {
        numpixels = adx + 1;
        d = (2 * ady) - adx;
        dinc1 = ady * 2;
        dinc2 = (ady - adx) * 2;
        xinc1 = 1; xinc2 = 1; yinc1 = 0; yinc2 = 1;
    } else {
        numpixels = ady + 1;
        d = (2 * adx) - ady;
        dinc1 = adx * 2;
        dinc2 = (adx - ady) * 2;
        xinc1 = 0; xinc2 = 1; yinc1 = 1; yinc2 = 1;
    }
    if (x1 > x2) { xinc1 = -xinc1; xinc2 = -xinc2; }
    if (y1 > y2) { yinc1 = -yinc1; yinc2 = -yinc2; }
    int x = x1, y = y1;
    for (int i = 0; i < numpixels; i++) {
        px[(size_t)y * stride + x] = color;
        if (d < 0) { d += dinc1; x += xinc1; y += yinc1; }
        else       { d += dinc2; x += xinc2; y += yinc2; }
    }
}

/* 等价于 SDL_RenderDrawLine。 */
static inline void raster_line(Raster *rs, int x0, int y0, int x1, int y1, uint32_t c)
{
    if (x0 == x1 && y0 == y1) {
        /* ④ 退化成一点：画 1 个像素（不是"什么都不画"）。
           ★ 这条踩过坑：第一版按"零长线不产生像素"直接 return，z9 差 216 个
             像素。用「同帧双画 + 逐段隔离」量出来的真相是 SDL 走
             HLINE(x1,x2,y)，min==max 时照样落一个点；退化点落在裁剪框外
             才什么都不画（与 SDL_IntersectRectAndLine 返回假一致）。
             退化段在 z9 占 67%（69 万条里 46 万条），所以这条必须精确。 */
        int x = x0, y = y0;
        if ((unsigned)(x - rs->clip.x) < (unsigned)rs->clip.w &&
            (unsigned)(y - rs->clip.y) < (unsigned)rs->clip.h)
            px_write(rs, x, y, c);
        return;
    }
    int ax = x0, ay = y0, bx = x1, by = y1;
    int inside = ((unsigned)ax < (unsigned)rs->w && (unsigned)ay < (unsigned)rs->h &&
                  (unsigned)bx < (unsigned)rs->w && (unsigned)by < (unsigned)rs->h);
    if (!inside && !SDL_IntersectRectAndLine(&rs->clip, &ax, &ay, &bx, &by))
        return;                             /* 整段在画布外 */
    raster_line_clipped(rs, ax, ay, bx, by, c);
}

/* 粗线：与 paint.c 原来的 paint_thick_line 同一套画法（竖向偏移画 n 条，
   近垂直的再补横向偏移），只是改走直写像素。 */
static inline void raster_thick_line(Raster *rs, int x0, int y0, int x1, int y1,
                                     int width, uint32_t c)
{
    if (width <= 1) { raster_line(rs, x0, y0, x1, y1, c); return; }
    int half = width / 2;
    for (int d = -half; d <= half; d++) {
        if (width % 2 == 0 && d == half) continue;
        raster_line(rs, x0, y0 + d, x1, y1 + d, c);
    }
    int adx = x1 > x0 ? x1 - x0 : x0 - x1;
    int ady = y1 > y0 ? y1 - y0 : y0 - y1;
    if (adx < ady) {
        for (int d = -half; d <= half; d++) {
            if (width % 2 == 0 && d == half) continue;
            raster_line(rs, x0 + d, y0, x1 + d, y1, c);
        }
    }
}

#endif /* NAVIMAP_RASTER_H */
