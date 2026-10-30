/* tutorial/09_search/main.c — 第 9 课 · 地名搜索（bigram 倒排索引）
 *
 * 累加内容：用 bigram 倒排索引替代 LIKE '%x%' 全表扫描。
 *   ① 把中文名切成"字 bigram"（按 codepoint，不是字节）；② 每个 bigram → 倒排 posting(place_id)；
 *   ③ 查询时取各 bigram posting 的交集；④ 再用【子串复核】过滤掉"bigram 全中但不连续"的误命中。
 *
 * 编译： gcc -std=c11 -O2 -o tut main.c && ./tut
 */
#include "../common.h"
#include <stdint.h>
#include <string.h>

#define W 360
#define H 280
static uint32_t buf[W * H];

/* —— 极简 UTF-8 解码：返回下一个 codepoint，*p 前移 —— */
static int utf8_next(const char **p) {
    const unsigned char *s = (const unsigned char *)*p;
    int c = *s, len, cp;
    if (c < 0x80)      { len = 1; cp = c; }
    else if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
    else { *p += 1; return 0xFFFD; }
    for (int i = 1; i < len; i++) cp = (cp << 6) | (s[i] & 0x3F);
    *p += len;
    return cp;
}

/* 地名库（模拟 places_auto.db 的小样本） */
static struct { const char *name; double lat, lon; } PLACES[] = {
    {"中关村",       39.9836, 116.3164},
    {"中关村南",     39.9780, 116.3170},
    {"中关村大街",   39.9850, 116.3100},
    {"人民大学",     39.9720, 116.3300},
    {"北京大学",     39.9920, 116.3050},
    {"北京站",       39.9020, 116.4270},
};
#define NP (int)(sizeof(PLACES)/sizeof(PLACES[0]))

/* 倒排：(bigram_codepoints, place_id) 列表 */
typedef struct { uint64_t g; int pid; } Post;
static Post posts[512]; static int np = 0;

/* 把一个名字的 bigram 全部插入倒排 */
static void index_name(int pid, const char *name) {
    const char *p = name; int cps[64], nc = 0;
    while (*p) { int cp = utf8_next(&p); if (cp == 0xFFFD) continue; cps[nc++] = cp; if (nc >= 64) break; }
    for (int i = 0; i + 1 < nc; i++) {
        uint64_t g = ((uint64_t)cps[i] << 32) | (uint64_t)cps[i + 1];
        posts[np++] = (Post){g, pid};
    }
}

/* codepoint 级子串匹配：hay 是否包含 needle 连续子序列 */
static int cps_substr(const int *hay, int nh, const int *nd, int nn) {
    if (nn == 0) return 1;
    for (int i = 0; i + nn <= nh; i++) {
        int ok = 1; for (int j = 0; j < nn; j++) if (hay[i + j] != nd[j]) { ok = 0; break; }
        if (ok) return 1;
    }
    return 0;
}

int main(void) {
    tut_cls(buf, W, H, 0xFFF4F1EA);
    for (int i = 0; i < NP; i++) index_name(i, PLACES[i].name);

    /* 查询 "中关村" */
    const char *q = "中关村";
    const char *p = q; int qcps[64], nq = 0;
    while (*p) { int cp = utf8_next(&p); if (cp == 0xFFFD) continue; qcps[nq++] = cp; if (nq >= 64) break; }

    /* ① 各 bigram 的 posting 取交集（用计数法） */
    int cnt[NP]; memset(cnt, 0, sizeof cnt);
    for (int b = 0; b + 1 < nq; b++) {
        uint64_t g = ((uint64_t)qcps[b] << 32) | (uint64_t)qcps[b + 1];
        for (int k = 0; k < np; k++) if (posts[k].g == g) cnt[posts[k].pid]++;
    }

    printf("查询 \"%s\" 结果：\n", q);
    for (int i = 0; i < NP; i++) {
        int bigram_hit = (cnt[i] == nq - 1);                 /* bigram 全中 */
        int sub_hit = 0;
        /* 取该地名 codepoint 做子串复核 */
        const char *np2 = PLACES[i].name; int hc[64], nh = 0;
        while (*np2) { int cp = utf8_next(&np2); if (cp == 0xFFFD) continue; hc[nh++] = cp; if (nh >= 64) break; }
        sub_hit = cps_substr(hc, nh, qcps, nq);
        if (bigram_hit && sub_hit)
            printf("  ★ %s  (bigram=%d/%d, 子串复核=命中)\n", PLACES[i].name, cnt[i], nq - 1);
        else if (bigram_hit && !sub_hit)
            printf("  ✗ %s  (bigram 全中但子串复核=否——丢弃误命中)\n", PLACES[i].name);
    }

    /* 画：所有地名标灰点，命中标红 */
    double lat0 = 39.9650, lat1 = 39.9950, lon0 = 116.3000, lon1 = 116.3350;
    for (int i = 0; i < NP; i++) {
        int x = (int)((PLACES[i].lon - lon0) / (lon1 - lon0) * (W - 1));
        int y = (int)((lat1 - PLACES[i].lat) / (lat1 - lat0) * (H - 1));
        int hit = 0; const char *np3 = PLACES[i].name; int hc[64], nh = 0;
        while (*np3) { int cp = utf8_next(&np3); if (cp == 0xFFFD) continue; hc[nh++] = cp; if (nh >= 64) break; }
        if (cnt[i] == nq - 1 && cps_substr(hc, nh, qcps, nq)) hit = 1;
        tut_fill_rect(buf, W, H, x - 3, y - 3, 7, 7, hit ? 0xFFB03A1F : 0xFF9AA0A6);
    }
    if (tut_write_bmp("out.bmp", buf, W, H) != 0) { fprintf(stderr, "write failed\n"); return 1; }
    printf("OK: wrote out.bmp — 红=命中地名，灰=未命中\n");
    return 0;
}
