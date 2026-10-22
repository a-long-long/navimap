#ifndef NAVIMAP_FONT_H
#define NAVIMAP_FONT_H

#include <SDL.h>
#include <stdint.h>

/* TTF 字体 + 字形纹理缓存。
 *
 * 为什么缓存：中文常用码点几千个，TTF_Render* 每次都是光栅化 + 内存纹理
 * 上传，逐帧做会拖垮帧时间。按 codepoint 缓存 SDL_Texture，只在首次遇到
 * 时渲染一次。
 *
 * 注意：纹理挂在创建它时的 renderer 上。软件渲染器每帧是同一个 renderer
 * 没问题；map_shot 的离屏 renderer 每进程一个，也没问题。
 */

typedef struct Font Font;

/* 打开字体文件。单面 .ttf 用 index 0；.ttc 必须用 TTF_OpenFontIndex
 * 指定中文 face，别用 face 0（可能是日文/韩文面孔）。
 * face_index < 0 时自动判断：.ttc/.otc 视为集合，取 index 指定的面孔。 */
Font *font_open(const char *path, int ptsize);
Font *font_open_ex(const char *path, int ptsize, int face_index);

/* 默认字体：宋体优先，逐个尝试候选，返回第一个能打开的（全失败返回 NULL）。
 * 想只拿到"打算用哪个文件"而不打开，用 font_default_path()。 */
Font      *font_open_default(int ptsize);
const char *font_default_path(void);

/* 画一行 UTF-8 文本（左上角对齐）。shadow 非零时先画黑影提高可读性。 */
void font_draw(Font *f, SDL_Renderer *r, int x, int y,
               const char *utf8, uint8_t cr, uint8_t cg, uint8_t cb,
               int shadow);

/* 文本像素宽度（含字间距，不含阴影偏移） */
int font_text_w(Font *f, const char *utf8);

/* 作废全部字形纹理（要销毁它所属的那个 renderer 之前必须调）。 */
void font_drop_textures(Font *f);

void font_close(Font *f);

#endif
