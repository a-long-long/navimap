/* core_no_sdl —— 断言某个目标里**没有** SDL 符号（方案2 的架构门禁）。
 *
 * 为什么要有它：
 *   Qt 前端（navmap_qt）必须**一行 SDL 都不链**。但"Qt 不链 SDL"很容易在
 *   某次改动里悄悄失效：core 里多加一个 #include <SDL.h> 或一个 SDL 调用，
 *   编译照样过、Qt 照样能跑（只是间接多依赖了 SDL2.dll），直到某天有人在没装
 *   SDL 的机器上构建 Qt 前端才炸。静态链接下更隐蔽：core.a 里引了 SDL 符号，
 *   最终由 navimap_qt 链接进去，依赖就落到 Qt 前端头上了。
 *
 * 判据（查符号表，不是"看起来没有"）：
 *   正向：目标里不得出现任何 SDL_ 未定义符号；
 *   反向：SDL 适配层（navmap.exe）**必须**含 SDL 符号 —— 证明这个检查器
 *         不是"查不出东西的空壳"。
 *
 * ★ 为什么必须**两个提取器取并集**（踩过的坑）：
 *   - `nm -u <PE exe>`：Windows 的 PE 可执行文件里，导入的 DLL 符号**不进**
 *     未定义符号表（实测 navimap.exe 只有 3 行，全是 CRT 的 __tls_*）。
 *     真正的 SDL_CreateWindow 之类在 **objdump -p 的导入表**里。
 *   - `objdump -p <PE exe>`：只对 PE 有用；Linux 的 ELF 走这条只能拿到
 *     .dynamic 段，静态库的导入符号看不到。
 *   - `nm -u <静态库 .a>`：有效（实测 465 行未定义符号）。
 *   所以：nm 和 objdump 两个都跑，取并集。只跑一个会在另一个平台上
 *   静默漏检 —— 这正是"永远绿的门禁"最常见的成因。
 *
 * 用法：
 *   core_no_sdl <文件...>              期望：这些目标里没有 SDL 符号
 *   core_no_sdl --expect-sdl <文件...> 期望：这些目标里**必须有** SDL 符号
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "utils/plat.h"

#if defined(_WIN32)
#  define NM_EXE      "nm.exe"
#  define OBJDUMP_EXE "objdump.exe"
#else
#  define NM_EXE      "nm"
#  define OBJDUMP_EXE "objdump"
#endif

/* stderr 的落点：必须与 stdout 分开，否则错误行会被当成符号行（假绿）。
 * 放在系统临时目录并带进程号，避免污染源码树、也避免并发冲突。 */
static char g_stderr_path[1200];

static const char *stderr_path(void)
{
    if (!g_stderr_path[0]) {
        const char *dir = nm_temp_dir();
        snprintf(g_stderr_path, sizeof(g_stderr_path), "%s%snm_no_sdl_%ld.err",
                 dir ? dir : ".", (dir && dir[0] && dir[strlen(dir) - 1] == '/') ? "" : "/",
                 (long)nm_get_pid());
    }
    return g_stderr_path;
}

/* 定位工具链里的 nm / objdump。
 *
 * ★ 为什么必须自己找（踩过的坑）：Windows 上 popen 走的是**原生** CreateProcess，
 *   它继承的是当前进程的 PATH；而 MSYS2 的 PATH（/d/msys64/ucrt64/bin）在
 *   原生进程里未必被翻译过去 —— 实测 popen("nm.exe ...") 报"系统找不到指定的
 *   文件"，可同一个 shell 里 nm.exe 明明能跑。用 plat 的 nm_find_in_path
 *   （它自己按 Windows 的 ';' 规则切 PATH、并且会补 .exe）就稳了。
 *
 * 找不到就返回 NULL ⇒ 调用方报"无法判定"，绝不静默当成"通过"。 */
static const char *find_tool(const char *name)
{
    static char buf[2][1024];
    static int  slot = 0;
    char path[1024];
    if (nm_find_in_path(name, path, sizeof(path)) == 0) {
        char *b = buf[slot];
        slot = (slot + 1) % 2;
        snprintf(b, sizeof(buf[0]), "%s", path);
        return b;
    }
    return NULL;
}

typedef struct {
    int  total;      /* 扫到的**符号**行数（只算 stdout，不含错误输出） */
    int  sdl;        /* 含 "SDL" 的行数 */
    int  ok;         /* 至少一个提取器成功执行（退出码 0） */
    char first[256]; /* 首个 SDL 命中行 */
} Scan;

/* 跑一条命令，把**stdout** 逐行喂给回调。
 *
 * ★ cmd 兼容 + 不被 stderr 污染（踩过的坑，坑得很隐蔽）：
 *   MinGW 的 popen 走 **cmd.exe**，不是 POSIX shell。两条硬规则：
 *     ① 带引号的可执行文件路径在 cmd 里必须用 `call` 前缀，否则 cmd 把引号
 *        当成普通字符 ⇒ 报"…不是内部或外部命令"；
 *     ② `2>/dev/null` 在 cmd 里是**无效重定向**（/dev/null 不是 Windows 路径），
 *        会让整条命令失败。
 *   ★★ 但 `2>&1` 同样有害：stderr 的错误行会混进 stdout，被当成"符号行"计数。
 *      后果是**假绿**——命令根本没跑起来（"文件不存在"之类），错误行也凑出了
 *      total>0，于是判成"提取成功且无 SDL"。实测就踩到了：把 SDL 版
 *      navimap.exe 传给"期望无 SDL"的判据，它报了 OK。
 *      正确做法：**stderr 单独重定向到临时文件**，stdout 走管道；
 *      再用**退出码**判定这次提取到底成功没有。
 *
 *   Linux 上 /bin/sh 没有 ① ② 两条限制，但同样要把 stderr 分离（语义一致）。 */
