# 第 8 课：软件光栅与性能旋钮

> 模块：`src/render/raster.{c,h}`
> 前置：第 2 课坐标系、第 7 课 `mv_proj`、Cohen-Sutherland 线段裁剪概念。

## 一、教学目的

理解"为什么要自己写光栅"——而不是用 SDL 软渲染器的逐次 `DrawLine`/`FillRect`。学完后学生应能：
- 解释 SDL 软渲染器的"每次调用固定税"是怎么把一帧拖垮的；
- 口述 `raster_line` 的裁剪→光栅化两段式结构，以及为什么 Cohen-Sutherland 必须"逐字移植"；
- 说明 64 位中间量（`RasterBig`）防溢出、交点差值方向 `(x2-x1)/(y2-y1)` 这两处踩坑；
- 说清 `raster_rgb`/`px_write`/`raster_px_rgba` 这些"像素直写"原语，就是与 SDL 软渲染逐像素等价的自写光栅底座。

## 二、功能构成

| 关键结构体 / 函数 | 文件:行号区间 | 职责 |
|---|---|---|
| `Raster` | `raster.h:29-37` | 像素缓冲 + 裁剪矩形的自写光栅上下文（取代 `SDL_Surface`/`SDL_Renderer`） |
| `shift_of` | `raster.c:4-10` | 从颜色 mask 算出像素打包时的位偏移 `rsh/gsh/bsh` |
| `raster_rgb` | `raster.h:44-50` | 把 r/g/b 按 mask 打包成 32 位字（等价于 `SDL_MapRGBA(...,255)`） |
| `px_write` | `raster.h:52-55` | 像素直写（调用方已保证在画布内） |
| `raster_point` / `raster_hspan` | `raster.h:57-73` | 单点 / 整行水平填充原语 |
| `blend_ch` | `raster.c:42-45` | 单通道 alpha 混色（与 SDL `BLENDMODE_BLEND` 逐位一致） |
| `raster_px_rgba` | `raster.c:47-60` | 带 alpha 的单像素混色写入 |
| `raster_fill_rect` / `raster_rect` | `raster.c:62-84` / `raster.c:159-176` | 实心填充 / 1px 描边矩形（替代 `SDL_RenderFillRect`/`DrawRect`） |
| `raster_out_code` / `raster_clip_line` | `raster.h:154-257` | 逐字移植 `SDL_IntersectRectAndLine` 的 Cohen-Sutherland 裁剪 |
| `raster_line_clipped` | `raster.h:77-129` | 裁剪后线段光栅化（HLINE/VLINE/DLINE/BLINE） |
| `raster_line` | `raster.h:260-276` | 对外的不透明画线入口：退化点 + 裁剪 + 光栅化 |

`Raster` 取代的是 `SDL_RenderDrawLine`/`SDL_RenderFillRect` 这一套：它自己持有 `uint32_t *px` 像素首地址与 `stride`，所有绘制都直接写内存，不再经过 SDL 的每一帧调用税。64 位中间量出现在 `raster_clip_line` 的交点公式里（见下）。

## 三、完整实现代码

### 1. `Raster` 结构体（`raster.h:29-37`）

```c
typedef struct {
    uint32_t *px;      /* 像素首地址（调用方持有缓冲） */
    int       w, h;
    size_t    stride;  /* 每行多少个 uint32_t */
    RectI     clip;    /* 裁剪矩形（inclusive），等同 SDL 的 clip_rect */
    uint32_t  amask;   /* 打包颜色时补上满 alpha（没有 alpha 通道时是 0） */
    uint32_t  rmask, gmask, bmask;
    uint8_t   rsh, gsh, bsh;
} Raster;
```

### 2. `shift_of`（`raster.c:4-10`）

从颜色 mask 里数出最低置位的位置，用于 `raster_rgb` 把通道打包到正确字节：

```c
/* 位偏移：把 r/g/b 按 mask 拼成一个 32 位字（见 raster_rgb）。 */
static int shift_of(uint32_t mask)
{
    if (!mask) return 0;
    int s = 0;
    while (!((mask >> s) & 1)) s++;
    return s;
}
```

### 3. 颜色打包与像素直写（`raster.h:44-73`）

```c
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
```

### 4. alpha 混色与带 alpha 写入（`raster.c:29-60`）

```c
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
```

### 5. 实心填充与 1px 描边矩形（`raster.c:62-84` 与 `raster.c:159-176`）

```c
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
```

```c
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
```

### 6. 线段裁剪与光栅化（含 Cohen-Sutherland、64 位中间量防溢出）

`raster_line` 是对外入口：退化点直接落单像素；否则先做画布内快速判定，必要才走 `raster_clip_line` 的 Cohen-Sutherland 裁剪；裁剪后的真实端点交给 `raster_line_clipped` 光栅化。

```c
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
```

```c
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
```

> 注意 `raster_line` 里的快速判定 `inside`：四端都在画布内就跳过裁剪；否则才调 `raster_clip_line`。裁剪里的交点公式全部用 `RasterBig`（即 `int64_t`）做 `(x2-x1)*(y-y1)` 这种乘法，避免 32 位溢出——这是"64 位中间量防溢出"的落点。

## 四、与其他功能之间的联系

- **上游 · 第 2 / 7 课**：`raster_*` 只接收屏幕坐标 `(x, y)`；这些坐标由第 7 课 `mv_proj` 把 `lat/lon` 投影而来。光栅层本身不做任何地理换算。
- **下游 · 第 9 课（渲染管线）**：`paint_areas/roads/lines/contours` 与 `navdraw` 全部把 `Raster *rs` 当绘制目标，经 `paint_frame` 组织成每帧。
- **下游 · 第 14 课（架构门禁）**：`raster_clip_line` 由 `tools/clip_equiv.c` 门禁（20 万随机线段比 `SDL_IntersectRectAndLine` 端点逐位相同）；`blend_ch` 由 `tools/blend_equiv.c` 门禁（36 组颜色 × 4096 像素 0 差异）。"与 SDL 软渲染逐像素等价"是硬要求，不是"看着差不多"。

## 五、演示、练习与验收

- **演示**：跑裁剪等价门禁，看 0 差异断言通过：
  `./build_rel/tools/clip_equiv`（逐字比对 `SDL_IntersectRectAndLine` 与 `raster_clip_line`）。
- **演示**：跑混色等价门禁：
  `./build_rel/tools/blend_equiv`（36 组颜色 × 4096 像素逐位相同）。
- **练习**：在 `raster_clip_line` 里把某处差值方向改成 `(x1-x2)`，重跑 `clip_equiv`，观察跨界线段差 1 像素的断言被抓出；改回。
- **验收**：能口述 `raster_line` 的"退化点 → 画布内快速判定 → Cohen-Sutherland 裁剪 → 裁剪后光栅化"四段结构，并解释 64 位中间量与差值方向这两处为"逐位等价 SDL"而存在的细节。
