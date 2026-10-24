#include "render/raster.h"

/* 位偏移：把 r/g/b 按 mask 拼成一个 32 位字（见 raster_rgb）。 */
static int shift_of(uint32_t mask)
{
    if (!mask) return 0;
    int s = 0;
    while (!((mask >> s) & 1)) s++;
    return s;
}

int raster_bind_mem(Raster *rs, void *px, int w, int h, size_t stride,
                    uint32_t rmask, uint32_t gmask, uint32_t bmask, uint32_t amask)
{
    if (!px || w <= 0 || h <= 0) return -1;
    rs->px = (uint32_t *)px;
    rs->w = w;
    rs->h = h;
    rs->stride = stride;
    rs->clip.x = 0; rs->clip.y = 0; rs->clip.w = w; rs->clip.h = h;
    rs->amask = amask;
    rs->rmask = rmask; rs->gmask = gmask; rs->bmask = bmask;
    rs->rsh = (uint8_t)shift_of(rmask);
    rs->gsh = (uint8_t)shift_of(gmask);
    rs->bsh = (uint8_t)shift_of(bmask);
    return 0;
}

/* ★ alpha 混色公式：**必须与 SDL 软渲染器逐位一致**。
 *
 * SDL 的做法是"两项各自除再相加"：
 *     d = (s * a) / 255 + (d * (255 - a)) / 255     ← 两个独立的截断除法
 * 而"先加后除" (s*a + d*(255-a)) / 255 **差 1 级**：
 * 两项相加后再除会多保留一点小数（截断前的余数被合并），于是 14% 的
 * 混色像素会与 SDL 差 1（实测：bg(244,241,234) fg(255,186,64) a=200
 * → SDL 给 (252,196,100)，先加后除给 (252,197,100)）。
 *
 * 这个差异在肉眼下完全看不出来（面板颜色深浅差 1 级），但逐像素比对会报
 * 1.8% 差异，很容易被误判成"要素丢了"。所以这里按 SDL 的写法拆成两次
 * 除法，并用 tools/blend_equiv.c 实测钉死（36 组颜色 × 4096 像素必须 0 差异）。
 */
static inline uint8_t blend_ch(uint8_t s, uint8_t d, uint8_t a)
{
    return (uint8_t)((s * a) / 255 + (d * (255 - a)) / 255);
}

void raster_px_rgba(Raster *rs, int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    if ((unsigned)x >= (unsigned)rs->w || (unsigned)y >= (unsigned)rs->h) return;
    if (a >= 255) { px_write(rs, x, y, raster_rgb(rs, r, g, b)); return; }
    if (a == 0) return;
    uint32_t dst = rs->px[(size_t)y * rs->stride + x];
    uint8_t dr = (uint8_t)((dst >> rs->rsh) & 0xff);
    uint8_t dg = (uint8_t)((dst >> rs->gsh) & 0xff);
    uint8_t db = (uint8_t)((dst >> rs->bsh) & 0xff);
    uint8_t br = blend_ch(r, dr, a);
    uint8_t bg = blend_ch(g, dg, a);
    uint8_t bb = blend_ch(b, db, a);
    px_write(rs, x, y, raster_rgb(rs, br, bg, bb));
}

void raster_fill_rect(Raster *rs, int x, int y, int w, int h,
                      uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    if (w <= 0 || h <= 0) return;
    int x1 = x, y1 = y, x2 = x + w, y2 = y + h;
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > rs->w) x2 = rs->w;
    if (y2 > rs->h) y2 = rs->h;
    if (x1 >= x2 || y1 >= y2) return;

    if (a >= 255) {
        uint32_t c = raster_rgb(rs, r, g, b);
        for (int yy = y1; yy < y2; yy++) {
            uint32_t *p = rs->px + (size_t)yy * rs->stride;
            for (int xx = x1; xx < x2; xx++) p[xx] = c;
        }
        return;
    }
    for (int yy = y1; yy < y2; yy++)
        for (int xx = x1; xx < x2; xx++)
            raster_px_rgba(rs, xx, yy, r, g, b, a);
}