static int run_scan(const char *cmd, Scan *sc)
{
    FILE *fp = popen(cmd, "r");
    if (!fp) return 0;
    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        sc->total++;
        if (strstr(line, "SDL")) {
            if (sc->sdl == 0) snprintf(sc->first, sizeof(sc->first), "%s", line);
            sc->sdl++;
        }
    }
    int rc = pclose(fp);
    /* pclose 返回的是命令的退出状态（Unix 语义）。0 = 成功。 */
    if (rc == 0) sc->ok = 1;
    return 1;
}

/* 拼一条"扫某个文件的符号表"的命令。
 * stderr 单独重定向到临时文件（见 run_scan 的说明：混进 stdout 会造成假绿）。 */
static void make_cmd(char *out, size_t cap, const char *tool,
                     const char *args, const char *file)
{
#if defined(_WIN32)
    /* cmd.exe：2> 到 nul 是合法的（Windows 空设备），但错误信息就看不见了；
     * 落到临时文件里，好在失败时把原因报出来。 */
    snprintf(out, cap, "call \"%s\" %s \"%s\" 2>\"%s\"",
             tool, args, file, stderr_path());
#else
    snprintf(out, cap, "\"%s\" %s \"%s\" 2>\"%s\"",
             tool, args, file, stderr_path());
#endif
}

static void scan_file(const char *file, Scan *sc)
{
    char cmd[2600];

    /* ① 未定义符号表：静态库与 Linux ELF 有效 */
    const char *nm = find_tool(NM_EXE);
    if (nm) {
        make_cmd(cmd, sizeof(cmd), nm, "-u", file);
        run_scan(cmd, sc);
    }

    /* ② 导入表：Windows PE 的 DLL 导入符号只在这里 */
    const char *od = find_tool(OBJDUMP_EXE);
    if (od) {
        make_cmd(cmd, sizeof(cmd), od, "-p", file);
        run_scan(cmd, sc);
    }
}

/* 读出 stderr 临时文件的第一行（非空才读），用于失败时说明原因。 */
static void read_stderr(char *out, size_t cap)
{
    if (!cap) return;
    out[0] = '\0';
    FILE *fp = fopen(stderr_path(), "r");
    if (!fp) return;
    while (fgets(out, (int)cap, fp)) {
        size_t n = strlen(out);
        while (n && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = '\0';
        if (n) break;
    }
    fclose(fp);
}

int main(int argc, char **argv)
{
    int expect_sdl = 0, first_arg = 1;
    if (argc > 1 && strcmp(argv[1], "--expect-sdl") == 0) {
        expect_sdl = 1;
        first_arg = 2;
    }
    if (argc <= first_arg) {
        fprintf(stderr,
                "用法: core_no_sdl [--expect-sdl] <文件...>\n"
                "  默认断言这些目标里没有 SDL 符号；--expect-sdl 断言必须有。\n");
        return 2;
    }

    int bad = 0;
    for (int i = first_arg; i < argc; i++) {
        Scan sc;
        memset(&sc, 0, sizeof(sc));
        scan_file(argv[i], &sc);

        /* ★ 判定用"提取器是否成功"（退出码），不是"扫到几行"。
         *   用行数判定会假绿：命令失败时 stderr 混进 stdout 也凑得出行数。
         *   实测踩到过——把 SDL 版 navimap.exe 传给"期望无 SDL"的判据，
         *   因为 nm 在 PE 上不列 DLL 导入符号（只有 3 行 CRT 符号）、
         *   objdump 的输出又被 stderr 噪声污染，结果报了 OK。 */
        if (!sc.ok) {
            char err[512];
            read_stderr(err, sizeof(err));
            fprintf(stderr, "CORE-NO-SDL-FAIL  %s: 提取器都没成功执行"
                            "（文件不存在？或 nm/objdump 不可用？）%s%s\n",
                    argv[i], err[0] ? "  原因: " : "", err);
            bad = 1;
            continue;
        }
        if (expect_sdl) {
            if (sc.sdl > 0) {
                printf("OK  %s  有 %d 个 SDL 符号（首个: %s）\n",
                       argv[i], sc.sdl, sc.first);
            } else {
                fprintf(stderr, "CORE-NO-SDL-FAIL  %s: 期望有 SDL 符号，实际 0 个"
                                "（检查器失效？或该目标已彻底去 SDL？）\n", argv[i]);
                bad = 1;
            }
        } else {
            if (sc.sdl == 0) {
                printf("OK  %s  扫过 %d 行符号，无 SDL\n", argv[i], sc.total);
            } else {
                fprintf(stderr, "CORE-NO-SDL-FAIL  %s: %d/%d 行含 SDL（首个: %s）\n",
                        argv[i], sc.sdl, sc.total, sc.first);
                bad = 1;
            }
        }
    }
    remove(stderr_path());          /* 清掉 stderr 临时文件 */
    if (bad) return 1;
    printf("CORE-NO-SDL-OK\n");
    return 0;
}
