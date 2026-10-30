/* skedaddle_blend_equiv — 验证自写光栅的 alpha 混色与 SDL 软渲染器逐像素等价。
 *
 * 为什么要它：raster.h 写着"与 SDL 软渲染器逐像素等价是硬要求"，但之前只
 * 验证了线段裁剪（4000 随机线段 + 1500 随机折线 0 差异），**没验证 alpha
 * 混色**。去 SDL 化之后 chrome 的半透明面板/圆角按钮全走 raster_px_rgba，
 * 若混色公式与 SDL 差 1 级，画面上就是"同一块面板颜色深浅略有不同"——
 * 肉眼看不出，但逐像素比对会报差异，必须查清是"公式不等价"还是"要素丢了"。
 *
 * 做法：同一组 (前景色, 背景色, alpha) 两条路各画一遍，逐像素比。
 *   SDL 路：SDL_CreateSoftwareRenderer + SDL_SetRenderDrawColor + FillRect
 *   raster 路：raster_px_rgba
 * 判据：差异必须是 0；若不为 0，报出最大的差值与出现次数（说明公式不等价）。
 *
 * ★ 覆盖四种图元（填充 / 描边矩形 / 线段 / 点），因为 chrome.c 里的
 *   rfill / rstroke / rline / raster_point 各走一条不同的 SDL 调用，
 *   语义差别各不相同（尤其 SDL_RenderDrawPoint 会做 alpha 混色，
 *   而 raster_point 是直接覆盖 —— 两者不等价，见下面的对照）。
 */
