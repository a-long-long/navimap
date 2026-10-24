#include "settings/settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils/plat.h"

/* ---- 可编辑设置清单（面板按此渲染；顺序即显示顺序） ---- */
const SettingMeta SETTINGS_SCHEMA[] = {
    { "day_night",   "昼夜模式",   ST_BOOL, 0, 0, 1, 1, NULL },
    { "show_panel",  "导航面板",   ST_BOOL, 1, 0, 1, 1, NULL },
    { "show_poi",    "POI 图标",   ST_BOOL, 1, 0, 1, 1, NULL },
    { "show_labels", "地名标注",   ST_BOOL, 1, 0, 1, 1, NULL },
    { "mode",        "默认页签",   ST_INT,  0, 0, 1, 1, "浏览\0导航\0" },
    { "font_size",   "字号(pt)",   ST_INT, 14, 8, 32, 1, NULL },
    { "max_class",   "道路细节",   ST_INT,  4, 0, 4, 1, NULL },
};
const int SETTINGS_SCHEMA_N = (int)(sizeof(SETTINGS_SCHEMA) / sizeof(SETTINGS_SCHEMA[0]));

/* ---- 内部：key=value 条目表 ---- */
typedef struct {
    char *key;
    char *val;
} Entry;

struct Settings {
    Entry *e;
    int    n, cap;
    int    dirty;
    char   path[1024];
};

static const SettingMeta *schema_of(const char *key)
{
    for (int i = 0; i < SETTINGS_SCHEMA_N; i++)
        if (strcmp(SETTINGS_SCHEMA[i].key, key) == 0) return &SETTINGS_SCHEMA[i];
    return NULL;
}

static int find_idx(const Settings *s, const char *key)
{
    for (int i = 0; i < s->n; i++)
        if (strcmp(s->e[i].key, key) == 0) return i;
    return -1;
}

static int ensure_cap(Settings *s)
{
    if (s->n < s->cap) return 0;
    int nc = s->cap ? s->cap * 2 : 16;
    Entry *ne = (Entry *)realloc(s->e, (size_t)nc * sizeof(Entry));
    if (!ne) return -1;
    s->e = ne; s->cap = nc;
    return 0;
}

static void set_raw(Settings *s, const char *key, const char *v)
{
    int i = find_idx(s, key);
    if (i >= 0) {
        char *nv = nm_strdup(v);
        if (!nv) return;
        free(s->e[i].val);
        s->e[i].val = nv;
        s->dirty = 1;
        return;
    }
    if (ensure_cap(s) != 0) return;
    s->e[s->n].key = nm_strdup(key);
    s->e[s->n].val = nm_strdup(v);
    if (!s->e[s->n].key || !s->e[s->n].val) { /* 部分失败：回收已分配部分 */
        free(s->e[s->n].key); s->e[s->n].key = NULL;
        free(s->e[s->n].val);  s->e[s->n].val  = NULL;
        return;
    }
    s->n++;
    s->dirty = 1;
}

Settings *settings_create(void)
{
    Settings *s = (Settings *)calloc(1, sizeof(Settings));
    return s; /* NULL 仅在 OOM，调用方按 NULL 处理 */
}

void settings_destroy(Settings *s)
{
    if (!s) return;
    for (int i = 0; i < s->n; i++) { free(s->e[i].key); free(s->e[i].val); }
    free(s->e);
    free(s);
}

static void ensure_path(Settings *s)
{
    if (s->path[0]) return;
    const char *env = getenv("NAVMAP_SETTINGS");
    if (env && *env) {
        snprintf(s->path, sizeof(s->path), "%s", env);
        return;
    }
    const char *d = nm_config_dir();
    snprintf(s->path, sizeof(s->path), "%s/settings.ini", d);
}

int settings_load(Settings *s, const char *path)
{
    if (!s) return -1;
    if (path) { snprintf(s->path, sizeof(s->path), "%s", path); }
    else      ensure_path(s);

    FILE *f = fopen(s->path, "rb");
    if (!f) return 0;                 /* 没有文件：保持默认值，不致命 */

    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        /* 去尾换行 */
        size_t len = strlen(line);
        while (len && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';
        /* 跳过前导空白与注释/空行 */
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '#') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = p;
        char *v = eq + 1;
        /* 去 key 尾空白 */
        char *ke = k + strlen(k);
        while (ke > k && (ke[-1] == ' ' || ke[-1] == '\t')) *--ke = '\0';
        if (*k == '\0') continue;
        /* 去 value 首空白 */
        while (*v == ' ' || *v == '\t') v++;
        set_raw(s, k, v);
    }
    fclose(f);
    s->dirty = 0;
    return 0;
}

