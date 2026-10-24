#include "render/font.h"
#include "render/raster.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils/plat.h"

/* stb_truetype 单头文件实现。它自带 static 函数，部分在"只用 GetCodepointBitmap"
 * 的路径下不会被调用，开 -Wextra 会触发 -Wunused-function；用 pragma 把这一段的
 * 告警压掉，保持工程"零警告"门禁。 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#define STB_TRUETYPE_IMPLEMENTATION
#include "render/stb_truetype.h"
#pragma GCC diagnostic pop

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

/* 整文件读进内存（stb 需要整段数据常驻）。失败返回 NULL。 */
static unsigned char *read_file(const char *path, size_t *out_len)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long sz = ftell(fp);
    if (sz <= 0) { fclose(fp); return NULL; }
    if (fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return NULL; }
    unsigned char *buf = (unsigned char *)malloc((size_t)sz);
    if (!buf) { fclose(fp); return NULL; }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        free(buf); fclose(fp); return NULL;
    }
    fclose(fp);
    *out_len = (size_t)sz;
    return buf;
}

/* 复制字符串（诊断用的 path 字段持有副本）。失败返回 NULL。 */
static char *dup_str(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

/* --- 字形缓存：链表桶哈希 --- */

typedef struct Glyph {
    struct Glyph *next;
    uint32_t      cp;
    uint8_t      *alpha;  /* w*h 8-bit 覆盖度；NULL = 该码点没有字形 */
    int           w, h;   /* 字形位图尺寸 */
    int           xoff, yoff;  /* 基线偏移（可负，来自 stb） */
    int           advance;     /* 像素 advance（已 ×scale） */
    int           face;        /* 命中面下标 */
} Glyph;

#define FONT_BUCKETS 4096

/* 字体面：stb 需要一个常驻的数据缓冲 + 一个 fontinfo（内部指针指向该缓冲）。 */
typedef struct {
    stbtt_fontinfo info;
    int            file_idx;   /* Font.files[] 下标（同一缓冲可多面共享） */
    float          scale;      /* 该面在 ptsize 下的缩放 */
    char          *path;       /* 该面来自哪个文件（诊断用；**本结构持有**，close 时释放） */
} Face;

/* 字体集合：多个面。渲染时按码点选第一个含该字形的面（per-glyph 回退）。 */
struct Font {
    int        n;          /* 面数量（>=1） */
    Face      *faces;
    Glyph    *buckets[FONT_BUCKETS];
    unsigned char **files;  /* 各面的原始文件数据（本结构持有，关闭时释放） */
    int        nfiles;
    int        ptsize;     /* 打开时给定的字号（像素高） */
};

static unsigned glyph_hash(uint32_t cp)
{
    return (cp * 2654435761u) & (FONT_BUCKETS - 1);
}

/* ------------------------------------------------------------------ */
/* 字体选择：内置 Noto 系列优先，系统字体兜底（见 utils/plat.c）         */
/* ------------------------------------------------------------------ */

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

/* 把一段字体数据初始化成一个面，填好 scale。成功返回 0。 */
static int face_init(Face *fa, unsigned char *data, int face_index, int ptsize)
{
    int off = stbtt_GetFontOffsetForIndex(data, face_index);
    if (off < 0) return -1;
    if (!stbtt_InitFont(&fa->info, data, off)) return -1;
    int ascent = 0, descent = 0, line_gap = 0;
    stbtt_GetFontVMetrics(&fa->info, &ascent, &descent, &line_gap);
    fa->scale = stbtt_ScaleForPixelHeight(&fa->info, (float)ptsize);
    return 0;
}

/* 打开单个文件为一个字体集合（1 面）。.ttc/.otc 取 face_index 指定面。 */
Font *font_open_ex(const char *path, int ptsize, int face_index)
{
    if (!path || !*path) return NULL;
    size_t len = 0;
    unsigned char *data = read_file(path, &len);
    if (!data) return NULL;

    int fi = (face_index < 0) ? 0 : face_index;
    Face *faces = (Face *)malloc(sizeof(Face));
    if (!faces) { free(data); return NULL; }
    if (face_init(&faces[0], data, fi, ptsize) != 0) {
        free(faces); free(data); return NULL;
    }
    faces[0].file_idx = 0;
    faces[0].path = dup_str(path);

    Font *f = (Font *)calloc(1, sizeof(Font));
    if (!f) { free(faces); free(data); return NULL; }
    f->n = 1;
    f->faces = faces;
    f->files = (unsigned char **)malloc(sizeof(unsigned char *));
    if (!f->files) { free(faces); free(data); free(f); return NULL; }
    f->files[0] = data;
    f->nfiles = 1;
    f->ptsize = ptsize;
    return f;
}

Font *font_open(const char *path, int ptsize)
{
    return font_open_ex(path, ptsize, 0);
}

/* 默认字体集合：逐个尝试候选链上的文件，全部能打开的都收进集合。 */
Font *font_open_default(int ptsize)
{
    size_t n = 0;
    const char *const *cands = nm_font_candidates(&n);
    if (n == 0) return NULL;

    /* 候选个数已知 ⇒ 数组一次分配到位，**不用 realloc**。
       （原实现用 realloc 扩容，失败分支里先 free(files) 再读 files[k]，
        是货真价实的 use-after-free——GCC 的 -Wuse-after-free 正是报这里；
        且 realloc 返回 NULL 时原指针被覆盖 ⇒ 泄漏。零警告门禁必须修。） */
    int    cnt = 0, nfiles = 0;
    Face  *faces = (Face *)calloc(n, sizeof(Face));
    unsigned char **files = (unsigned char **)calloc(n, sizeof(unsigned char *));
    if (!faces || !files) {
        free(faces); free(files);
        return NULL;
    }
    for (size_t i = 0; i < n; i++) {
        size_t len = 0;
        unsigned char *data = read_file(cands[i], &len);
        if (!data) continue;
        int fi = 0;   /* 集合默认取面 0，与旧 TTF_OpenFont 行为一致 */
        if (face_init(&faces[cnt], data, fi, ptsize) != 0) { free(data); continue; }
        faces[cnt].file_idx = nfiles;
        faces[cnt].path = dup_str(cands[i]);
        files[nfiles++] = data;
        cnt++;
    }
    if (cnt == 0) {
        free(faces); free(files);
        return NULL;
    }
    Font *f = (Font *)calloc(1, sizeof(Font));
    if (!f) {
        for (int k = 0; k < nfiles; k++) free(files[k]);
        free(faces); free(files);
        return NULL;
    }
    f->n = cnt;
    f->faces = faces;
    f->files = files;
    f->nfiles = nfiles;
    f->ptsize = ptsize;
    fprintf(stderr, "[font] 默认字体集合：载入 %d 个面（首面 %s）\n",
            cnt, n ? cands[0] : "(空)");
    return f;
}

/* 选面：返回第一个含该字形（glyph index 非 0）的面下标；都没有则回退 0。 */
static int glyph_face(Font *f, uint32_t cp)
{
    for (int i = 0; i < f->n; i++)
        if (stbtt_FindGlyphIndex(&f->faces[i].info, (int)cp) != 0)
            return i;
    return 0;
}

/* 光栅化 cp 到 g->alpha（用 face 指定的面）。g->w/h/advance/xoff/yoff 一并填好。
   缺失字形：alpha 留 NULL，advance 估一个。 */
static void glyph_build(Glyph *g, Font *f, uint32_t cp, int face)
{
    Face *fa = &f->faces[face];
    int gi = stbtt_FindGlyphIndex(&fa->info, (int)cp);
    if (gi == 0) {
        int ascent = 0, descent = 0, lg = 0;
        stbtt_GetFontVMetrics(&fa->info, &ascent, &descent, &lg);
        int fh = (int)(fa->scale * (ascent - descent) + 0.5);
        g->advance = fh / 2;
        g->w = g->h = 0;
        g->alpha = NULL;
        return;
    }
    int w = 0, h = 0, xoff = 0, yoff = 0;
    unsigned char *bmp = stbtt_GetCodepointBitmap(&fa->info, fa->scale, fa->scale,
                                                  (int)cp, &w, &h, &xoff, &yoff);
    g->w = w; g->h = h; g->xoff = xoff; g->yoff = yoff;
    g->alpha = bmp;
    int adv = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(&fa->info, (int)cp, &adv, &lsb);
    g->advance = (int)(adv * fa->scale + 0.5);
    if (g->advance <= 0) g->advance = w;
}

static Glyph *glyph_get(Font *f, uint32_t cp)
{
    unsigned h = glyph_hash(cp);
    for (Glyph *g = f->buckets[h]; g; g = g->next)
        if (g->cp == cp) return g;

    int face = glyph_face(f, cp);
    Glyph *g = (Glyph *)calloc(1, sizeof(Glyph));
    if (!g) return NULL;
    g->cp = cp;
    g->face = face;
    glyph_build(g, f, cp, face);
    g->next = f->buckets[h];
    f->buckets[h] = g;
    return g;
}

/* 把字形 alpha 按颜色 (r,g,b,a) 合成进 Raster（带阴影偏移）。 */
static void blit_glyph(Raster *rs, const Glyph *gl, int px, int py,
                       uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    int ox = px + gl->xoff, oy = py + gl->yoff;
    for (int yy = 0; yy < gl->h; yy++) {
        int sy = oy + yy;
        if (sy < 0 || sy >= rs->h) continue;
        const uint8_t *src = gl->alpha + (size_t)yy * gl->w;
        for (int xx = 0; xx < gl->w; xx++) {
            uint8_t av = src[xx];
            if (!av) continue;
            raster_px_rgba(rs, ox + xx, sy, r, g, b,
                           (uint8_t)((av * (unsigned)a) / 255));
        }
    }
}

void font_draw(Font *f, Raster *rs, int x, int y,
               const char *utf8, uint8_t cr, uint8_t cg, uint8_t cb,
               int shadow)
{
    if (!f || !utf8 || !rs) return;
    const char *p = utf8;
    int cx = x;
    while (*p) {
        int len;
        uint32_t cp = utf8_next(p, &len);
        if (len <= 0) break;
        Glyph *g = glyph_get(f, cp);
        if (g && g->alpha && g->w && g->h) {
            if (shadow)
                blit_glyph(rs, g, cx + 1, y + 1, 0, 0, 0, 200);
            blit_glyph(rs, g, cx, y, cr, cg, cb, 255);
        }
        cx += g ? g->advance : (f->ptsize / 2);
        p += len;
    }
}

int font_text_w(Font *f, const char *utf8)
{
    if (!f || !utf8 || f->n == 0) return 0;
    const char *p = utf8;
    int total = 0;
    while (*p) {
        int len;
        uint32_t cp = utf8_next(p, &len);
        if (len <= 0) break;
        int face = glyph_face(f, cp);
        int adv = 0;
        if (stbtt_FindGlyphIndex(&f->faces[face].info, (int)cp) != 0) {
            int a = 0, lsb = 0;
            stbtt_GetCodepointHMetrics(&f->faces[face].info, (int)cp, &a, &lsb);
            adv = (int)(a * f->faces[face].scale + 0.5);
        }
        if (adv <= 0) {
            int ascent = 0, descent = 0, lg = 0;
            stbtt_GetFontVMetrics(&f->faces[face].info, &ascent, &descent, &lg);
            adv = ((int)(f->faces[face].scale * (ascent - descent) + 0.5)) / 2;
        }
        total += adv;
        p += len;
    }
    return total;
}

/* 释放全部缓存的字形 alpha 位图（内存回收用；与渲染目标生命周期无关）。 */
void font_drop_textures(Font *f)
{
    if (!f) return;
    for (int i = 0; i < FONT_BUCKETS; i++) {
        Glyph *g = f->buckets[i];
        while (g) {
            Glyph *nx = g->next;
            if (g->alpha) stbtt_FreeBitmap(g->alpha, NULL);
            free(g);
            g = nx;
        }
        f->buckets[i] = NULL;
    }
}

/* ---- 诊断接口（tools/font_probe.c 用）----
 *
 * 目的是把"中文变方框"这件事拆成可分别判定的两问：
 *   ① **有没有这个字形**（stbtt_FindGlyphIndex != 0）？
 *   ② **有的话，光栅化出来的东西是不是真的画得出来**（位图非空、有非零像素）？
 * 只看 ① 会漏掉"字形在但 scale=0 ⇒ 位图全空"这种情况；
 * 只看 ② 会漏掉"这个字体没这个字形，该换下一个面"这种情况。 */
int font_probe_face_count(Font *f)
{
    return f ? f->n : 0;
}

int font_probe_face(Font *f, int i, const char **name, float *scale)
{
    if (!f || i < 0 || i >= f->n) return 0;
    if (name)  *name  = f->faces[i].path;
    if (scale) *scale = f->faces[i].scale;
    return 1;}

int font_probe_glyph(Font *f, uint32_t cp, int *face, int *glyph_index,
                      int *w, int *h, int *nonzero_px)
{
    if (!f) return 0;
    int fi = glyph_face(f, cp);
    const Face *fa = &f->faces[fi];
    int gi = stbtt_FindGlyphIndex(&fa->info, (int)cp);

    if (face)       *face = fi;
    if (glyph_index) *glyph_index = gi;
    if (w) *w = 0;
    if (h) *h = 0;
    if (nonzero_px) *nonzero_px = 0;
    if (gi == 0) return 1;              /* 该面没有这个字形（信息已填） */

    int gw = 0, gh = 0, xoff = 0, yoff = 0;
    unsigned char *bmp = stbtt_GetCodepointBitmap(&fa->info, fa->scale, fa->scale,
                                                  (int)cp, &gw, &gh, &xoff, &yoff);
    if (w) *w = gw;
    if (h) *h = gh;
    if (bmp) {
        int nz = 0;
        for (int i = 0; i < gw * gh; i++) if (bmp[i]) nz++;
        if (nonzero_px) *nonzero_px = nz;
        stbtt_FreeBitmap(bmp, NULL);
    }
    return 1;
}

void font_close(Font *f)
{
    if (!f) return;
    font_drop_textures(f);
    if (f->faces) {
        for (int i = 0; i < f->n; i++) free(f->faces[i].path);
        free(f->faces);
    }
    if (f->files) {
        for (int i = 0; i < f->nfiles; i++) free(f->files[i]);
        free(f->files);
    }
    free(f);
}
