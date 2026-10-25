#ifndef NAVIMAP_OVERLAY_H
#define NAVIMAP_OVERLAY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * 浮层（画在地图之上的界面层）清单与**逐浮层透明度**
 *
 * 需求：所有浮层默认 75% 半透明，且每个浮层可由用户单独设置。
 *
 * 设计要点：
 *   1. 一张表管 16 个浮层（key / 中文标签 / 在哪些前端真的被画出来）。
 *      key 同时是 settings.ini 的键名（"alpha_title" 等），与 view_lat/lon/zoom
 *      一样**不入 SETTINGS_SCHEMA**（那个表是"设置面板主列表"，16 行会把面板撑爆），
 *      而是由本模块显式读写；settings_reset_all 清空条目即回到默认 75%。
 *   2. 值统一存**百分比 0~100**（用户看得懂、面板上是整数），绘制时才转 0~255。
 *      转 255 用 ×255/100 而不是 ×2.55 的浮点截断，保证 100% 恒等于 255（不透明）。
 *   3. ovl_frontends() 说明该浮层在哪个前端存在。SDL/map_shot 是自绘（C 侧），
 *      Qt 前端由真控件承担 ⇒ 同一个设置项在没画它的前端上不生效，UI 里要标出来
 *      而不是假装能调（否则用户调了半天没反应）。
 * ------------------------------------------------------------------------- */

typedef enum {
    OVL_TITLE = 0,      /* 自绘标题栏 */
    OVL_TOPBAR,         /* 顶栏（菜单按钮 + 搜索框 + 模式页签） */
    OVL_RCOL,           /* 右栏按钮（放大/缩小/定位/昼夜…） */
    OVL_NAVTOP,         /* 导航顶栏（返回 + 目的地 + 退出） */
    OVL_NAVBOTTOM,      /* "重新规划中…" 提示条 */
    OVL_NAVPANEL,       /* 左上导航信息面板（路线摘要 + 转向列表） */
    OVL_NAVBANNER,      /* 底部指令条（模拟导航时） */
    OVL_STEPS,          /* 步骤列表（chrome 版） */
    OVL_ALTS,           /* 备选方案对比面板 */
    OVL_MENU,           /* ☰ 主菜单 */
    OVL_SETTINGS,       /* 设置面板（含浮层透明度子面板） */
    OVL_NOMAP,          /* 无图卡片 / 无图提示条 */
    OVL_SCALE,          /* 比例尺 */
    OVL_COMPASS,        /* 指北针 */
    OVL_SEARCH,         /* 搜索框 + 结果列表 */
    OVL_TRACK,          /* 轨迹 / 收藏面板 */
    OVL_COUNT
} NmOverlay;

#define OVL_ALPHA_DEFAULT 75     /* 百分比：需求指定的默认半透明 */
#define OVL_ALPHA_MIN     0
#define OVL_ALPHA_MAX     100
#define OVL_ALPHA_STEP    5      /* 面板上每次 ±5% */

/* 该浮层在哪个前端真的被画出来（位掩码）。 */
#define OVL_FE_C  1u             /* C 自绘：SDL navmap / map_shot */
#define OVL_FE_QT 2u             /* Qt 前端控件 */

const char *ovl_key(NmOverlay id);        /* "alpha_title" …；越界返回 NULL */
const char *ovl_label(NmOverlay id);      /* "标题栏" …；越界返回 "?" */
unsigned    ovl_frontends(NmOverlay id);  /* OVL_FE_* 位掩码 */
int         ovl_by_key(const char *key);  /* 反查；找不到返回 -1 */

int ovl_clamp_pct(int pct);               /* 夹到 [0,100] */
int ovl_step_pct(int pct, int dir);       /* dir>0 加一档、dir<0 减一档 */
int ovl_count(void);                      /* = OVL_COUNT（给 UI/自检遍历用） */

/* 设置持久化：前向声明，避免本头依赖 settings 头（与 window.h 的惯例一致）。 */
typedef struct Settings Settings;

/* 从设置读全部浮层透明度（键缺失/越界 ⇒ 默认 OVL_ALPHA_DEFAULT）。 */
void ovl_load_all(Settings *cfg, int out[OVL_COUNT]);
/* 写回全部（只在值真的变了时才 set ⇒ 不会把文件搞脏）。 */
void ovl_store_all(Settings *cfg, const int v[OVL_COUNT]);

#ifdef __cplusplus
}
#endif

#endif /* NAVIMAP_OVERLAY_H */
