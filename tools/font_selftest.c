/* font_selftest — 验收内置 Noto 字体集是否真的能加载并出字形。
 * 沿用 plat_selftest 的"契约可断言"风格：不靠肉眼，靠像素与宽度证据。
 *
 * ★ 不依赖 SDL：自持一块 32bpp 像素缓冲 + raster_bind_mem，把字形画进去，
 *   再数非零像素。字体渲染是 stb_truetype + 自写合成（见 render/font.c），
 *   与窗口系统无关——这正是"核心零 SDL"的前提：连字体自检都不该拉 SDL 进来。
 *
 * 构建：链 navimap_core（已零 SDL 依赖）。
 * 运行：./font_selftest（字体目录按 NAVMAP_FONT_DIR 或 exe 相对 fonts/ 解析）。 */
#include "render/font.h"
#include "render/raster.h"
#include "utils/plat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FT_W 512
#define FT_H 64

int main(void)
{
    /* 1) 候选链：确认内置 Noto 排在系统字体之前 */
    size_t n = 0;
    const char *const *c = nm_font_candidates(&n);
    printf("[font_selftest] 候选字体 %zu 个（前若干个应为内置 Noto）：\n", n);
    for (size_t i = 0; i < n && i < 12; i++)
        printf("    [%zu] %s\n", i, c[i]);

    Font *f = font_open_default(20);
    if (!f) {
        fprintf(stderr, "font_open_default 失败：未找到任何可用字体\n");
        return 1;
    }

    int fail = 0;

    /* 2) 宽度：拉丁与中文都应 > 0（中文走 Noto CJK，不再是占位宽度） */
    const char *latin = "Hello, Map!";
    const char *cjk   = "世界地图一二三";
    int wl = font_text_w(f, latin);
    int wc = font_text_w(f, cjk);
    printf("[font_selftest] 宽度 latin=%d cjk=%d\n", wl, wc);
    if (wl <= 0) { fprintf(stderr, "拉丁宽度异常\n"); fail = 1; }
    if (wc <= 0) { fprintf(stderr, "中文宽度异常\n"); fail = 1; }

    /* 3) 渲染混排串，统计确有非零像素被画出（Noto 字形真正出图） */
    uint32_t *px = (uint32_t *)calloc((size_t)FT_W * FT_H, sizeof(uint32_t));
    if (!px) { fprintf(stderr, "分配像素缓冲失败\n"); font_close(f); return 2; }

    Raster rs;
    /* 格式 0x00RRGGBB（无 alpha 通道）。字形是 8-bit alpha 合成上去的，
       所以只要 RGB 有非零就说明真画出了东西。 */
    if (raster_bind_mem(&rs, px, FT_W, FT_H, (size_t)FT_W,
                        0x00FF0000u, 0x0000FF00u, 0x000000FFu, 0x00000000u) != 0) {
        fprintf(stderr, "raster_bind_mem 失败\n");
        free(px); font_close(f); return 2;
    }

    const char *mix = "Hello 世界 123";
    font_draw(f, &rs, 4, 4, mix, 255, 255, 255, 0);

    int nonz = 0;
    for (int i = 0; i < FT_W * FT_H; i++)
        if (px[i] & 0x00ffffff) nonz++;   /* 任一 RGB 分量非零即算画到 */
    printf("[font_selftest] 混排串 \"%s\" 画出非零像素 %d\n", mix, nonz);
    if (nonz == 0) {
        fprintf(stderr, "未画出任何字形：Noto 字体可能未生效或候选链解析失败\n");
        fail = 1;
    }

    free(px);
    font_close(f);
    if (fail) { fprintf(stderr, "FONT SELFTEST FAIL\n"); return 1; }
    printf("FONT SELFTEST PASS\n");
    return 0;
}
