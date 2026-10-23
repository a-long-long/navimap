#ifndef NAVIMAP_AUTOBUILD_H
#define NAVIMAP_AUTOBUILD_H

#include "map/mapset.h"

/* ---------------------------------------------------------------------------
 * 启动自愈：源 pbf 还在，产物却没了/坏了 —— 那就当场重建，别让人去记命令。
 *
 * 为什么需要它：src/ 里从来没有 system/popen/exec，程序是纯加载。实测把
 * products/graph/tianjin 删掉后，map_shot 只打印一句"既不是省目录也没有
 * nodes.bin"就空跑，graph_check 直接 ASSERT-FAIL —— 没有任何自愈能力。
 * 而转换本身是确定性的（删掉重建后 nodes.bin 的 md5 与删前完全一致），
 * 所以"检测到缺失就重转"是安全的，不存在"重建出不一样的东西"的风险。
 *
 * 判据刻意与 scripts/sync_maps.sh 完全一致（同一套 manifest 指纹），
 * 避免出现"脚本说该转、程序说不转"的分歧。五条：
 *   1. 产物目录不存在
 *   2. 缺必需文件（manifest.txt / nodes.bin / edges.bin / osmid.bin）
 *   3. nodes.bin 或 edges.bin 打不开、或 nmbin_check 不过（截断/损坏）
 *   4. manifest 里的 binfmt_version 与当前二进制格式版本不符
 *   5. manifest 里的 src_bytes / src_mtime 与源 pbf 实际值不符（源被换过了）
 *
 * 安全性：只起项目自带的 graph_extract 子进程（同目录 → build/ → PATH 依次探测），
 * 参数固定、不经 shell，省名先过白名单（不允许 '/' 和 '.'/'..'），
 * 所以不存在命令注入或路径逃逸。进程派生/等待走平台层 nm_spawn_wait
 * （POSIX fork+execv，Windows CreateProcess）。清理旧产物用平台层的
 * nm_unlink / nm_rmtree 递归删除，不依赖外部 rm 命令。
 * ------------------------------------------------------------------------- */

typedef struct {
    int         enable;    /* 1 = 检测到就重建（默认）；0 = 只诊断不动手 */
    const char *src_dir;   /* 源 pbf 目录。NULL = 自动探测（graph_dir 往上找 sources/） */
    const char *tool;      /* graph_extract 路径。NULL = 自动探测 */
} AutoBuildOpt;

typedef struct {
    int    checked;   /* 检查了几个省 */
    int    ok;        /* 本来就完好的 */
    int    rebuilt;   /* 重建成功的 */
    int    failed;    /* 重建失败的（源损坏或转换器报错） */
    int    nosrc;     /* 产物不可用但源 pbf 也没有，想建也建不了 */
    int    skipped;   /* 需要重建但被 --no-autobuild 关掉了 */
    double elapsed_ms;
} AutoBuildStat;

/* 体检并在需要时重建。返回 0 = 过程本身没出错（不保证每个省都可用，
 * 看 stat->failed / stat->nosrc）。opt 传 NULL 表示"开启 + 全部自动探测"。 */
int  autobuild_repair(const char *graph_dir, const AutoBuildOpt *opt,
                      AutoBuildStat *st_out);

/* 便捷包装：先自愈再加载。st_out 可为 NULL。 */
int  mapset_load_repair(MapSet *ms, const char *graph_dir,
                        const AutoBuildOpt *opt, AutoBuildStat *st_out);

#endif
