#ifndef NAVIMAP_PLAT_H
#define NAVIMAP_PLAT_H

/* ---------------------------------------------------------------------------
 * 平台抽象层。规则：**"换个平台就不成立"的东西只许出现在这里**。
 *
 * 收编的对象（都在这台机器之外炸过或明显会炸）：
 *   - 路径分隔符 '/' vs '\\'、盘符绝对路径
 *   - 临时目录 /tmp（Windows 没有）
 *   - 字体路径 /usr/share/fonts/...（Windows 在 %WINDIR%\Fonts）
 *   - mmap/madvise（Windows 用 CreateFileMapping）
 *   - opendir/readdir、mkdir、unlink、rmdir、lstat
 *   - /proc/self/exe（Windows 用 GetModuleFileName）
 *   - fork/execv/waitpid、kill、getpid
 *   - usleep、clock_gettime(CLOCK_MONOTONIC)
 *   - getrusage（峰值内存）
 *
 * POSIX 分支覆盖 Linux/macOS；Windows 分支覆盖 MSVC 与 MinGW。
 * ★ 平台差异 100% 收口在本抽象层：业务代码只调 nm_* 接口、零 #ifdef _WIN32，
 *   同一套源文件由编译器按 _WIN32 在编译期选分支，本地/远端无分叉。
 *   Windows 分支已在 MSYS2/UCRT64(MinGW) 实机编译验证（plat_selftest 全过）；
 *   POSIX 侧仍由 tools/plat_selftest.c 保证"接口契约"可断言。
 * ------------------------------------------------------------------------- */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- 路径 ---------------- */

char        nm_sep(void);                    /* '/' 或 '\\' */
const char *nm_last_sep(const char *path);   /* 最后一个分隔符（没有返回 NULL） */
const char *nm_base_name(const char *path);  /* 最后一个分隔符之后 */
int         nm_is_abs(const char *path);     /* '/' 开头或盘符开头 */

/* 拼接。空串参数被当成"没有这一级"（join("", "b") == "b"）。
 * 缓冲区不够返回 -1（绝不静默截断——截断的路径会写到别的省上）。 */
int nm_path_join(char *out, size_t cap, const char *a, const char *b);

/* ---------------- 文件 / 目录 ---------------- */

int nm_is_dir(const char *path);
int nm_file_exists(const char *path);
/* 存在且可执行。Windows 没有可执行位，"存在"即算可执行
 * （能不能跑由扩展名/.exe 决定，见 nm_find_in_path）。 */
int nm_executable(const char *path);

/* 取大小（字节）与 mtime（秒）。文件不存在返回 -1。 */
int nm_stat_file(const char *path, uint64_t *size, int64_t *mtime);

int nm_mkdir(const char *path);      /* 单层 */
int nm_mkdir_p(const char *path);    /* 递归，已存在算成功 */
int nm_rmtree(const char *path);     /* 递归删除；不存在算成功 */
int nm_unlink(const char *path);
/* 独占创建并写入一小段内容（转换锁用：文件已存在即失败，不会截断别人的锁）。
 * 成功返回 0；失败（已存在/建不了/写不进）返回 -1。 */
int nm_create_exclusive(const char *path, const char *content);
/* 覆盖式改名。POSIX rename 本身就是原子替换；Windows 的 rename()
 * 在目标存在时失败，所以这里显式 MOVEFILE_REPLACE_EXISTING。 */
int nm_rename(const char *from, const char *to);

/* 只读映射整个文件，长度写入 *out_len，返回基址；失败返回 NULL。
 * 空文件返回 NULL（与旧行为一致：size<=0 不当可映射）。 */
void *nm_map_file(const char *path, size_t *out_len);
void  nm_unmap_file(void *addr, size_t len);

/* 目录枚举。返回的条目名属于 NmDir，下次 nm_dir_next 前有效；
 * "." 与 ".." 已跳过。顺序由文件系统决定，需要可复现就自己排序
 * （mapset_load / places_build 都先收集再 qsort）。 */
