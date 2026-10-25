#include "map/gpx.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>
#include "utils/plat.h"

/* ================================================================== */
/* store 管理                                                          */
/* ================================================================== */

/* 轨迹调色板：与底图/路线都不糊。
 * 路线是蓝 (30,110,235)、高速橙红 (214,94,40)、水蓝、绿地绿 ——
 * 所以轨迹取品红/青/紫/草绿这套"高饱和、非常用色"。 */
static const struct { uint8_t r, g, b; } kPalette[] = {
    { 232,  62, 158 },   /* 品红 */
    {   0, 178, 178 },   /* 青   */
    { 150,  92, 220 },   /* 紫   */
    { 110, 190,  60 },   /* 草绿 */
};
#define GPX_PALETTE_N ((int)(sizeof(kPalette) / sizeof(kPalette[0])))

void gpx_store_init(GpxStore *s)
{
    memset(s, 0, sizeof(*s));
}

void gpx_store_free(GpxStore *s)
{
    if (!s) return;
    for (int i = 0; i < s->n_trk; i++) free(s->trk[i].lat_e7), free(s->trk[i].lon_e7);
    free(s->trk);
    free(s->wpt);
    memset(s, 0, sizeof(*s));
}

int gpx_store_add_track(GpxStore *s, const char *name)
{
    if (s->n_trk >= s->cap_trk) {
        int nc = s->cap_trk ? s->cap_trk * 2 : 4;
        GpxTrack *nt = realloc(s->trk, (size_t)nc * sizeof(*nt));
        if (!nt) return -1;
        s->trk = nt; s->cap_trk = nc;
    }
    GpxTrack *t = &s->trk[s->n_trk];
    memset(t, 0, sizeof(*t));
    if (name && *name) {
        size_t n = strlen(name);
        if (n >= GPX_NAME_MAX) n = GPX_NAME_MAX - 1;
        memcpy(t->name, name, n); t->name[n] = '\0';
    }
    int k = s->n_trk % GPX_PALETTE_N;
    t->r = kPalette[k].r; t->g = kPalette[k].g; t->b = kPalette[k].b;
    return s->n_trk++;
}

int gpx_track_push(GpxTrack *t, double lat, double lon)
{
    if (!t) return -1;
    if (t->n >= t->cap) {
        uint32_t nc = t->cap ? t->cap * 2 : 256;
        int32_t *la = realloc(t->lat_e7, (size_t)nc * sizeof(*la));
        if (!la) return -1;
        t->lat_e7 = la;
        int32_t *lo = realloc(t->lon_e7, (size_t)nc * sizeof(*lo));
        if (!lo) return -1;
        t->lon_e7 = lo;
        t->cap = nc;
    }
    t->lat_e7[t->n] = (int32_t)llround(lat * 1e7);
    t->lon_e7[t->n] = (int32_t)llround(lon * 1e7);
    t->n++;
    return 0;
}

int gpx_store_add_wpt(GpxStore *s, const char *name, double lat, double lon)
{
    if (s->n_wpt >= s->cap_wpt) {
        int nc = s->cap_wpt ? s->cap_wpt * 2 : 8;
        GpxWpt *nw = realloc(s->wpt, (size_t)nc * sizeof(*nw));
        if (!nw) return -1;
        s->wpt = nw; s->cap_wpt = nc;
    }
    GpxWpt *w = &s->wpt[s->n_wpt];
    memset(w, 0, sizeof(*w));
    if (name && *name) {
        size_t n = strlen(name);
        if (n >= GPX_NAME_MAX) n = GPX_NAME_MAX - 1;
        memcpy(w->name, name, n); w->name[n] = '\0';
    }
    w->lat = lat; w->lon = lon;
    return s->n_wpt++;
}

