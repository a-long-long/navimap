/* skedaddle_settings_selftest —— 设置持久化模块的断言式自测（regress 用）。
 *
 * 判据：全过打印 SETTINGS-OK 且退出码 0；任一断言失败打印 ASSERT-FAIL 且退出码 1。
 * 覆盖：默认值 / 存读往返 / reset_all / NAVIMAP_SETTINGS 路径覆盖 / 损坏文件不崩。
 * 负向对照：持久化一旦坏掉（存了读不出），往返断言立刻 ASSERT-FAIL。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "settings/settings.h"
#include "utils/plat.h"

static int g_fail = 0;
#define CHECK(cond, msg) do {                                              \
    if (!(cond)) {                                                        \
        printf("ASSERT-FAIL %s:%d %s\n", __FILE__, __LINE__, msg);        \
        g_fail++;                                                         \
    }                                                                     \
} while (0)

static char g_tmp[2048];

static void test_defaults(Settings *s)
{
    /* 新建未写盘：所有键都不存在，get_* 退回默认值 */
    CHECK(!settings_has(s, "font_size"),  "默认不应有 font_size 键");
    CHECK(settings_get_int(s, "font_size", 14) == 14, "font_size 默认应为 14");
    CHECK(settings_get_bool(s, "day_night", 0) == 0, "day_night 默认应为 0");
    CHECK(settings_get_dbl(s, "view_lat", 39.9) == 39.9, "view_lat 默认应为 39.9");
    CHECK(strcmp(settings_get_str(s, "missing", "x"), "x") == 0, "缺失键退回默认串");
}

static void test_roundtrip(Settings *s)
{
    settings_set_int(s, "font_size", 20);
    settings_set_bool(s, "day_night", 1);
    settings_set_int(s, "max_class", 2);
    settings_set_dbl(s, "view_lat", 39.912);
    settings_set_dbl(s, "view_lon", 116.404);
    settings_set_dbl(s, "view_zoom", 14.0);
    CHECK(settings_save_as(s, g_tmp) == 0, "save 应成功");

    Settings *s2 = settings_create();
    CHECK(s2 != NULL, "二次 create 不应为 NULL");
    CHECK(settings_load(s2, g_tmp) == 0, "从临时文件 load 应成功");
    CHECK(settings_get_int(s2, "font_size", 0) == 20, "往返 font_size 应为 20");
    CHECK(settings_get_bool(s2, "day_night", 0) == 1, "往返 day_night 应为 1");
    CHECK(settings_get_int(s2, "max_class", 0) == 2, "往返 max_class 应为 2");
    CHECK(settings_get_dbl(s2, "view_lat", 0) == 39.912, "往返 view_lat 应为 39.912");
    CHECK(settings_get_dbl(s2, "view_lon", 0) == 116.404, "往返 view_lon 应为 116.404");
    CHECK(settings_get_dbl(s2, "view_zoom", 0) == 14.0, "往返 view_zoom 应为 14.0");
    settings_destroy(s2);
}

static void test_reset(Settings *s)
{
    settings_set_int(s, "font_size", 28);
    settings_reset_all(s);
    CHECK(settings_save_as(s, g_tmp) == 0, "reset 后 save 应成功");
    Settings *s2 = settings_create();
    CHECK(settings_load(s2, g_tmp) == 0, "reset 后 load 应成功");
    CHECK(!settings_has(s2, "font_size"), "reset 后 font_size 键应消失");
    CHECK(settings_get_int(s2, "font_size", 14) == 14, "reset 后读回默认 14");
    settings_destroy(s2);
}

static void test_env_override(void)
{
    /* NAVIMAP_SETTINGS 指向独立路径，load(NULL) 应落到该路径 */
    char envpath[2048];
    snprintf(envpath, sizeof(envpath), "%s/settings_env_override.ini", nm_temp_dir());
    FILE *f = fopen(envpath, "wb");
    if (f) { fputs("mode=1\n", f); fclose(f); }

    nm_setenv("NAVMAP_SETTINGS", envpath);
    Settings *s = settings_create();
    CHECK(s != NULL, "env-override create 不应为 NULL");
    CHECK(settings_load(s, NULL) == 0, "env-override load(NULL) 应成功");
    CHECK(strcmp(settings_path(s), envpath) == 0, "path 应等于 NAVIMAP_SETTINGS");
    CHECK(settings_get_int(s, "mode", 0) == 1, "env 文件里的 mode 应为 1");
    settings_destroy(s);
    nm_setenv("NAVMAP_SETTINGS", "");   /* 复位，避免影响其它用例 */
}

static void test_corrupt(void)
{
    /* 损坏文件：写入乱码，load 不应崩溃，且值退回默认 */
    FILE *f = fopen(g_tmp, "wb");
    if (f) {
        fputs("\x00\x01\x02not = valid\nshort\n=k=v=extra\n", f);
        /* 故意写超长无换行（截断在缓冲内也应被安全处理） */
        for (int i = 0; i < 5000; i++) fputc('a', f);
        fputs("=noeq\n", f);
        fclose(f);
    }
    Settings *s = settings_create();
    CHECK(s != NULL, "corrupt create 不应为 NULL");
    CHECK(settings_load(s, g_tmp) == 0, "损坏文件 load 应返回 0（不崩）");
    CHECK(settings_get_int(s, "font_size", 14) == 14, "损坏文件后 font_size 退回默认");
    settings_destroy(s);
}

int main(void)
{
    snprintf(g_tmp, sizeof(g_tmp), "%s/skedaddle_settings_selftest.ini", nm_temp_dir());

    Settings *s = settings_create();
    if (!s) { printf("ASSERT-FAIL 无法创建 Settings\n"); return 1; }

    test_defaults(s);
    test_roundtrip(s);
    test_reset(s);
    test_env_override();
    test_corrupt();

    settings_destroy(s);

    if (g_fail == 0) {
        printf("SETTINGS-OK 全部断言通过（默认值/往返/reset/env覆盖/损坏文件）\n");
        return 0;
    }
    printf("SETTINGS-FAIL 共 %d 条断言失败\n", g_fail);
    return 1;
}