/* 裁剪后的带 alpha 线段光栅化（与 raster_line_clipped 同算法，hline 用混色）。 */
static void raster_line_rgba_clipped(Raster *rs, int x1, int y1, int x2, int y2,
                                    uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    if (y1 == y2) {                                   /* HLINE */
        int xlo = x1 <= x2 ? x1 : x2, xhi = x1 <= x2 ? x2 : x1;
        for (int x = xlo; x <= xhi; x++) raster_px_rgba(rs, x, y1, r, g, b, a);
        return;
    }
    if (x1 == x2) {                                   /* VLINE */
        int ylo = y1 <= y2 ? y1 : y2, yhi = y1 <= y2 ? y2 : y1;
        for (int y = ylo; y <= yhi; y++) raster_px_rgba(rs, x1, y, r, g, b, a);
        return;
    }
    int dx = x2 - x1, dy = y2 - y1;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    if (adx == ady) {                                 /* DLINE */
        int x, y, n, sx;
        if (y1 <= y2) { x = x1; y = y1; n = y2 - y1 + 1; sx = (x2 > x1) ? 1 : -1; }
        else          { x = x2; y = y2; n = y1 - y2 + 1; sx = (x1 > x2) ? 1 : -1; }
        for (int i = 0; i < n; i++) {
            raster_px_rgba(rs, x, y, r, g, b, a);
            x += sx; y++;
        }
        return;
    }
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
        raster_px_rgba(rs, x, y, r, g, b, a);
        if (d < 0) { d += dinc1; x += xinc1; y += yinc1; }
        else       { d += dinc2; x += xinc2; y += yinc2; }
    }
}

void raster_line_rgba(Raster *rs, int x0, int y0, int x1, int y1,
                      uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    if (a >= 255) { raster_line(rs, x0, y0, x1, y1, raster_rgb(rs, r, g, b)); return; }
    if (a == 0) return;
    if (x0 == x1 && y0 == y1) {
        raster_px_rgba(rs, x0, y0, r, g, b, a);
        return;
    }
    int ax = x0, ay = y0, bx = x1, by = y1;
    int inside = ((unsigned)ax < (unsigned)rs->w && (unsigned)ay < (unsigned)rs->h &&
                  (unsigned)bx < (unsigned)rs->w && (unsigned)by < (unsigned)rs->h);
    if (!inside && !raster_clip_line(rs, &ax, &ay, &bx, &by)) return;
    raster_line_rgba_clipped(rs, ax, ay, bx, by, r, g, b, a);
}

/* 1px 描边矩形（可带 alpha）。替代 SDL_RenderDrawRect。
 *
 * ★ 角点只画一次：SDL_RenderDrawRect 的四个角是**各画一次**（SDL 内部把
 *   描边拆成四条边，但角上只落一次笔）。若这里也画四条完整边，角上会被
 *   混色**两次**，半透明时颜色明显偏深（实测 a=128 白字：SDL 249 vs 双混 252）。
 *   所以上下边画满、左右边**去掉角**（各短 1px），保证每像素只混一次。
 */
void raster_rect(Raster *rs, int x, int y, int w, int h,
                 uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    if (w <= 0 || h <= 0) return;
    if (w == 1 && h == 1) { raster_px_rgba(rs, x, y, r, g, b, a); return; }

    /* 上下两条边：画满（含左右角） */
    raster_line_rgba(rs, x, y, x + w - 1, y, r, g, b, a);
    if (h > 1)
        raster_line_rgba(rs, x, y + h - 1, x + w - 1, y + h - 1, r, g, b, a);
    /* 左右两条边：去掉上下角（h<=2 时中间没有像素） */
    if (w > 1 && h > 2) {
        raster_line_rgba(rs, x, y + 1, x, y + h - 2, r, g, b, a);
        raster_line_rgba(rs, x + w - 1, y + 1, x + w - 1, y + h - 2, r, g, b, a);
    } else if (w > 1 && h == 2) {
        /* 高为 2：左右边只有角，而角已由上下边画过 ⇒ 不能再画 */
    }
}
