#include "render/font.h"

#include <SDL_ttf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils/plat.h"

/* UTF-8 解码：返回码点，*len 为字节数，非法序列返回 0xFFFD */
static uint32_t utf8_next(const char *s, int *len)
{
    const unsigned char *p = (const unsigned char *)s;
    if (p[0] < 0x80) { *len = 1; return p[0]; }
    if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
        *len = 2; return ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
    }
    if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        *len = 3; return ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    }
    if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 &&
        (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
        *len = 4;
        return ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) |
               ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
    }
    *len = 1;
    return 0xFFFD;
}

/* 码点 → UTF-8（≤0xFFFF），供 TTF_RenderUTF8_Blended 单字渲染 */
static void u8_encode(uint32_t cp, char *out)
{
    if (cp < 0x80) { out[0] = (char)cp; out[1] = 0; }
    else if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        out[2] = 0;
    } else {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        out[3] = 0;
    }
}

/* --- 字形缓存：链表桶哈希 --- */

typedef struct Glyph {
    struct Glyph *next;
    uint32_t      cp;
    SDL_Texture  *tex;    /* 属于创建时的 renderer */
    int           w, h;   /* 纹理尺寸 */
    int           advance;
} Glyph;

#define FONT_BUCKETS 4096

struct Font {
    TTF_Font *ttf;
    Glyph    *buckets[FONT_BUCKETS];
    SDL_Renderer *tex_owner;   /* 纹理归属（换 renderer 时清缓存） */
};

static unsigned glyph_hash(uint32_t cp)
{
    return (cp * 2654435761u) & (FONT_BUCKETS - 1);
}

/* ------------------------------------------------------------------ */
/* 字体选择：可选路径，默认宋体                                          */
/* ------------------------------------------------------------------ */

/* 宋体优先的候选链。排序理由：
 *   1) 地图标注要的是"衬线宋体 + GB18030 全覆盖"，生僻地名才有字；
 *   2) GB2312 版覆盖小，只在 GB18030 版缺失时兜底；
 *   3) 小标宋/仿宋是宋体变体，再往后才是黑体（最后兜底，保证一定有字）。
 *
 * ★ 候选链本身按平台给（utils/plat.c 的 nm_font_candidates）：
 *   Linux   /usr/share/fonts/...（本机实测有字的那几款）；
 *   Windows %WINDIR%/Fonts 下的 simsun/msyh/simhei/simfang/simkai；
 *   macOS   /System/Library/Fonts 与 /Library/Fonts 下的宋体/黑体/苹方。
 *   以前这里写死五条 /usr/share/fonts 绝对路径，换平台直接一个字体都没有
 *   （文字标注静默全灭）。命令行 --font 与环境变量 NAVMAP_FONT 优先级更高，
 *   见 ui/window.c 的 app_init。
 */

const char *font_default_path(void)
{
    size_t n = 0;
    const char *const *cands = nm_font_candidates(&n);
    for (size_t i = 0; i < n; i++) {
        FILE *fp = fopen(cands[i], "rb");
        if (fp) { fclose(fp); return cands[i]; }
    }
    return NULL;
}

Font *font_open_default(int ptsize)
{
    size_t n = 0;
    const char *const *cands = nm_font_candidates(&n);
    for (size_t i = 0; i < n; i++) {
        Font *f = font_open(cands[i], ptsize);
        if (f) return f;
    }
    return NULL;
}

Font *font_open(const char *path, int ptsize)
{
    return font_open_ex(path, ptsize, 0);
}

Font *font_open_ex(const char *path, int ptsize, int face_index)
{
    if (!path || !*path) return NULL;
    if (TTF_Init() != 0) {
        fprintf(stderr, "TTF_Init 失败: %s\n", TTF_GetError());
        return NULL;
    }
    Font *f = calloc(1, sizeof(Font));
    if (!f) return NULL;

    /* .ttc/.otc 是字体集合：TTF_OpenFont 只取 face 0，中文 ttc 的 face 0
       常常是日文/韩文面孔（本机 NotoSerifCJK-Regular.ttc 的 face 0 就是 KR），
       所以必须走 TTF_OpenFontIndex 让调用方能挑面孔。 */
    size_t len = strlen(path);
    int is_collection = 0;
    if (len >= 4) {
        const char *ext = path + len - 4;
        if (nm_strcasecmp(ext, ".ttc") == 0 || nm_strcasecmp(ext, ".otc") == 0)
            is_collection = 1;
    }
    f->ttf = is_collection ? TTF_OpenFontIndex(path, ptsize, face_index < 0 ? 0 : face_index)
                           : TTF_OpenFont(path, ptsize);
    if (!f->ttf) {
        fprintf(stderr, "打开字体 %s 失败: %s\n", path, TTF_GetError());
        free(f);
        return NULL;
    }
    TTF_SetFontHinting(f->ttf, TTF_HINTING_LIGHT);
    return f;
}