void gpx_store_del_track(GpxStore *s, int i)
{
    if (!s || i < 0 || i >= s->n_trk) return;
    free(s->trk[i].lat_e7); free(s->trk[i].lon_e7);
    memmove(&s->trk[i], &s->trk[i + 1],
            (size_t)(s->n_trk - i - 1) * sizeof(*s->trk));
    s->n_trk--;
}

void gpx_store_del_wpt(GpxStore *s, int i)
{
    if (!s || i < 0 || i >= s->n_wpt) return;
    memmove(&s->wpt[i], &s->wpt[i + 1],
            (size_t)(s->n_wpt - i - 1) * sizeof(*s->wpt));
    s->n_wpt--;
}

/* ================================================================== */
/* XML 解析（手写扫描，不引 expat：GPX 子集结构简单，正则化即可）          */
/* ================================================================== */

/* 反转义 &amp; &lt; &gt; &quot; &apos; &#nn; &#xHH;（GPX name 里常见 &amp;）。
 * 就地改写，返回改写后长度。 */
static size_t xml_unescape(char *s, size_t n)
{
    size_t w = 0;
    for (size_t r = 0; r < n; ) {
        if (s[r] == '&') {
            if (!strncmp(s + r, "&amp;", 5))  { s[w++] = '&'; r += 5; continue; }
            if (!strncmp(s + r, "&lt;", 4))   { s[w++] = '<'; r += 4; continue; }
            if (!strncmp(s + r, "&gt;", 4))   { s[w++] = '>'; r += 4; continue; }
            if (!strncmp(s + r, "&quot;", 6)) { s[w++] = '"'; r += 6; continue; }
            if (!strncmp(s + r, "&apos;", 6)) { s[w++] = '\''; r += 6; continue; }
            if (s[r + 1] == '#' && r + 2 < n) {
                char *endp = NULL;
                long cp = strtol(s + r + 2, &endp, (s[r + 2] == 'x' || s[r + 2] == 'X') ? 16 : 10);
                if (endp && *endp == ';' && cp > 0 && cp < 0x110000) {
                    /* 只处理 ASCII 范围（GPX 名字里的 &#nn; 基本是引号/连字符） */
                    if (cp < 128) s[w++] = (char)cp;
                    r = (size_t)(endp - s) + 1;
                    continue;
                }
            }
        }
        s[w++] = s[r++];
    }
    s[w] = '\0';
    return w;
}

/* 从 tag 文本起点（刚过 '<'）找属性 attr="value"，把 value（反转义后）拷到 out。
 * 返回 1 找到，0 没有。attr 必须带 '=' 前的完整名字（调用方传 "lat="）。 */
static int tag_attr(const char *p, const char *end, const char *attr,
                    char *out, size_t cap)
{
    size_t al = strlen(attr);
    while (p < end) {
        if (*p == '>') return 0;              /* 标签结束还没找到 */
        if (*p == attr[0] && (size_t)(end - p) > al && !strncmp(p, attr, al)) {
            p += al;                          /* 跳过 lat= */
            if (p >= end || *p != '"') return 0;
            p++;
            size_t w = 0;
            while (p < end && *p != '"') {
                if (w + 1 < cap) out[w++] = *p;
                p++;
            }
            out[w] = '\0';
            xml_unescape(out, w);
            return 1;
        }
        p++;
    }
    return 0;
}

/* 读元素文本 <name>..</name>：p 指向 '<'（name 标签起点）。
 * 拷文本到 out 并反转义；跳过 p 到文本结束。返回 1。 */
