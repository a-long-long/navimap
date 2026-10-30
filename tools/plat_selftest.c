/* skedaddle_plat_selftest —— 平台抽象层（utils/plat.h）的全接口自验收。
 *
 * 为什么值得单独立一个门禁：平台层的失效模式全是"编译期全绿、换平台才炸"
 * （路径分隔符、临时目录、字体路径、进程派生、目录枚举）。这台机器只有
 * POSIX 能真跑，所以契约在这里全部断言一遍；Windows 分支按同一契约写，
 * 接口行为由本程序钉死。
 *
 * 判据是双向的（本项目一贯纪律：负向对照必须能失败）：
 *   默认模式      正向契约逐项断言：该成功的必须成功；
 *   --negative    失败路径逐项断言：该失败的必须失败（不存在路径、空 PATH、
 *                 起不来的进程）。同一个函数在两个模式里被要求相反的行为，
 *                 哪一侧坏掉都会红——这不是"永远绿"的门禁。
 *
 * 用法：
 *   ./skedaddle_plat_selftest            正向
 *   ./skedaddle_plat_selftest --negative  负向对照
 *   ./skedaddle_plat_selftest --child-exit=N   内部用：给 spawn 测试当子进程
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils/plat.h"

static int g_fail;
static int g_neg;                 /* 1 = 负向对照模式（只影响打印与统计口径） */

/* 断言：条件必须成立。负向模式里的"必须失败"也写成条件为真
 * （如 nm_file_exists(bogus) == 0），不搞反转——反转会把
 * "本来就该为假"的断言全部误判成意外通过（踩过）。 */
static void check(int cond, const char *what)
{
    if (cond) {
        printf("ok    %s\n", what);
    } else {
        g_fail++;
        printf("FAIL  %s（%s）\n", what, g_neg ? "失败路径没有失败" : "断言不成立");
    }
}

/* 准备步骤：与模式无关，必须为真，否则后面的断言没有意义。 */
static void need(int cond, const char *what)
{
    if (cond) {
        printf("ok    %s\n", what);
    } else {
        g_fail++;
        printf("FAIL  %s（准备失败，后续断言无效）\n", what);
    }
}