int settings_save(Settings *s)
{
    if (!s) return -1;
    if (!s->path[0]) ensure_path(s);

    /* 确保目录存在：取路径最后一个 '/' 之前的部分 */
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", s->path);
    char *sl = strrchr(dir, '/');
    if (!sl) sl = strrchr(dir, '\\');
    if (sl) { *sl = '\0'; if (nm_mkdir_p(dir) != 0) return -1; }

    FILE *f = fopen(s->path, "wb");
    if (!f) return -1;
    for (int i = 0; i < s->n; i++)
        fprintf(f, "%s=%s\n", s->e[i].key, s->e[i].val);
    fclose(f);
    s->dirty = 0;
    return 0;
}

int settings_save_as(Settings *s, const char *path)
{
    if (!s || !path) return -1;
    snprintf(s->path, sizeof(s->path), "%s", path);
    return settings_save(s);
}

const char *settings_path(const Settings *s)
{
    return s ? s->path : NULL;
}

int settings_has(const Settings *s, const char *key)
{
    return s && find_idx(s, key) >= 0;
}

int settings_get_int(const Settings *s, const char *key, int def)
{
    int i = s ? find_idx(s, key) : -1;
    if (i < 0) return def;
    return (int)strtol(s->e[i].val, NULL, 10);
}

double settings_get_dbl(const Settings *s, const char *key, double def)
{
    int i = s ? find_idx(s, key) : -1;
    if (i < 0) return def;
    return strtod(s->e[i].val, NULL);
}

int settings_get_bool(const Settings *s, const char *key, int def)
{
    int i = s ? find_idx(s, key) : -1;
    if (i < 0) return def;
    const char *v = s->e[i].val;
    if (strcmp(v, "1") == 0 || strcmp(v, "true") == 0 || strcmp(v, "yes") == 0)
        return 1;
    if (strcmp(v, "0") == 0 || strcmp(v, "false") == 0 || strcmp(v, "no") == 0)
        return 0;
    return strtol(v, NULL, 10) != 0;     /* 其它非零当真 */
}

const char *settings_get_str(const Settings *s, const char *key, const char *def)
{
    int i = s ? find_idx(s, key) : -1;
    if (i < 0) return def;
    return s->e[i].val;
}

void settings_set_int(Settings *s, const char *key, int v)
{
    if (!s) return;
    char b[32]; snprintf(b, sizeof(b), "%d", v);
    set_raw(s, key, b);
}

void settings_set_dbl(Settings *s, const char *key, double v)
{
    if (!s) return;
    char b[64]; snprintf(b, sizeof(b), "%.9g", v);
    set_raw(s, key, b);
}

void settings_set_bool(Settings *s, const char *key, int v)
{
    if (!s) return;
    set_raw(s, key, v ? "1" : "0");
}

void settings_set_str(Settings *s, const char *key, const char *v)
{
    if (!s || !v) return;
    set_raw(s, key, v);
}

void settings_reset_key(Settings *s, const char *key)
{
    if (!s) return;
    const SettingMeta *m = schema_of(key);
    int i = find_idx(s, key);
    if (m && i < 0) {                   /* 没存过：无需动作（读取时退回默认） */
        s->dirty = 1;                   /* 仍标记脏，便于显式落盘 */
        return;
    }
    if (i < 0) return;                  /* schema 外且没有：忽略 */
    free(s->e[i].key); free(s->e[i].val);
    for (int j = i; j + 1 < s->n; j++) s->e[j] = s->e[j+1];
    s->n--;
    s->dirty = 1;
}

void settings_reset_all(Settings *s)
{
    if (!s) return;
    for (int i = 0; i < s->n; i++) { free(s->e[i].key); free(s->e[i].val); }
    s->n = 0;
    s->dirty = 1;
}

int settings_dirty(const Settings *s)
{
    return s ? s->dirty : 0;
}