static int elem_text(char **pp, const char *end, const char *elem,
                     char *out, size_t cap)
{
    char *p = *pp;
    size_t el = strlen(elem);
    /* 确认是 <elem[ >/]（边界内） */
    if ((size_t)(end - p) < el + 2 || strncmp(p + 1, elem, el)) return 0;
    char after = p[1 + el];
    if (after != '>' && after != ' ' && after != '/' && after != '\t' &&
        after != '\n' && after != '\r') return 0;
    /* 找 '>' */
    while (p < end && *p != '>') p++;
    if (p >= end) return 0;
    p++;
    /* 读文本到 </elem */
    size_t w = 0;
    while (p < end) {
        /* CDATA 段：剥掉 <![CDATA[ 与 ]]> 包装，内容当普通字符数据
         * （XML 规范语义；常见于 <name><![CDATA[带 < > 的名字]]></name>） */
        if ((size_t)(end - p) >= 9 && !strncmp(p, "<![CDATA[", 9)) { p += 9; continue; }
        if ((size_t)(end - p) >= 3 && !strncmp(p, "]]>", 3))         { p += 3; continue; }
        if (*p == '<' && (size_t)(end - p) >= el + 3 &&
            p[1] == '/' && !strncmp(p + 2, elem, el)) {
            p += el + 2;
            while (p < end && *p != '>') p++;
            if (p < end) p++;                 /* 过 '>' */
            break;
        }
        if (w + 1 < cap) out[w++] = *p;
        p++;
    }
    out[w] = '\0';
    xml_unescape(out, w);
    *pp = p;
    return 1;
}

/* 平台无关的"在 buf[0..n) 里找 needle"：MSVC 没有 memmem，自己写。
 * 前向声明：skip_special 在下面用到，而它自己又调用本函数。 */
static char *memmem_local(const char *buf, size_t n, const char *needle, size_t nl);

static char *memmem_local(const char *buf, size_t n, const char *needle, size_t nl)
{
    if (nl == 0 || n < nl) return NULL;
    for (size_t i = 0; i + nl <= n; i++)
        if (!memcmp(buf + i, needle, nl)) return (char *)(buf + i);
    return NULL;
}

/* 跳过注释 / CDATA / 处理指令（<!DOCTYPE …> / <?xml …?>）。返回跳过后的位置。 */
static char *skip_special(char *p, const char *end)
{
    if ((size_t)(end - p) >= 4 && !strncmp(p, "<!--", 4)) {
        char *q = memmem_local(p, end - p, "-->", 3);
        return q ? q + 3 : (char *)end;
    }
    if ((size_t)(end - p) >= 9 && !strncmp(p, "<![CDATA[", 9)) {
        char *q = memmem_local(p, end - p, "]]>", 3);
        return q ? q + 3 : (char *)end;
    }
    if ((size_t)(end - p) >= 2 && p[0] == '<' && (p[1] == '?' || p[1] == '!')) {
        char *q = p;
        while (q < end && *q != '>') q++;
        return q < end ? q + 1 : (char *)end;
    }
    return p;
}

