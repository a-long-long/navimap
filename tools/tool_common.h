#ifndef NAVIMAP_TOOL_COMMON_H
#define NAVIMAP_TOOL_COMMON_H

/* tools/ 下各 CLI 的公共小板凳（2026-10-24 从 7~8 个工具里收敛而来）。
 *
 * 收敛前，这些三行代码在多个工具里各抄一遍：
 *   - arg_val：7 份逐字相同
 *   - AutoBuildOpt 默认值 + mapset_load_repair：6 份逐字相同
 * 目的是"改一处处处生效"：以后自动重建要加参数、或装载要打进度回调，
 * 只动这一个文件，所有 exe 全部跟上。
 *
 * ★ 为什么不顺手统一 parse_ll：6 个工具里 skedaddle_lmbuild_cli 多一道 n == 0
 *   （空纬度）的防护，另外 5 个没有 —— 直接统一会改变那 5 个工具的行为
 *   （",116.4" 从 lat=0 变成报错退出）。要统一得先当独立的有意改动做，
 *   不能夹带在纯搬迁里（本项目"逐字节等价"纪律）。
 */

#include "map/mapset.h"
#include "map/autobuild.h"

/* ★ 别再手写 strncmp(a, "--x=", N)：N 该写字面量长度还是长度-1，这个项目
   已经踩过三次（少 1 永不相等、多 1 把 '\0' 比进去），而且取值的偏移
   a+N 还得跟着对。统一走这里：不匹配返回 NULL，否则返回 '=' 之后的串。 */
const char *tc_arg_val(const char *a, const char *prefix);

/* AutoBuildOpt 默认值：开自动重建（产物缺失/损坏时重建；regress 依赖它）。 */
void tc_ab_init(AutoBuildOpt *ab);

/* 装载图集；ab 为 NULL 时用 tc_ab_init 的默认值。
 * 只包一层 mapset_load_repair(..., 进度回调=NULL)：不吞错误信息、不改返回码，
 * 调用方该打的错误信息照旧自己打（各工具措辞不同，regress 各自盯着）。 */
int tc_load_graph(MapSet *ms, const char *graph_dir, const AutoBuildOpt *ab);

#endif /* NAVIMAP_TOOL_COMMON_H */
