#ifndef NAVIMAP_FONT_H
#define NAVIMAP_FONT_H

#include <stdint.h>
#include "render/raster.h"

/* 字体：stb_truetype 光栅化 + 自写像素合成（不依赖 SDL_ttf / SDL 纹理）。
 *
 * 为什么缓存：中文常用码点几千个，每次都光栅化 + 合成会拖垮帧时间。按 codepoint
 * 缓存 8-bit alpha 字形位图，只在首次遇到某个码点时光栅化一次；绘制时把 alpha
 * 按文字颜色混进 Raster（含阴影）。
 *
 * 字体集合：多个面（每个候选文件一个面）。渲染时按码点选第一个含该字形的面
 * （per-glyph 回退），故中英混排能分别走 NotoSans / DroidSansFallback。 */

typedef struct Font Font;

/* 打开字体文件。单面 .ttf 用 index 0；.ttc/.otc 视为集合，取 index 指定的面
 * （face_index < 0 时取面 0，与旧 TTF_OpenFontIndex 行为一致）。 */
Font *font_open(const char *path, int ptsize);
Font *font_open_ex(const char *path, int ptsize, int face_index);

/* 默认字体集合：内置 Noto 系列优先、系统字体兜底（见 utils/plat.c）。
 * 候选链上所有能打开的字体都会被收进集合；渲染时按码点选第一个含该字形的
 * 面（per-glyph 回退）。想只拿到"打算用哪个文件"而不打开，用 font_default_path()。 */
Font      *font_open_default(int ptsize);
const char *font_default_path(void);

/* 画一行 UTF-8 文本（左上角对齐）。shadow 非零时先画黑影提高可读性。 */
void font_draw(Font *f, Raster *rs, int x, int y,
               const char *utf8, uint8_t cr, uint8_t cg, uint8_t cb,
               int shadow);

/* 文本像素宽度（含字间距，不含阴影偏移） */
int font_text_w(Font *f, const char *utf8);

/* 作废全部字形缓存（释放 alpha 位图内存；与渲染目标生命周期无关，可随时调）。 */
void font_drop_textures(Font *f);

/* ---- 诊断接口（tools/font_probe.c 用；不参与正常渲染路径）----
 *
 * 为什么需要：中文变方框（tofu）时，要能逐个字体回答"是**没找到字形**，
 * 还是**找到了但画不出来**（scale=0 / 位图全 0）"。这两种的修法完全不同，
 * 靠猜会来回折腾。
 *   - font_probe_face_count : 集合里有几个面
 *   - font_probe_face       : 第 i 个面的文件名与 scale（scale<=0 即不可用）
 *   - font_probe_glyph      : 某码点最终选了哪个面、glyph index 是否非 0、
 *                             光栅化出来的位图尺寸与非零像素数
 * 全部返回 0 表示"查不到"，调用方按"未知"处理，不要当成"有"。 */
int  font_probe_face_count(Font *f);
int  font_probe_face(Font *f, int i, const char **name, float *scale);
int  font_probe_glyph(Font *f, uint32_t cp, int *face, int *glyph_index,
                      int *w, int *h, int *nonzero_px);

void font_close(Font *f);

#endif