int gpx_parse(const char *xml, size_t len, GpxStore *out)
{
    if (!xml || !out) return -1;
    char *p = (char *)xml;
    char *end = (char *)xml + len;
    int n_trk0 = out->n_trk;

    /* 跳过 UTF-8 BOM */
    if (len >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB &&
        (unsigned char)p[2] == 0xBF) p += 3;

    while (p < end) {
        if (*p != '<') { p++; continue; }
        char *q = skip_special(p, end);
        if (q != p) { p = q; continue; }

        if ((size_t)(end - p) >= 4 && !strncmp(p, "<trk", 4)) {
            int ti = gpx_store_add_track(out, "");
            if (ti < 0) return -1;
            GpxTrack *t = &out->trk[ti];
            int got_name = 0;
            p += 4;
            while (p < end) {
                if ((size_t)(end - p) >= 6 && !strncmp(p, "</trk>", 6)) { p += 6; break; }
                if (*p == '<' && p[1] == '/' ) {           /* 其它闭合标签整体跳过 */
                    char *q2 = p; while (q2 < end && *q2 != '>') q2++;
                    p = (q2 < end) ? q2 + 1 : end; continue;
                }
                if (!got_name && *p == '<' && p[1] == 'n') {
                    char *q2 = p;
                    if (elem_text(&q2, end, "name", t->name, GPX_NAME_MAX)) {
                        got_name = 1; p = q2; continue;
                    }
                }
                if (*p == '<' && !strncmp(p + 1, "trkpt", 5)) {
                    char la[32], lo[32];
                    if (tag_attr(p + 6, end, "lat=", la, sizeof(la)) &&
                        tag_attr(p + 6, end, "lon=", lo, sizeof(lo))) {
                        double vla = strtod(la, NULL), vlo = strtod(lo, NULL);
                        if (vla >= -90.0 && vla <= 90.0 && vlo >= -180.0 && vlo <= 180.0)
                            gpx_track_push(t, vla, vlo);
                    }
                    char *q2 = p; while (q2 < end && *q2 != '>') q2++;
                    p = (q2 < end) ? q2 + 1 : end; continue;
                }
                p++;
            }
            /* 空轨迹（一个点都没解析到，通常是文件被截断）⇒ 回退，
             * 不留在 store 里污染导入结果。 */
            if (t->n == 0) gpx_store_del_track(out, ti);
            continue;
        }

        if ((size_t)(end - p) >= 4 && !strncmp(p, "<rte", 4)) {
            /* rte 归一成轨迹：名字前缀 "(路线) " 以示区别 */
            char nm[GPX_NAME_MAX] = "";
            int ti = gpx_store_add_track(out, "(路线)");
            if (ti < 0) return -1;
            GpxTrack *t = &out->trk[ti];
            int got_name = 0;
            p += 4;
            while (p < end) {
                if ((size_t)(end - p) >= 6 && !strncmp(p, "</rte>", 6)) { p += 6; break; }
                if (*p == '<' && p[1] == '/') {
                    char *q2 = p; while (q2 < end && *q2 != '>') q2++;
                    p = (q2 < end) ? q2 + 1 : end; continue;
                }
                if (!got_name && *p == '<' && p[1] == 'n') {
                    char *q2 = p;
                    if (elem_text(&q2, end, "name", nm, sizeof(nm))) {
                        got_name = 1;
                        if (nm[0]) snprintf(t->name, GPX_NAME_MAX, "(路线)%s", nm);
                        p = q2; continue;
                    }
                }
                if (*p == '<' && !strncmp(p + 1, "rtept", 5)) {
                    char la[32], lo[32];
                    if (tag_attr(p + 6, end, "lat=", la, sizeof(la)) &&
                        tag_attr(p + 6, end, "lon=", lo, sizeof(lo))) {
                        double vla = strtod(la, NULL), vlo = strtod(lo, NULL);
                        if (vla >= -90.0 && vla <= 90.0 && vlo >= -180.0 && vlo <= 180.0)
                            gpx_track_push(t, vla, vlo);
                    }
                    char *q2 = p; while (q2 < end && *q2 != '>') q2++;
                    p = (q2 < end) ? q2 + 1 : end; continue;
                }
                p++;
            }
            /* 同上：空路线不回退（见 trk 分支的说明）。 */
            if (t->n == 0) gpx_store_del_track(out, ti);
            continue;
        }

        if ((size_t)(end - p) >= 4 && !strncmp(p, "<wpt", 4)) {
            char la[32], lo[32], nm[GPX_NAME_MAX] = "";
            char *q2 = p + 4;
            int has = tag_attr(q2, end, "lat=", la, sizeof(la)) &&
                      tag_attr(q2, end, "lon=", lo, sizeof(lo));
            /* 找 </wpt> 之前（或 /> 自闭合）的 <name> */
            while (q2 < end && !(q2[0] == '<' && q2[1] == '/' && !strncmp(q2 + 2, "wpt", 3))) {
                if (q2[0] == '<' && q2[1] == 'n') {
                    char *q3 = q2;
                    if (elem_text(&q3, end, "name", nm, sizeof(nm))) { q2 = q3; continue; }
                }
                q2++;
            }
            if (has) {
                double vla = strtod(la, NULL), vlo = strtod(lo, NULL);
                if (vla >= -90.0 && vla <= 90.0 && vlo >= -180.0 && vlo <= 180.0)
                    gpx_store_add_wpt(out, nm, vla, vlo);
            }
            while (q2 < end && *q2 != '>') q2++;
            p = (q2 < end) ? q2 + 1 : end;
            continue;
        }

        p++;
    }

    if (out->n_trk == n_trk0 && out->n_wpt == 0) return -2;
    return out->n_trk - n_trk0;
}

