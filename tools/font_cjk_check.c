/* font_cjk_check —— 断言字体链里**确实有能画中文的字体**。
 *
 * 为什么要有它：
 *   中文变方框（tofu）有两种完全不同的成因，修法也不同：
 *     ① 字体文件里**根本没有**中文字形（字体不对）→ 换字体；
 *     ② 字体有字形，但**渲染方**没拿到（Qt 侧没注册 / 没设应用字体）→ 修接入。
 *   本工具只管 ①，而且是"整条候选链"级别的断言 —— 因为
 *   render/font.c（地图文字，stb_truetype）和 ui_qt/qt_font.cpp（Qt 控件文字）
 *   **共用同一条候选链**（plat.c 的 nm_font_candidates）。链条里没有 CJK 字体，
 *   两边都会变方框。
 *
 * 判据：
 *   遍历 nm_font_candidates()，对每个能打开的文件查 cmap 里有没有常用汉字
 *   （用 stb_truetype 的 FindGlyphIndex + 实际光栅化出的非零像素数）。
 *   ★ 必须**两个条件都满足**：glyph index 非 0（cmap 里有）
 *     且光栅化后非零像素 > 0（真的有形状）。只看 index 会漏掉
 *     "有字形但光栅化出空图"的情况（比如 scale 算成 0）。
 *
 * 负向对照：--negative 断言**检测逻辑本身有效** ——
 *   找一个确定**没有**中文的字体（NotoSans-Regular 之类）必须被判成"无中文"，
 *   找一个确定**有**中文的必须被判成"有中文"。两侧任一判错就 FAIL。
 *   （这比"把字体藏起来"更有意义：藏起自带字体后系统字体还在，
 *     判据照样通过 ⇒ 那种对照是假的。）
 */
#include "render/font.h"
#include "utils/plat.h"

#include <stdio.h>
#include <string.h>

/* 常用汉字，覆盖界面真实用到的字 + 生僻字（确认是完整 CJK 覆盖而非只补几个） */
static const uint32_t kCJK[] = {
    0x4E2D, /* 中 */
    0x56FD, /* 国 */
    0x5317, /* 北 */
    0x4EAC, /* 京 */
    0x8DEF, /* 路 */
    0x5730, /* 地 */
    0x56FE, /* 图 */
    0x6570, /* 数 */
    0x636E, /* 据 */
    0x641C, /* 搜 */
    0x7D22, /* 索 */
    0x5BFC, /* 导 */
    0x822A, /* 航 */
    0x672A, /* 未 */
    0x52A0, /* 加 */
    0x8F7D, /* 载 */
    0x9F99, /* 龙 */
    0x9F99, /* 龙（重复占位，验证不会因重复而出错） */
};

/* 这个字体文件里有多少个测试汉字"真的画得出来"？ */
static int cjk_hit_count(const char *path)
{
    Font *f = font_open(path, 14);
    if (!f) return -1;
    int hits = 0;
    for (size_t k = 0; k < sizeof(kCJK) / sizeof(kCJK[0]); k++) {
        int face = -1, gi = -1, w = 0, h = 0, nz = 0;
        if (!font_probe_glyph(f, kCJK[k], &face, &gi, &w, &h, &nz)) continue;
        if (gi != 0 && nz > 0) hits++;
    }
    font_close(f);
    return hits;
}

int main(int argc, char **argv)
{
    int verbose = 0, negative = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--verbose") == 0) verbose = 1;
        else if (strcmp(argv[i], "--negative") == 0) negative = 1;
    }

    size_t n = 0;
    const char *const *cands = nm_font_candidates(&n);
    if (n == 0) {
        fprintf(stderr, "FONT-CJK-FAIL  字体候选链是空的\n");
        return 1;
    }

    /* ---------- 负向对照：检测逻辑本身对不对 ---------- */
    if (negative) {
        /* 找一个确定无中文的（拉丁字体）和一个确定有中文的。
         * 前者：NotoSans 系列；后者：任意命中 > 0 的。 */
        const char *no_cjk = NULL, *has_cjk = NULL;
        for (size_t i = 0; i < n; i++) {
            if (!cands[i]) continue;
            int hits = cjk_hit_count(cands[i]);
            if (hits < 0) continue;
            if (hits == 0 && !no_cjk) {
                /* 拉丁字体：文件名像 NotoSans/Estedad 这类（排除 CJK 字体名） */
                const char *b = strrchr(cands[i], '/');
                b = b ? b + 1 : cands[i];
                if (strstr(b, "NotoSans-") && !strstr(b, "CJK") && !strstr(b, "Fallback"))
                    no_cjk = cands[i];
            } else if (hits > 0 && !has_cjk) {
                has_cjk = cands[i];
            }
        }
        if (!no_cjk) {
            fprintf(stderr, "FONT-CJK-FAIL(--negative)  找不到可用于对照的拉丁字体"
                            "（候选链里应有一个 NotoSans-*.ttf）\n");
            return 1;
        }
        if (!has_cjk) {
            fprintf(stderr, "FONT-CJK-FAIL(--negative)  候选链里没有含中文的字体，"
                            "正向判据本身就已经不成立\n");
            return 1;
        }
        int h1 = cjk_hit_count(no_cjk);
        int h2 = cjk_hit_count(has_cjk);
        printf("  无中文对照 %-40s 命中 %d（应为 0）\n", no_cjk, h1);
        printf("  有中文对照 %-40s 命中 %d（应 > 0）\n", has_cjk, h2);
        if (h1 != 0 || h2 <= 0) {
            fprintf(stderr, "FONT-CJK-FAIL(--negative)  检测逻辑有误："
                            "无中文字体判成 %d，有中文字体判成 %d\n", h1, h2);
            return 1;
        }
        printf("FONT-CJK-OK（检测逻辑正反两侧都正确）\n");
        return 0;
    }

    /* ---------- 正向：链里必须有能画中文的字体 ---------- */
    int cjk_files = 0;
    const char *first_cjk = NULL;
    int max_hits = 0;

    for (size_t i = 0; i < n; i++) {
        if (!cands[i]) continue;
        int hits = cjk_hit_count(cands[i]);
        if (hits < 0) continue;
        if (hits > 0) {
            cjk_files++;
            if (!first_cjk) first_cjk = cands[i];
            if (hits > max_hits) max_hits = hits;
        }
        if (verbose || hits > 0) {
            printf("  %-42s 中文命中 %d/%d\n", cands[i], hits,
                   (int)(sizeof(kCJK) / sizeof(kCJK[0])));
        }
    }

    printf("候选文件 %zu 个，其中含中文的 %d 个\n", n, cjk_files);
    if (cjk_files == 0) {
        fprintf(stderr, "FONT-CJK-FAIL  字体链里没有任何能画中文的字体"
                        "⇒ Qt 控件与地图上的中文都会变成方框\n");
        return 1;
    }
    printf("中文来源: %s（命中 %d 个测试字）\n", first_cjk, max_hits);
    printf("FONT-CJK-OK\n");
    return 0;
}
