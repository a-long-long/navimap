#ifndef NAVIMAP_RASTER_H
#define NAVIMAP_RASTER_H

#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * 自己写像素的光栅原语（面填充 / 线 / 点 / 文字合成）
 *
 * 为什么要有它：SDL 软渲染器每次 DrawLine 有 ≈65 ns 的固定开销（本机实测，
 * 与画多少像素无关），z9 一帧 91 万次调用 ≈ 60 ms，占整帧 39%（上限是用
 * LD_PRELOAD 把绘制调用变成空操作量出来的，见 PLAN.md §29）。面/路/线三条
 * 热点换成自己的光栅化后，这笔"每次调用的税"直接消失。
 *
 * ★★ 与 SDL 软渲染器逐像素等价是硬要求，不是"看着差不多"。原本的
 *    SDL_IntersectRectAndLine 已逐字移植到 raster_clip_line（见下方实现）：
 *    Cohen-Sutherland 裁剪、64 位中间量防溢出，并用 4000 条随机线段 + 1500
 *    条随机折线验证过 0 差异。改这里之前先跑验证。
 *
 * 坐标/裁剪约定（与 SDL 完全一致）：
 *    - 画布是 w×h 的 32bpp 像素缓冲，stride 以"像素"计。
 *    - clip 是[inclusive]矩形 (x, y, w, h)：左/上包含 x..x+w-1 / y..y+h-1。
 *    - 退化成一点的线段（两端坐标相等）画 1 个像素（不走"什么都不画"）。
 * ------------------------------------------------------------------------- */

/* 整数矩形（布局/命中测试用，跟像素格式无关）。取代原来的 SDL_Rect。 */
typedef struct { int x, y, w, h; } RectI;

typedef struct {
    uint32_t *px;      /* 像素首地址（调用方持有缓冲） */
    int       w, h;
    size_t    stride;  /* 每行多少个 uint32_t */
    RectI     clip;    /* 裁剪矩形（inclusive），等同 SDL 的 clip_rect */
    uint32_t  amask;   /* 打包颜色时补上满 alpha（没有 alpha 通道时是 0） */
    uint32_t  rmask, gmask, bmask;
    uint8_t   rsh, gsh, bsh;
} Raster;

/* 绑定到自己的一块内存（窗口路径：画完再上传成纹理）。masks 由调用方给。 */
int raster_bind_mem(Raster *rs, void *px, int w, int h, size_t stride,
                    uint32_t rmask, uint32_t gmask, uint32_t bmask, uint32_t amask);

/* 打包成 32 位字（SDL_MapRGBA(...,255) 语义：有 alpha 通道就填满）。 */
static inline uint32_t raster_rgb(const Raster *rs, uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint32_t)r << rs->rsh) |
           ((uint32_t)g << rs->gsh) |
           ((uint32_t)b << rs->bsh) |
           rs->amask;
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

/* 逐字移植 SDL_IntersectRectAndLine（SDL 2.32：src/video/SDL_rect_impl.h）。
 * 用 rs->clip 作为裁剪矩形；返回 1=有交集、0=整段在外。
 *
 * ★ 为什么必须"逐字"而不是"照着写个差不多的"：
 *   SDL 的裁剪结果会**移动端点**（不是逐像素丢弃），斜率都可能变；而交点
 *   位置的取整方式一旦不同，线段跨过画布边界时就会落在**相邻两像素**上。
 *   实测：手写版与 SDL 在 34.3% 的跨界线段上差 1 像素（tools/clip_equiv.c
 *   门禁，20 万条随机线段）。画面上只表现为"最外圈像素颜色不同"（整图
 *   差 0.07%），但那正是"要素色 → 背景色"的方向，容易被误判成要素丢失。
 *
 *   三个必须与 SDL 一致的细节（都踩过）：
 *   ① 交点公式的**差值方向**：统一写成 x2-x1 / y2-y1。反过来写（x1-x2）
 *      在负斜率段上差 1 像素 —— 这就是第一版 34% 不一致的直接原因。
 *   ② 水平线 / 垂直线**单独处理**（直接夹取端点），不走通用分支。
 *   ③ 两个提前返回：整段在框内（端点不动）、整段在框外（不相交）。
 *   算术用 64 位中间量（BIGSCALARTYPE=Sint64），整数除法向零截断，与 SDL 一致。 */