static void write_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    if (content) fwrite(content, 1, strlen(content), f);
    fclose(f);
}

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *buf = malloc(64);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, 63, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static int cmp_names(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* ------------------------- 正向契约 ------------------------- */

static void run_positive(const char *base)
{
    char p[4096], q[4096], want[64];

    /* --- 路径 --- */
#ifdef _WIN32
    check(nm_sep() == '\\', "分隔符是反斜杠");
#else
    check(nm_sep() == '/', "分隔符是正斜杠");
#endif

    snprintf(want, sizeof(want), "a%cb", nm_sep());
    check(nm_path_join(p, sizeof(p), "a", "b") == 0 && strcmp(p, want) == 0,
          "path_join 基本拼接");
    check(nm_path_join(p, sizeof(p), "", "b") == 0 && strcmp(p, "b") == 0,
          "path_join 左空串");
    check(nm_path_join(p, sizeof(p), "a", "") == 0 && strcmp(p, "a") == 0,
          "path_join 右空串");
    {
        char tiny[4];
        check(nm_path_join(tiny, sizeof(tiny), "aaaa", "bbbb") == -1,
              "path_join 缓冲区不够必须报错（不许静默截断）");
    }
    check(strcmp(nm_base_name("dir/file.txt"), "file.txt") == 0, "base_name 取文件名");
    check(strcmp(nm_base_name("plain"), "plain") == 0, "base_name 无分隔符");
    check(nm_base_name(NULL) == NULL, "base_name(NULL) 返回 NULL（有定义，不崩）");
#ifdef _WIN32
    check(nm_is_abs("C:/x") == 1 && nm_is_abs("x") == 0, "is_abs 认盘符");
#else
    check(nm_is_abs("/x") == 1 && nm_is_abs("x") == 0, "is_abs 认根路径");
#endif

    /* --- 目录/文件 --- */
    need(nm_is_dir(base) == 1, "自建工作目录存在");
    check(nm_file_exists(base) == 1, "file_exists 对目录也为真（stat 语义）");

    need(nm_path_join(p, sizeof(p), base, "sub/deep") == 0, "mkdir_p 目标路径");
    check(nm_mkdir_p(p) == 0 && nm_is_dir(p) == 1, "mkdir_p 递归建目录");
    check(nm_mkdir_p(base) == 0, "mkdir_p 对已存在目录算成功");

    need(nm_path_join(p, sizeof(p), base, "stat.bin") == 0, "stat 目标路径");
    write_file(p, "0123456789");
    {
        uint64_t sz = 0; int64_t mt = 0;
        check(nm_stat_file(p, &sz, &mt) == 0 && sz == 10 && mt > 0,
              "stat_file 取大小与 mtime");
    }
    check(nm_unlink(p) == 0 && !nm_file_exists(p), "unlink 删文件");

    need(nm_path_join(p, sizeof(p), base, "sub/deep/x") == 0, "rmtree 叶子路径");
    write_file(p, "x");
    need(nm_path_join(p, sizeof(p), base, "sub") == 0, "rmtree 目标路径");
    check(nm_rmtree(p) == 0 && !nm_file_exists(p), "rmtree 递归删除");
    {
        char gone[4096];
        need(nm_path_join(gone, sizeof(gone), base, "gone") == 0, "rmtree 不存在路径");
        check(nm_rmtree(gone) == 0, "rmtree 对不存在路径算成功");
    }

    /* 目录枚举：建 3 文件 + 1 子目录，必须一个不少地枚举到 */
    static const char *kNames[] = { "a.txt", "b.txt", "c.txt" };
    need(nm_path_join(p, sizeof(p), base, "enum") == 0 && nm_mkdir_p(p) == 0,
         "枚举目录就绪");
    for (size_t i = 0; i < 3; i++) {
        need(nm_path_join(p, sizeof(p), base, "enum") == 0 &&
             nm_path_join(q, sizeof(q), p, kNames[i]) == 0, "枚举文件路径");
        write_file(q, "x");
    }
    need(nm_path_join(p, sizeof(p), base, "enum/d") == 0 && nm_mkdir(p) == 0,
         "枚举子目录就绪");
    {
        char *seen[8] = { 0 };
        int nseen = 0;
        char ep[4096];
        need(nm_path_join(ep, sizeof(ep), base, "enum") == 0, "枚举目标路径");
        NmDir *ed = nm_dir_open(ep);
        need(ed != NULL, "nm_dir_open 打开 enum");
        const char *nm;
        while (ed && (nm = nm_dir_next(ed)) != NULL && nseen < 8) {
            char *dup = nm_strdup(nm);
            if (dup) seen[nseen++] = dup;
        }
        if (ed) nm_dir_close(ed);
        qsort(seen, (size_t)nseen, sizeof(seen[0]), cmp_names);
        int all = (nseen == 4);
        for (int i = 0; i < nseen && i < 3; i++)
            if (strcmp(seen[i], kNames[i]) != 0) all = 0;
        if (nseen == 4 && strcmp(seen[3], "d") != 0) all = 0;
        check(all, "目录枚举一个不少（. 与 .. 已跳过）");
        for (int i = 0; i < nseen; i++) free(seen[i]);
    }

    /* 覆盖式改名 */
    need(nm_path_join(p, sizeof(p), base, "rn_a") == 0 &&
         nm_path_join(q, sizeof(q), base, "rn_b") == 0, "rename 路径");
    write_file(p, "A");
    write_file(q, "B");
    check(nm_rename(p, q) == 0, "rename 覆盖已存在的目标");
    {
        char *c = slurp(q);
        check(c && strcmp(c, "A") == 0 && !nm_file_exists(p),
              "rename 后内容是新文件、旧路径消失");
        free(c);
    }

    /* 文件映射 */
    need(nm_path_join(p, sizeof(p), base, "map.bin") == 0, "map 目标路径");
    {
        unsigned char data[256];
        for (int i = 0; i < 256; i++) data[i] = (unsigned char)i;
        FILE *f = fopen(p, "wb");
        if (f) { fwrite(data, 1, sizeof(data), f); fclose(f); }
        size_t len = 0;
        void *m = nm_map_file(p, &len);
        check(m != NULL && len == 256 && memcmp(m, data, 256) == 0,
              "nm_map_file 映射并读回内容");
        nm_unmap_file(m, len);
        nm_unmap_file(NULL, 0);            /* 不能崩 */
        check(nm_map_file(base, &len) == NULL, "映射目录必须失败");
        need(nm_path_join(p, sizeof(p), base, "empty.bin") == 0, "空文件路径");
        write_file(p, NULL);
        check(nm_map_file(p, &len) == NULL, "空文件映射返回 NULL（size<=0）");
    }

    /* --- 进程 --- */
    {
        char exe[4096];
        need(nm_exe_path(exe, sizeof(exe)) == 0 && exe[0] != '\0', "exe_path 非空");
#ifndef _WIN32
        check(exe[0] == '/', "exe_path 是绝对路径");
#endif
        /* spawn 自己：正常退出码要原样带回 */
        char *av1[] = { exe, (char *)"--child-exit=42", NULL };
        char *av2[] = { exe, (char *)"--child-exit=7", NULL };
        int code = -1;
        check(nm_spawn_wait(exe, av1, &code) == 0 && code == 42,
              "spawn_wait 带回退出码 42");
        code = -1;
        check(nm_spawn_wait(exe, av2, &code) == 0 && code == 7,
              "spawn_wait 带回退出码 7");
        char bogus[4096];
        need(nm_path_join(bogus, sizeof(bogus), base, "no_such_tool") == 0,
             "bogus exe 路径");
        char *av3[] = { bogus, NULL };
        code = 0;
        int rc = nm_spawn_wait(bogus, av3, &code);
        check(rc != 0 || code != 0, "spawn 不存在的可执行文件必须失败");
    }

    /* PATH 查找：造一个临时可执行文件，令 PATH 指向它 */
    {
        char dir[4096], tool[4096], found[4096];
        need(nm_path_join(dir, sizeof(dir), base, "pathdir") == 0 &&
             nm_mkdir(dir) == 0, "PATH 测试目录就绪");
#ifdef _WIN32
        need(nm_path_join(tool, sizeof(tool), dir, "tool.exe") == 0, "tool 路径");
#else
        need(nm_path_join(tool, sizeof(tool), dir, "tool") == 0, "tool 路径");
#endif
        write_file(tool, "");
        need(nm_chmod_x(tool) == 0, "chmod_x 加可执行位");
        const char *old = getenv("PATH");
        char saved[8192] = "";
        if (old) snprintf(saved, sizeof(saved), "%s", old);
        need(nm_setenv("PATH", dir) == 0, "setenv 覆盖 PATH");
        check(nm_find_in_path("tool", found, sizeof(found)) == 0 &&
              strcmp(found, tool) == 0, "find_in_path 找到临时工具");
        check(nm_find_in_path("no_such_tool_xyz", found, sizeof(found)) == -1,
              "find_in_path 找不到时必须 -1");
        nm_setenv("PATH", saved);          /* 还原，别污染后续检查 */
    }

    check(nm_pid_alive(nm_get_pid()) == 1, "pid_alive 认自己的 pid");
    check(nm_pid_alive(0) == 0, "pid_alive(0) 必须 0（0 是进程组，不能查）");
    check(nm_pid_alive(-1) == 0, "pid_alive(-1) 必须 0（-1 是全部进程，不能查）");

    /* --- 时间 --- */
    {
        uint64_t t0 = nm_now_ms();
        nm_sleep_ms(20);
        uint64_t dt = nm_now_ms() - t0;
        check(dt >= 10, "sleep_ms(20) 至少睡到 10 ms（不能睡个寂寞）");
        check(nm_now_ms() >= t0, "now_ms 单调不回退");
    }

    /* --- 其它 --- */
    check(nm_temp_dir() && nm_temp_dir()[0] != '\0', "temp_dir 非空");
    check(nm_strcasecmp("AbC", "aBc") == 0 && nm_strcasecmp("abc", "ABD") < 0,
          "strcasecmp 大小写不敏感");
    check(nm_strdup("x") != NULL, "strdup 基本");
    check(nm_peak_rss_mb() > 0, "peak_rss_mb 为正");

    {
        size_t n = 0;
        const char *const *fc = nm_font_candidates(&n);
        need(fc != NULL && n >= 1, "字体候选链非空");
        int all_ok = (fc != NULL);
        for (size_t i = 0; fc && i < n; i++)
            if (!fc[i] || !fc[i][0]) all_ok = 0;
        check(all_ok, "字体候选每项都是非空路径");
#if !defined(_WIN32) && !defined(__APPLE__)
        /* 2026-10-25 起字体策略改为"内置 Noto 优先（与 OsmAnd 同源，跨机器
         * 随 exe 部署）、系统字体兜底"（plat.c kBundledNoto 注释）。这条钉住
         * 新契约的两半：① 有内置 fonts/ 目录时首选必须是 NotoSans-Regular；
         * ② CESI 宋体必须仍在链里（兜底不被丢）；③ 没有内置目录时首选
         * 退回 CESI 宋体（老契约保留为降级路径）。 */
        int cesi_pos = -1, noto_pos = -1;
        for (size_t i = 0; fc && i < n; i++) {
            if (strstr(fc[i], "CESI_SS_GB18030.TTF"))  cesi_pos = (int)i;
            if (strstr(fc[i], "NotoSans-Regular.ttf")) noto_pos = (int)i;
        }
        check(cesi_pos >= 0, "Linux 候选链仍含 CESI 宋体（兜底不被丢）");
        check(noto_pos < 0 || noto_pos == 0,
              "Linux 首选字体是内置 Noto（有内置目录时必须排第一）");
        if (noto_pos < 0)
            check(fc && n >= 1 && strcmp(fc[0],
                      "/usr/share/fonts/truetype/yozo/CESI_SS_GB18030.TTF") == 0,
                  "无内置 fonts/ 时 Linux 首选仍是 CESI 宋体");
#endif
    }
}

/* ------------------------- 负向对照 ------------------------- */

/* 每一项都是"必须失败"。任何意外成功都说明判据没有分辨力。 */
static void run_negative(const char *base)
{
    char bogus[4096], bogus_exe[4096], found[4096];

    need(nm_path_join(bogus, sizeof(bogus), base, "definitely_missing") == 0,
         "bogus 路径");
    need(nm_path_join(bogus_exe, sizeof(bogus_exe), base, "no_such_exe") == 0,
         "bogus exe 路径");

    check(nm_file_exists(bogus) == 0, "负向：file_exists 对不存在文件为假");
    check(nm_is_dir(bogus) == 0, "负向：is_dir 对不存在路径为假");
    {
        uint64_t sz = 12345; int64_t mt = 12345;
        check(nm_stat_file(bogus, &sz, &mt) == -1, "负向：stat_file 失败");
    }
    check(nm_dir_open(bogus) == NULL, "负向：dir_open 打不开不存在目录");
    {
        size_t len = 999;
        check(nm_map_file(bogus, &len) == NULL, "负向：map_file 不存在文件失败");
    }
    /* PATH 指向一个真实但空的目录：查找必须走完整个循环后失败 */
    {
        char dir[4096];
        need(nm_path_join(dir, sizeof(dir), base, "pathdir") == 0, "负向：空 PATH 目录");
        nm_mkdir(dir);
        const char *old = getenv("PATH");
        char saved[8192] = "";
        if (old) snprintf(saved, sizeof(saved), "%s", old);
        need(nm_setenv("PATH", dir) == 0, "负向：setenv 覆盖 PATH");
        check(nm_find_in_path("no_such_tool_xyz", found, sizeof(found)) == -1,
              "负向：PATH 里没有必须找不到");
        nm_setenv("PATH", saved);
    }
    {
        char *av[] = { bogus_exe, NULL };
        int code = 0;
        int rc = nm_spawn_wait(bogus_exe, av, &code);
        check(rc != 0 || code != 0, "负向：spawn 不起进程必须报失败");
    }
    check(nm_pid_alive(0) == 0, "负向：pid_alive(0) 为假");
}

int main(int argc, char **argv)
{
    char b[4096];
    int keep = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--negative") == 0) g_neg = 1;
        else if (strcmp(argv[i], "--keep") == 0) keep = 1;
        else if (strncmp(argv[i], "--child-exit=", 13) == 0)
            return atoi(argv[i] + 13);    /* 给 spawn 测试当子进程 */
        else { fprintf(stderr, "未知参数 %s\n", argv[i]); return 2; }
    }

    snprintf(b, sizeof(b), "%s%cplat_selftest.%d", nm_temp_dir(), nm_sep(),
             nm_get_pid());
    if (nm_mkdir_p(b) != 0) {
        fprintf(stderr, "建不了工作目录 %s\n", b);
        return 1;
    }

    printf(" skedaddle_plat_selftest  %s  工作目录 %s\n", g_neg ? "负向对照" : "正向", b);
    if (g_neg) run_negative(b);
    else       run_positive(b);

    if (!keep) nm_rmtree(b);

    if (g_fail) {
        printf("ASSERT-FAIL  %d 项不符\n", g_fail);
        return 1;
    }
    printf("ASSERT-OK\n");
    return 0;
}
