#include "ui/overlay.h"

#include <string.h>

#include "settings/settings.h"

/* 一张表管死：加浮层只需在这里加一行（key 即 settings.ini 的键名）。
 * 顺序必须与 NmOverlay 枚举一致 —— 用静态断言式的自检（ovl_selftest 会查
 * key 非空 + 反查能回到原 id），不靠人眼对齐。 */
typedef struct {
    const char *key;
    const char *label;
    unsigned    fe;
} OvlMeta;

static const OvlMeta OVL_TAB[OVL_COUNT] = {
    { "alpha_title",     "标题栏",       OVL_FE_C           },
    { "alpha_topbar",    "顶栏",         OVL_FE_C           },
    { "alpha_rcol",      "右栏按钮",     OVL_FE_C | OVL_FE_QT },
    { "alpha_navtop",    "导航顶栏",     OVL_FE_C           },
    { "alpha_navbottom", "重新规划提示", OVL_FE_C           },
    { "alpha_navpanel",  "导航信息面板", OVL_FE_C           },
    { "alpha_navbanner", "底部指令条",   OVL_FE_C           },
    { "alpha_steps",     "步骤列表",     OVL_FE_C           },
    { "alpha_alts",      "备选方案",     OVL_FE_C           },
    { "alpha_menu",      "主菜单",       OVL_FE_C           },
    { "alpha_settings",  "设置面板",     OVL_FE_C           },
    { "alpha_nomap",     "无图提示",     OVL_FE_C           },
    { "alpha_scale",     "比例尺",       OVL_FE_C           },
    { "alpha_compass",   "指北针",       OVL_FE_C           },
    { "alpha_search",    "搜索浮层",     OVL_FE_C | OVL_FE_QT },
    { "alpha_track",     "轨迹/收藏面板", OVL_FE_QT          },
};

const char *ovl_key(NmOverlay id)
{
    if (id < 0 || id >= OVL_COUNT) return NULL;
    return OVL_TAB[id].key;
}

const char *ovl_label(NmOverlay id)
{
    if (id < 0 || id >= OVL_COUNT) return "?";
    return OVL_TAB[id].label;
}

unsigned ovl_frontends(NmOverlay id)
{
    if (id < 0 || id >= OVL_COUNT) return 0;
    return OVL_TAB[id].fe;
}

int ovl_by_key(const char *key)
{
    if (!key || !*key) return -1;
    for (int i = 0; i < OVL_COUNT; i++)
        if (strcmp(OVL_TAB[i].key, key) == 0) return i;
    return -1;
}

int ovl_clamp_pct(int pct)
{
    if (pct < OVL_ALPHA_MIN) return OVL_ALPHA_MIN;
    if (pct > OVL_ALPHA_MAX) return OVL_ALPHA_MAX;
    return pct;
}

int ovl_step_pct(int pct, int dir)
{
    /* 先夹再加减再夹：从 3 这种非法值进来也不会溢出到区间外 */
    int v = ovl_clamp_pct(pct);
    v += (dir > 0) ? OVL_ALPHA_STEP : (dir < 0 ? -OVL_ALPHA_STEP : 0);
    return ovl_clamp_pct(v);
}

/* ---- 从设置读 / 写回（供 app_init 与 app_persist_settings 用） ---- */

void ovl_load_all(Settings *cfg, int out[OVL_COUNT])
{
    for (int i = 0; i < OVL_COUNT; i++)
        out[i] = ovl_clamp_pct(
            settings_get_int(cfg, OVL_TAB[i].key, OVL_ALPHA_DEFAULT));
}

void ovl_store_all(Settings *cfg, const int v[OVL_COUNT])
{
    if (!cfg) return;
    for (int i = 0; i < OVL_COUNT; i++)
        settings_set_int(cfg, OVL_TAB[i].key, ovl_clamp_pct(v[i]));
}

int ovl_count(void) { return (int)OVL_COUNT; }