#include <SDL.h>
#include "render/raster.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 四种图元各测一遍。返回 0 = 该图元 0 差异。 */
static int test_shape(int shape)
{
    const int W = 64, H = 64;

    typedef struct { int r, g, b, a; } FgCol;
    typedef struct { int r, g, b; }     BgCol;
    static const FgCol FG[] = {
        {255,255,255,255}, {255,255,255,128}, {255,255,255,64},
        {  0,  0,  0,255}, {  0,  0,  0,128}, { 30,110,235,255},
        {255,186, 64,200}, {244,241,234, 90}, { 12, 18, 24,255},
    };
    static const BgCol BG[] = {
        {244,241,234}, {255,255,255}, { 40, 44, 52}, {200,210,220},
    };

    int total_cmp = 0, total_diff = 0, max_delta = 0;

    for (size_t fi = 0; fi < sizeof(FG)/sizeof(FG[0]); fi++) {
        for (size_t bi = 0; bi < sizeof(BG)/sizeof(BG[0]); bi++) {
            const FgCol fg = FG[fi];
            const BgCol bg = BG[bi];

            SDL_Surface *s = SDL_CreateRGBSurface(0, W, H, 32, 0, 0, 0, 0);
            if (!s) { fprintf(stderr, "surface 失败\n"); return 2; }
            SDL_Renderer *r = SDL_CreateSoftwareRenderer(s);
            if (!r) { fprintf(stderr, "renderer 失败\n"); return 2; }
            SDL_SetRenderDrawColor(r, (Uint8)bg.r, (Uint8)bg.g, (Uint8)bg.b, 255);
            SDL_RenderClear(r);

            uint32_t *px = (uint32_t *)malloc((size_t)W * H * sizeof(uint32_t));
            if (!px) { fprintf(stderr, "malloc 失败\n"); return 2; }
            Raster rs;
            raster_bind_mem(&rs, px, W, H, (size_t)W,
                            s->format->Rmask, s->format->Gmask,
                            s->format->Bmask, s->format->Amask);
            raster_fill_rect(&rs, 0, 0, W, H, (uint8_t)bg.r, (uint8_t)bg.g,
                             (uint8_t)bg.b, 255);

            /* --- 画图元：SDL 路与 raster 路各画一遍 --- */
            switch (shape) {
            case 0:   /* 填充矩形 */
                SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(r, (Uint8)fg.r, (Uint8)fg.g, (Uint8)fg.b, (Uint8)fg.a);
                { SDL_Rect q = { 8, 8, 40, 30 }; SDL_RenderFillRect(r, &q); }
                raster_fill_rect(&rs, 8, 8, 40, 30, (uint8_t)fg.r, (uint8_t)fg.g,
                                 (uint8_t)fg.b, (uint8_t)fg.a);
                break;
            case 1:   /* 描边矩形（1px 框） */
                SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(r, (Uint8)fg.r, (Uint8)fg.g, (Uint8)fg.b, (Uint8)fg.a);
                { SDL_Rect q = { 8, 8, 40, 30 }; SDL_RenderDrawRect(r, &q); }
                raster_rect(&rs, 8, 8, 40, 30, (uint8_t)fg.r, (uint8_t)fg.g,
                            (uint8_t)fg.b, (uint8_t)fg.a);
                break;
            case 2:   /* 线段（斜线，最能暴露光栅化差异） */
                SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(r, (Uint8)fg.r, (Uint8)fg.g, (Uint8)fg.b, (Uint8)fg.a);
                SDL_RenderDrawLine(r, 3, 5, 55, 47);
                raster_line_rgba(&rs, 3, 5, 55, 47, (uint8_t)fg.r, (uint8_t)fg.g,
                                 (uint8_t)fg.b, (uint8_t)fg.a);
                break;
            case 3:   /* 单点：chrome.c 的画点处 blend 模式是 NONE、alpha 恒 255
                       （rfill/rstroke 都会把 blend 模式复位成 NONE），
                       所以这里按**应用实际用法**测：直接覆盖，不混色。 */
                SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
                SDL_SetRenderDrawColor(r, (Uint8)fg.r, (Uint8)fg.g, (Uint8)fg.b, 255);
                SDL_RenderDrawPoint(r, 32, 32);
                raster_point(&rs, 32, 32, raster_rgb(&rs, (uint8_t)fg.r, (uint8_t)fg.g,
                                                     (uint8_t)fg.b));
                break;
            }
            SDL_RenderPresent(r);

            const uint32_t *sp = (const uint32_t *)s->pixels;
            int shown = 0;
            for (int i = 0; i < W * H; i++) {
                total_cmp++;
                uint32_t a = sp[i] & 0x00FFFFFFu;
                uint32_t b = px[i] & 0x00FFFFFFu;
                if (a != b) {
                    total_diff++;
                    int dr = (int)((a >> 16) & 0xFF) - (int)((b >> 16) & 0xFF);
                    int dg = (int)((a >>  8) & 0xFF) - (int)((b >>  8) & 0xFF);
                    int db = (int)(a & 0xFF) - (int)(b & 0xFF);
                    int d = dr < 0 ? -dr : dr;
                    int t = dg < 0 ? -dg : dg; if (t > d) d = t;
                    t = db < 0 ? -db : db;       if (t > d) d = t;
                    if (d > max_delta) max_delta = d;
                    if (shown < 6) {
                        printf("      x=%2d y=%2d SDL(%3d,%3d,%3d) raster(%3d,%3d,%3d)\n",
                               i % W, i / W,
                               (int)((a >> 16) & 0xFF), (int)((a >> 8) & 0xFF), (int)(a & 0xFF),
                               (int)((b >> 16) & 0xFF), (int)((b >> 8) & 0xFF), (int)(b & 0xFF));
                        shown++;
                    }
                }
            }
            free(px);
            SDL_DestroyRenderer(r);
            SDL_FreeSurface(s);
        }
    }
    printf("  %-14s compare %6d  diff %6d  max_delta %d\n",
           shape == 0 ? "填充矩形" : shape == 1 ? "描边矩形" :
           shape == 2 ? "线段" : "单点", total_cmp, total_diff, max_delta);
    return total_diff;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 2;
    }

    static const char *SHAPE[] = { "填充矩形", "描边矩形", "线段", "单点" };
    int bad = 0;
    for (int sh = 0; sh < 4; sh++) {
        if (test_shape(sh) != 0) {
            printf("  >>> %s 与 SDL 不等价\n", SHAPE[sh]);
            bad++;
        }
    }
    if (bad) {
        printf("BLEND-EQUIV-FAIL：%d 种图元与 SDL 不等价\n", bad);
        SDL_Quit();
        return 1;
    }
    printf("BLEND-EQUIV-OK（填充/描边/线段/单点 均与 SDL 软渲染器逐像素一致）\n");
    SDL_Quit();
    return 0;
}