/* 光栅化 cp 并把纹理建到 renderer r 上。g->w/h/advance 一并填好。 */
static void glyph_build_tex(Font *f, SDL_Renderer *r, Glyph *g, uint32_t cp)
{
    if (cp > 0xFFFF) {
        /* 本机 SDL2_ttf 2.0.15 没有 32 位字形 API（2.0.18+ 才有）。
           中文全在 BMP 内；扩展 B 区生僻字暂时缺字形，只是少画不致错。 */
        g->advance = TTF_FontHeight(f->ttf) / 2;
        return;
    }
    if (!TTF_GlyphIsProvided(f->ttf, (Uint16)cp)) {
        g->advance = TTF_FontHeight(f->ttf) / 2;
        return;
    }
    char u8[4];
    u8_encode(cp, u8);
    SDL_Surface *s = TTF_RenderUTF8_Blended(f->ttf, u8,
                                            (SDL_Color){255,255,255,255});
    if (!s) return;
    g->tex = SDL_CreateTextureFromSurface(r, s);
    g->w = s->w; g->h = s->h;
    int adv, minx, maxx;
    if (TTF_GlyphMetrics(f->ttf, (Uint16)cp, &minx, &maxx,
                         NULL, NULL, &adv) == 0)
        g->advance = adv > 0 ? adv : (maxx - minx);
    if (g->advance <= 0) g->advance = s->w;
    SDL_FreeSurface(s);
}

static Glyph *glyph_get(Font *f, SDL_Renderer *r, uint32_t cp)
{
    if (f->tex_owner != r) {
        /* renderer 换了（离屏工具每帧新建 renderer 就会走到这）。
         * 注意：旧 renderer 被 SDL_DestroyRenderer 时它的纹理已经全部销毁，
         * 这里 g->tex 全是悬空指针 —— 绝不能再 DestroyTexture（会破坏 SDL
         * 内部状态，表现为之后 RenderCopy 静默画不出来）。只置 NULL，
         * 下次用到时按需重建。真实泄漏为零：资源归属旧 renderer，随它释放。 */
        for (int i = 0; i < FONT_BUCKETS; i++)
            for (Glyph *g = f->buckets[i]; g; g = g->next)
                g->tex = NULL;
        f->tex_owner = r;
    }

    unsigned h = glyph_hash(cp);
    for (Glyph *g = f->buckets[h]; g; g = g->next) {
        if (g->cp == cp) {
            /* 缓存命中也可能 tex 为空：换 renderer 被清掉了，或上次创建失败。
               必须按需重建，否则换 renderer 后一个字都画不出来。 */
            if (!g->tex && r) glyph_build_tex(f, r, g, cp);
            return g;
        }
    }

    Glyph *g = calloc(1, sizeof(Glyph));
    if (!g) return NULL;
    g->cp = cp;
    glyph_build_tex(f, r, g, cp);
    g->next = f->buckets[h];
    f->buckets[h] = g;
    return g;
}

void font_draw(Font *f, SDL_Renderer *r, int x, int y,
               const char *utf8, uint8_t cr, uint8_t cg, uint8_t cb,
               int shadow)
{
    if (!f || !utf8) return;
    const char *p = utf8;
    int cx = x;
    while (*p) {
        int len;
        uint32_t cp = utf8_next(p, &len);
        if (len <= 0) break;
        Glyph *g = glyph_get(f, r, cp);
        if (g && g->tex) {
            if (shadow) {
                SDL_SetTextureColorMod(g->tex, 0, 0, 0);
                SDL_Rect rc = { cx + 1, y + 1, g->w, g->h };
                SDL_RenderCopy(r, g->tex, NULL, &rc);
            }
            SDL_SetTextureColorMod(g->tex, cr, cg, cb);
            SDL_Rect rc = { cx, y, g->w, g->h };
            SDL_RenderCopy(r, g->tex, NULL, &rc);
        }
        cx += g ? g->advance : 8;
        p += len;
    }
}

int font_text_w(Font *f, const char *utf8)
{
    if (!f || !utf8) return 0;
    int w = 0, h = 0;
    /* 整串量宽，避免逐字缓存那些只为测量而生的 glyph */
    if (TTF_SizeUTF8(f->ttf, utf8, &w, &h) != 0) return 0;
    return w;
}

/* 渲染器要被销毁时调用：把缓存里的纹理指针全部作废。
   不能指望"换 renderer 时靠 tex_owner != r 自动清"——新 renderer 可能正好
   分配到旧的地址（窗口 resize 重建离屏渲染器就是这种场景），那时旧指针
   会被当成有效纹理用，SDL 内部状态直接被破坏（表现为画不出字或崩）。 */
void font_drop_textures(Font *f)
{
    if (!f) return;
    for (int i = 0; i < FONT_BUCKETS; i++)
        for (Glyph *g = f->buckets[i]; g; g = g->next)
            g->tex = NULL;
    f->tex_owner = NULL;
}

void font_close(Font *f)
{
    if (!f) return;
    for (int i = 0; i < FONT_BUCKETS; i++) {
        Glyph *g = f->buckets[i];
        while (g) {
            Glyph *nx = g->next;
            if (g->tex) SDL_DestroyTexture(g->tex);
            free(g);
            g = nx;
        }
    }
    if (f->ttf) TTF_CloseFont(f->ttf);
    free(f);
}