#define RASTER_CODE_BOTTOM 1
#define RASTER_CODE_TOP    2
#define RASTER_CODE_LEFT   4
#define RASTER_CODE_RIGHT  8

/* 与 SDL 的 ComputeOutCode 一致：注意下界用 rect.y / rect.x，
   上界用 rect.y+rect.h / rect.x+rect.w（**不含**该值）。 */
static inline int raster_out_code(int rectx1, int recty1,
                                  int rectx2, int recty2, int x, int y)
{
    int code = 0;
    if (y < recty1) {
        code |= RASTER_CODE_TOP;
    } else if (y > recty2) {
        code |= RASTER_CODE_BOTTOM;
    }
    if (x < rectx1) {
        code |= RASTER_CODE_LEFT;
    } else if (x > rectx2) {
        code |= RASTER_CODE_RIGHT;
    }
    return code;
}

static inline int raster_clip_line(Raster *rs, int *X1, int *Y1, int *X2, int *Y2)
{
    typedef int64_t RasterBig;
    int x1 = *X1, y1 = *Y1, x2 = *X2, y2 = *Y2;
    const int rectx1 = rs->clip.x, recty1 = rs->clip.y;
    const int rectx2 = rs->clip.x + rs->clip.w - 1;
    const int recty2 = rs->clip.y + rs->clip.h - 1;
    int outcode1, outcode2;
    int x = 0, y = 0;

    if (rs->clip.w <= 0 || rs->clip.h <= 0) return 0;

    /* 整段都在框内：端点不动，直接返回 */
    if (x1 >= rectx1 && x1 <= rectx2 && x2 >= rectx1 && x2 <= rectx2 &&
        y1 >= recty1 && y1 <= recty2 && y2 >= recty1 && y2 <= recty2)
        return 1;

    /* 整段都在框的某一侧之外：不相交 */
    if ((x1 < rectx1 && x2 < rectx1) || (x1 > rectx2 && x2 > rectx2) ||
        (y1 < recty1 && y2 < recty1) || (y1 > recty2 && y2 > recty2))
        return 0;

    /* 水平线：直接夹 x（SDL 同样单独处理） */
    if (y1 == y2) {
        if (x1 < rectx1)      x1 = rectx1;
        else if (x1 > rectx2) x1 = rectx2;
        if (x2 < rectx1)      x2 = rectx1;
        else if (x2 > rectx2) x2 = rectx2;
        *X1 = x1; *Y1 = y1; *X2 = x2; *Y2 = y2;
        return 1;
    }

    /* 垂直线：直接夹 y */
    if (x1 == x2) {
        if (y1 < recty1)      y1 = recty1;
        else if (y1 > recty2) y1 = recty2;
        if (y2 < recty1)      y2 = recty1;
        else if (y2 > recty2) y2 = recty2;
        *X1 = x1; *Y1 = y1; *X2 = x2; *Y2 = y2;
        return 1;
    }

    /* Cohen-Sutherland：两个分支的交点公式**完全相同**，只是更新哪个端点。
       ★ 差值方向必须是 (x2-x1) / (y2-y1)，不能反。 */
    outcode1 = raster_out_code(rectx1, recty1, rectx2, recty2, x1, y1);
    outcode2 = raster_out_code(rectx1, recty1, rectx2, recty2, x2, y2);
    while (outcode1 || outcode2) {
        if (outcode1 & outcode2) return 0;     /* 同一外侧：整段在外 */

        if (outcode1) {
            if (outcode1 & RASTER_CODE_TOP) {
                y = recty1;
                x = (int)(x1 + ((RasterBig)(x2 - x1) * (y - y1)) / (y2 - y1));
            } else if (outcode1 & RASTER_CODE_BOTTOM) {
                y = recty2;
                x = (int)(x1 + ((RasterBig)(x2 - x1) * (y - y1)) / (y2 - y1));
            } else if (outcode1 & RASTER_CODE_LEFT) {
                x = rectx1;
                y = (int)(y1 + ((RasterBig)(y2 - y1) * (x - x1)) / (x2 - x1));
            } else if (outcode1 & RASTER_CODE_RIGHT) {
                x = rectx2;
                y = (int)(y1 + ((RasterBig)(y2 - y1) * (x - x1)) / (x2 - x1));
            }
            x1 = x; y1 = y;
            outcode1 = raster_out_code(rectx1, recty1, rectx2, recty2, x1, y1);
        } else {
            if (outcode2 & RASTER_CODE_TOP) {
                y = recty1;
                x = (int)(x1 + ((RasterBig)(x2 - x1) * (y - y1)) / (y2 - y1));
            } else if (outcode2 & RASTER_CODE_BOTTOM) {
                y = recty2;
                x = (int)(x1 + ((RasterBig)(x2 - x1) * (y - y1)) / (y2 - y1));
            } else if (outcode2 & RASTER_CODE_LEFT) {
                x = rectx1;
                y = (int)(y1 + ((RasterBig)(y2 - y1) * (x - x1)) / (x2 - x1));
            } else if (outcode2 & RASTER_CODE_RIGHT) {
                x = rectx2;
                y = (int)(y1 + ((RasterBig)(y2 - y1) * (x - x1)) / (x2 - x1));
            }
            x2 = x; y2 = y;
            outcode2 = raster_out_code(rectx1, recty1, rectx2, recty2, x2, y2);
        }
    }

    *X1 = x1; *Y1 = y1; *X2 = x2; *Y2 = y2;
    return 1;
}