int gpx_parse_file(const char *path, GpxStore *out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz <= 0 || sz > 256L * 1024 * 1024) { fclose(f); return sz == 0 ? -2 : -1; }
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return -1; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) { free(buf); return -1; }
    buf[sz] = '\0';
    int rc = gpx_parse(buf, (size_t)sz, out);
    free(buf);
    return rc;
}

/* ================================================================== */
/* 写出                                                                */
/* ================================================================== */

static void xml_escape_to(FILE *f, const char *s)
{
    for (; *s; s++) {
        switch (*s) {
        case '&':  fputs("&amp;",  f); break;
        case '<':  fputs("&lt;",   f); break;
        case '>':  fputs("&gt;",   f); break;
        case '"':  fputs("&quot;", f); break;
        case '\'': fputs("&apos;", f); break;
        default:   fputc((unsigned char)*s, f); break;
        }
    }
}

int gpx_write(const GpxStore *s, const char *path)
{
    if (!s || !path) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fputs("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n", f);
    fputs("<gpx version=\"1.1\" creator=\"navimap\" "
          "xmlns=\"http://www.topografix.com/GPX/1/1\">\n", f);
    for (int i = 0; i < s->n_wpt; i++) {
        const GpxWpt *w = &s->wpt[i];
        fprintf(f, "  <wpt lat=\"%.7f\" lon=\"%.7f\"><name>", w->lat, w->lon);
        xml_escape_to(f, w->name[0] ? w->name : "(未命名)");
        fputs("</name></wpt>\n", f);
    }
    for (int i = 0; i < s->n_trk; i++) {
        const GpxTrack *t = &s->trk[i];
        fprintf(f, "  <trk><name>");
        xml_escape_to(f, t->name[0] ? t->name : "(未命名)");
        fputs("</name><trkseg>\n", f);
        for (uint32_t k = 0; k < t->n; k++)
            fprintf(f, "    <trkpt lat=\"%.7f\" lon=\"%.7f\"/>\n",
                    t->lat_e7[k] / 1e7, t->lon_e7[k] / 1e7);
        fputs("  </trkseg></trk>\n", f);
    }
    fputs("</gpx>\n", f);
    if (fclose(f) != 0) return -1;
    return 0;
}

/* ================================================================== */
/* sqlite 持久化                                                        */
/* ================================================================== */

static const char *kSchema =
    "CREATE TABLE IF NOT EXISTS track("
    " id INTEGER PRIMARY KEY, name TEXT, r INT, g INT, b INT);"
    "CREATE TABLE IF NOT EXISTS tpoint("
    " tid INT, seq INT, lat REAL, lon REAL);"
    "CREATE TABLE IF NOT EXISTS fav("
    " id INTEGER PRIMARY KEY, name TEXT, lat REAL, lon REAL);"
    "CREATE INDEX IF NOT EXISTS tpoint_tid ON tpoint(tid, seq);";

void *gpx_db_open(const char *path)
{
    if (!path || !*path) return NULL;
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return NULL;
    }
    char *err = NULL;
    if (sqlite3_exec(db, kSchema, NULL, NULL, &err) != SQLITE_OK) {
        sqlite3_free(err);
        sqlite3_close(db);
        return NULL;
    }
    return db;
}

void gpx_db_close(void *db)
{
    if (db) sqlite3_close((sqlite3 *)db);
}