typedef struct NmDir NmDir;
NmDir      *nm_dir_open(const char *path);
const char *nm_dir_next(NmDir *d);
void        nm_dir_close(NmDir *d);

/* ---------------- 并发 ---------------- */

/* 互斥锁（跨平台）。业务代码只调 nm_mutex_*，不直接碰 pthread / CRITICAL_SECTION。
 * 单一所有权：创建者负责 nm_mutex_destroy。锁内不要做阻塞/耗时操作。 */
typedef struct NmMutex NmMutex;
NmMutex *nm_mutex_create(void);
void     nm_mutex_destroy(NmMutex *m);
void     nm_mutex_lock(NmMutex *m);
void     nm_mutex_unlock(NmMutex *m);

/* 起一个脱离式（detached）线程跑 f(arg)。成功返回 0，失败返回 -1。
 * 线程自行结束，调用方不 join；f 内部自行处理自己的资源。 */
int nm_thread_spawn(void (*f)(void *), void *arg);

/* ---------------- 进程 / 时间 ---------------- */

/* 当前可执行文件路径（Linux /proc/self/exe，macOS _NSGetExecutablePath，
 * Windows GetModuleFileName）。失败返回 -1，调用方应退回 PATH 查找。 */
int nm_exe_path(char *out, size_t cap);
int nm_get_pid(void);
int nm_pid_alive(int pid);            /* pid<=0 一律返回 0 */

/* 在 PATH 里找可执行文件。Windows 上会补 ".exe" 再试一次。
 * 找不到返回 -1。 */
int nm_find_in_path(const char *name, char *out, size_t cap);

/* 在指定目录里找可执行文件（Windows 上补 ".exe"）。
 * find_tool 的"与本程序同目录"分支用它。找不到返回 -1。 */
int nm_which(const char *dir, const char *name, char *out, size_t cap);

/* 起一个进程并等它结束。exit_code 可传 NULL。
 * 返回 0 = 正常退出（码在 *exit_code）；-1 = 起不来/被信号杀。 */
int nm_spawn_wait(const char *exe, char *const argv[], int *exit_code);

uint64_t nm_now_ms(void);             /* 单调时钟，毫秒 */
uint64_t nm_now_us(void);             /* 单调时钟，微秒（基准测试要亚毫秒精度） */
void     nm_sleep_ms(unsigned ms);
double   nm_peak_rss_mb(void);        /* 峰值驻留内存（MB） */

/* ---------------- 其它 ---------------- */

int        nm_strcasecmp(const char *a, const char *b);
char      *nm_strdup(const char *s);  /* 失败返回 NULL（与 strdup 一致） */
/* 临时目录：TMPDIR/TEMP/TMP 里第一个存在的，退回 /tmp，再退回 "." */
const char *nm_temp_dir(void);
/* 用户级配置目录（跨平台）：POSIX 取 $XDG_CONFIG_HOME/navimap 或
   $HOME/.config/navimap；Windows 取 %APPDATA%/navimap。返回的字符串属于
   ﻿本函数（静态缓冲），调用方不应 free，也无需手动建目录（settings 写盘时
   会用 nm_mkdir_p 确保存在）。统一返回正斜杠形式。 */
const char *nm_config_dir(void);
/* 环境变量覆盖式设置（setenv / _putenv_s） */
int nm_setenv(const char *key, const char *val);
/* 加可执行位。Windows 无可执行位，直接成功（存在即可执行）。 */
int nm_chmod_x(const char *path);

/* 逐平台字体候选链（宋体优先，理由见 render/font.c 的注释）。
 * 返回的数组与字符串都是静态存储，勿 free。 */
const char *const *nm_font_candidates(size_t *n);

#ifdef __cplusplus
}
#endif

#endif /* NAVIMAP_PLAT_H */