/* 等价于 SDL_RenderDrawLine（不透明）。 */
static inline void raster_line(Raster *rs, int x0, int y0, int x1, int y1, uint32_t c)
{
    if (x0 == x1 && y0 == y1) {
        /* 退化成一点：画 1 个像素（不是"什么都不画"）。 */
        int x = x0, y = y0;
        if ((unsigned)(x - rs->clip.x) < (unsigned)rs->clip.w &&
            (unsigned)(y - rs->clip.y) < (unsigned)rs->clip.h)
            px_write(rs, x, y, c);
        return;
    }
    int ax = x0, ay = y0, bx = x1, by = y1;
    int inside = ((unsigned)ax < (unsigned)rs->w && (unsigned)ay < (unsigned)rs->h &&
                  (unsigned)bx < (unsigned)rs->w && (unsigned)by < (unsigned)rs->h);
    if (!inside && !raster_clip_line(rs, &ax, &ay, &bx, &by))
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

/* ---- UI 层用的带 alpha 原语（替代 SDL_BlendMode + SetRenderDrawColor）---- */

/* 单像素混色写入：a 为 0..255 的源 alpha。a>=255 直接写（不透明）；
   a==0 跳过；否则按 dst*(255-a)/255 + src*a/255 混合（与 SDL_BLENDMODE_BLEND
   一致）。越界不写。 */
void raster_px_rgba(Raster *rs, int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a);

/* 实心填充（可带 alpha），被裁剪到画布。替代 SDL_RenderFillRect。 */
void raster_fill_rect(Raster *rs, int x, int y, int w, int h,
                      uint8_t r, uint8_t g, uint8_t b, uint8_t a);

/* 1px 描边矩形（可带 alpha）。替代 SDL_RenderDrawRect。 */
void raster_rect(Raster *rs, int x, int y, int w, int h,
                 uint8_t r, uint8_t g, uint8_t b, uint8_t a);

/* 单像素线（可带 alpha），等价于 SDL_RenderDrawLine + alpha 混合。 */
void raster_line_rgba(Raster *rs, int x0, int y0, int x1, int y1,
                      uint8_t r, uint8_t g, uint8_t b, uint8_t a);

#endif /* NAVIMAP_RASTER_H */
