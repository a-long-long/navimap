#ifndef NAVMAP_SETTINGS_H
#define NAVMAP_SETTINGS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * 设置持久化（完整设置系统）
 *
 * 一个极简的 key=value INI 存读器 + 一份"可编辑设置清单"（SETTINGS_SCHEMA），
 * 供 chrome 设置面板逐项渲染。值以字符串形式存储，按需解析成 int/double/bool。
 *
 * 优先级：命令行参数 / NAVIMAP_* 环境变量 > 配置文件 > 代码默认值。
 *   - 文件默认路径：环境变量 NAVIMAP_SETTINGS（完整路径）>
 *     nm_config_dir()/settings.ini（用户级，不被 products/ 重建清掉）。
 *   - app_init 先把配置文件的值当成"默认值"填进 App；随后既有的 CLI 参数、
 *     环境变量读取逻辑会自然覆盖（谁后写谁赢），从而满足"CLI/env 优先"。
 * ------------------------------------------------------------------------- */

typedef enum {
    ST_BOOL,
    ST_INT,
    ST_FLOAT
} SettingType;

/* 一条可编辑设置的元数据：面板怎么显示、范围、默认值。
 * 实际值存在 Settings 的 key=value 表里（key 同时是 INI 键名）。 */
typedef struct {
    const char  *key;
    const char  *label;
    SettingType  type;
    double       def;                 /* 默认值（bool 用 0/1） */
    double       min, max, step;
    const char  *opts;                /* ST_INT 当枚举时："浏览\0导航\0"；NULL=数值 */
} SettingMeta;

/* 持久化设置清单（面板按此遍历；view_lat/lon/zoom 不入此表，只在退出时记）。 */
extern const SettingMeta SETTINGS_SCHEMA[];
extern const int         SETTINGS_SCHEMA_N;

typedef struct Settings Settings;

Settings *settings_create(void);
void      settings_destroy(Settings *s);

/* 载入：path 为 NULL 时用默认路径。
 * 文件不存在/损坏返回 0 且保持默认值（非致命）；返回 -1 仅表示内存分配失败。 */
int  settings_load(Settings *s, const char *path);
/* 写回载入时记录的路径；save_as 指定新路径。返回 0 成功，-1 失败。 */
int  settings_save(Settings *s);
int  settings_save_as(Settings *s, const char *path);
const char *settings_path(const Settings *s);

int    settings_has(const Settings *s, const char *key);
int    settings_get_int(const Settings *s, const char *key, int def);
double settings_get_dbl(const Settings *s, const char *key, double def);
int    settings_get_bool(const Settings *s, const char *key, int def);
const char *settings_get_str(const Settings *s, const char *key, const char *def);

void settings_set_int(Settings *s, const char *key, int v);
void settings_set_dbl(Settings *s, const char *key, double v);
void settings_set_bool(Settings *s, const char *key, int v);
void settings_set_str(Settings *s, const char *key, const char *v);

/* 把某 key 重置为默认值（schema 里没有的 key 直接删除）。 */
void settings_reset_key(Settings *s, const char *key);
/* 全部重置为默认（清空所有条目——再次存盘即为最小文件）。 */
void settings_reset_all(Settings *s);

/* 自上次 save 起是否有改动（set_* 后置 1，save 后清 0）。 */
int  settings_dirty(const Settings *s);

#ifdef __cplusplus
}
#endif
#endif /* NAVMAP_SETTINGS_H */
