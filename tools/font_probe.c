/* font_probe —— 诊断"中文变方框（tofu）"到底卡在哪一环。
 *
 * 现象：Qt/SDL 截图里中文全是方框。
 * 要把这件事拆成两问，分别用数据回答（别猜）：
 *   ① 字体集合里有没有 CJK 字体面？它的 scale 是多少？
 *      （scale<=0 ⇒ 这个面画不出任何东西）
 *   ② 某个汉字最终选了哪个面？glyph index 是不是 0？
 *      （=0 ⇒ 没有任何面含这个字形 ⇒ 字体文件不对）
 *      glyph index 非 0 但位图非零像素为 0 ⇒ 有字形但光栅化出空图 ⇒ scale 问题
 *
 * 用法: font_probe [码点...]   不带码点则测一串常用汉字
 */
#include "render/font.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* UTF-8 解码一个码点（够用即可：支持 1~4 字节） */
static unsigned utf8_next(const char *s, int *len)
{
    unsigned char c = (unsigned char)s[0];
    unsigned cp;
    int n;
    if (c < 0x80)       { cp = c;         n = 1; }
    else if (c < 0xE0)  { cp = c & 0x1F;  n = 2; }
    else if (c < 0xF0)  { cp = c & 0x0F;  n = 3; }
    else                { cp = c & 0x07;  n = 4; }
    for (int i = 1; i < n; i++)
        cp = (cp << 6) | ((unsigned char)s[i] & 0x3F);
    *len = n;
    return cp;
}

/* 把码点打回 UTF-8（只为输出可读） */
static void cp_to_utf8(unsigned cp, char *out, int cap)
{
    if (cp < 0x80) { snprintf(out, cap, "%c", (char)cp); return; }
    if (cp < 0x800) {
        snprintf(out, cap, "%c%c", (char)(0xC0 | (cp >> 6)),
                 (char)(0x80 | (cp & 0x3F)));
        return;
    }
    if (cp < 0x10000) {
        snprintf(out, cap, "%c%c%c", (char)(0xE0 | (cp >> 12)),
                 (char)(0x80 | ((cp >> 6) & 0x3F)), (char)(0x80 | (cp & 0x3F)));
        return;
    }
    snprintf(out, cap, "%c%c%c%c", (char)(0xF0 | (cp >> 18)),
             (char)(0x80 | ((cp >> 12) & 0x3F)), (char)(0x80 | ((cp >> 6) & 0x3F)),
             (char)(0x80 | (cp & 0x3F)));
}

int main(void)
{
    /* 测试用字符串：界面真实用到的中文 + 拉丁 + 标点 */
    static const char *kTests[] = {
        "未加载地图数据", "搜索地点、道路", "导航中", "重新规划中",
        "距离", "公里", "米", "分钟", "路口", "转向",
        "NavMap", "0123456789", "?", ":", " ",
    };

    Font *f = font_open_default(14);
    if (!f) { fprintf(stderr, "font_open_default 失败\n"); return 2; }

    int nf = font_probe_face_count(f);
    printf("字体集合：%d 个面\n", nf);
    for (int i = 0; i < nf; i++) {
        const char *name = NULL;
        float scale = 0;
        if (!font_probe_face(f, i, &name, &scale)) continue;
        printf("  面[%2d] scale=%8.4f  %s%s\n", i, scale,
               name ? name : "(未知)",
               scale <= 0 ? "   ★ scale<=0，这个面画不出东西" : "");
    }

    printf("\n逐字检查：\n");
    for (size_t t = 0; t < sizeof(kTests) / sizeof(kTests[0]); t++) {
        const char *s = kTests[t];
        printf("  \"%s\"\n", s);
        int len = 0;
        while (*s) {
            unsigned cp = utf8_next(s, &len);
            s += len;
            int face = -1, gi = -1, gw = 0, gh = 0, nz = 0;
            if (!font_probe_glyph(f, cp, &face, &gi, &gw, &gh, &nz)) continue;
            char ub[8];
            cp_to_utf8(cp, ub, sizeof(ub));
            const char *fname = NULL;
            float scale = 0;
            font_probe_face(f, face, &fname, &scale);
            const char *verdict;
            if (gi == 0)      verdict = "★ 无字形（所有面都没有）";
            else if (nz == 0)  verdict = "★ 有字形但位图全空（scale 问题）";
            else               verdict = "OK";
            printf("      U+%04X %-4s 面[%2d] gi=%-5d %dx%d nz=%-5d scale=%.3f  %s  %s\n",
                   cp, ub, face, gi, gw, gh, nz, scale, fname ? fname : "", verdict);
        }
    }
    font_close(f);
    return 0;
}