int gpx_db_default_path(char *out, size_t cap)
{
    if (!out || cap == 0) return -1;
    const char *dir = nm_config_dir();
    if (!dir || !*dir) return -1;
    /* ★ 配置目录可能还不存在（全新账户 / 刚清过 %APPDATA%）。sqlite3_open
     *   **不会建父目录**，失败只返回 NULL ⇒ 导入照常报"1 条轨迹"（内存侧
     *   成功了），但一条也不落库，且没有任何提示 —— 重启后收藏全丢。
     *   在这儿递归建好再拼路径；建失败（只读/权限）才真的返回 -1。 */
    if (!nm_is_dir(dir) && nm_mkdir_p(dir) != 0) return -1;
    if (nm_path_join(out, cap, dir, "tracks.db") != 0) return -1;
    return 0;
}

int gpx_db_save_track(void *db, const GpxTrack *t)
{
    if (!db || !t) return -1;
    sqlite3 *d = (sqlite3 *)db;
    char *err = NULL;
    if (sqlite3_exec(d, "BEGIN IMMEDIATE", NULL, NULL, &err) != SQLITE_OK) {
        sqlite3_free(err); return -1;
    }
    sqlite3_stmt *st = NULL;
    /* 同名先删（重复导入不翻倍）。★ tpoint 子表必须**跟着删**：track 删掉后
       它的 rowid 会被新 INSERT 复用（INTEGER PRIMARY KEY 取 max+1，删了最大
       的那条就正好复用同一个 id），不同时删 tpoint 的话新轨迹会"继承"旧轨迹的
       点 —— 实测表现为库里同一条轨迹的点数逐次导入不断累积（2→14→16→18）。 */
    if (sqlite3_prepare_v2(d,
            "DELETE FROM tpoint WHERE tid IN"
            " (SELECT id FROM track WHERE name=?1)",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, t->name, -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
    st = NULL;
    if (sqlite3_prepare_v2(d, "DELETE FROM track WHERE name=?1", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, t->name, -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
    st = NULL;
    if (sqlite3_prepare_v2(d, "INSERT INTO track(name,r,g,b) VALUES(?1,?2,?3,?4)",
                           -1, &st, NULL) != SQLITE_OK) { sqlite3_exec(d, "ROLLBACK", 0, 0, 0); return -1; }
    sqlite3_bind_text(st, 1, t->name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 2, t->r);
    sqlite3_bind_int(st, 3, t->g);
    sqlite3_bind_int(st, 4, t->b);
    if (sqlite3_step(st) != SQLITE_DONE) {
        sqlite3_finalize(st); sqlite3_exec(d, "ROLLBACK", 0, 0, 0); return -1;
    }
    sqlite3_finalize(st);
    sqlite3_int64 tid = sqlite3_last_insert_rowid(d);

    if (sqlite3_prepare_v2(d, "INSERT INTO tpoint(tid,seq,lat,lon) VALUES(?1,?2,?3,?4)",
                           -1, &st, NULL) == SQLITE_OK) {
        for (uint32_t k = 0; k < t->n; k++) {
            sqlite3_reset(st);
            sqlite3_bind_int64(st, 1, tid);
            sqlite3_bind_int(st, 2, (int)k);
            sqlite3_bind_double(st, 3, t->lat_e7[k] / 1e7);
            sqlite3_bind_double(st, 4, t->lon_e7[k] / 1e7);
            if (sqlite3_step(st) != SQLITE_DONE) break;
        }
    }
    sqlite3_finalize(st);
    if (sqlite3_exec(d, "COMMIT", NULL, NULL, &err) != SQLITE_OK) {
        sqlite3_free(err); sqlite3_exec(d, "ROLLBACK", 0, 0, 0); return -1;
    }
    return (int)tid;
}

int gpx_db_load_all(void *db, GpxStore *s)
{
    if (!s) return -1;
    gpx_store_free(s);
    gpx_store_init(s);
    if (!db) return 0;
    sqlite3 *d = (sqlite3 *)db;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(d, "SELECT id,name,r,g,b FROM track ORDER BY id",
                           -1, &st, NULL) != SQLITE_OK) return -1;
    while (sqlite3_step(st) == SQLITE_ROW) {
        int ti = gpx_store_add_track(s, (const char *)sqlite3_column_text(st, 1));
        if (ti < 0) break;
        GpxTrack *t = &s->trk[ti];
        t->db_id = sqlite3_column_int64(st, 0);
        t->r = (uint8_t)sqlite3_column_int(st, 2);
        t->g = (uint8_t)sqlite3_column_int(st, 3);
        t->b = (uint8_t)sqlite3_column_int(st, 4);
        sqlite3_stmt *ps = NULL;
        if (sqlite3_prepare_v2(d, "SELECT lat,lon FROM tpoint WHERE tid=?1 ORDER BY seq",
                               -1, &ps, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(ps, 1, t->db_id);
            while (sqlite3_step(ps) == SQLITE_ROW)
                gpx_track_push(t, sqlite3_column_double(ps, 0), sqlite3_column_double(ps, 1));
        }
        sqlite3_finalize(ps);
    }
    sqlite3_finalize(st);

    st = NULL;
    if (sqlite3_prepare_v2(d, "SELECT id,name,lat,lon FROM fav ORDER BY id",
                           -1, &st, NULL) != SQLITE_OK) return -1;
    while (sqlite3_step(st) == SQLITE_ROW) {
        gpx_store_add_wpt(s, (const char *)sqlite3_column_text(st, 1),
                          sqlite3_column_double(st, 2), sqlite3_column_double(st, 3));
        s->wpt[s->n_wpt - 1].db_id = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    return 0;
}

int gpx_db_add_fav(void *db, const char *name, double lat, double lon)
{
    if (!db) return -1;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2((sqlite3 *)db, "INSERT INTO fav(name,lat,lon) VALUES(?1,?2,?3)",
                           -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(st, 1, name && *name ? name : "(未命名)", -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(st, 2, lat);
    sqlite3_bind_double(st, 3, lon);
    int rc = sqlite3_step(st) == SQLITE_DONE ? (int)sqlite3_last_insert_rowid((sqlite3 *)db) : -1;
    sqlite3_finalize(st);
    return rc;
}

int gpx_db_find_fav(void *db, const char *name, double lat, double lon)
{
    if (!db) return 0;
    sqlite3_stmt *st = NULL;
    /* 坐标比到 1e-7 度（≈1 cm）：浮点相等太脆，往返一次 double 就未必逐位相同 */
    if (sqlite3_prepare_v2((sqlite3 *)db,
            "SELECT id FROM fav WHERE name=?1 AND abs(lat-?2)<1e-7 AND abs(lon-?3)<1e-7"
            " ORDER BY id LIMIT 1", -1, &st, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_text(st, 1, name && *name ? name : "(未命名)", -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(st, 2, lat);
    sqlite3_bind_double(st, 3, lon);
    int id = 0;
    if (sqlite3_step(st) == SQLITE_ROW) id = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return id;
}

void gpx_db_del_fav(void *db, int id)
{
    if (!db || id <= 0) return;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2((sqlite3 *)db, "DELETE FROM fav WHERE id=?1", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, id);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
}

void gpx_db_del_track(void *db, int id)
{
    if (!db || id <= 0) return;
    sqlite3 *d = (sqlite3 *)db;
    sqlite3_exec(d, "BEGIN IMMEDIATE", NULL, NULL, NULL);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(d, "DELETE FROM tpoint WHERE tid=?1", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, id);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
    st = NULL;
    if (sqlite3_prepare_v2(d, "DELETE FROM track WHERE id=?1", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, id);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
    sqlite3_exec(d, "COMMIT", NULL, NULL, NULL);
}
