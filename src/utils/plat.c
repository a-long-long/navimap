/* ---------------------------------------------------------------------------
 * plat.c —— 平台抽象层实现。接口契约见 utils/plat.h。
 *
 * 维护规则：
 *   - 新增"只在某平台成立"的调用时，**先在这里加函数，再让业务代码调它**；
 *     不允许业务文件里再出现 #ifdef _WIN32。
 *   - POSIX 分支必须保持 Linux/macOS 都能编（macOS 没有 /proc、ru_maxrss 是字节）。
 *   - 改任何行为判据前先看 tools/plat_selftest.c：那边是全接口契约。
 * ------------------------------------------------------------------------- */
#include "utils/plat.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  ifndef _WIN32_WINNT
#    define _WIN32_WINNT 0x0501      /* Vista+：GetTickCount64 / 受限查询句柄 */
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <psapi.h>
#  include <sys/stat.h>     /* MinGW 的 POSIX stat() / struct stat（nm_stat_file 用） */
#else
#  include <dirent.h>
#  include <fcntl.h>
#  include <signal.h>
#  include <strings.h>
#  include <sys/mman.h>
#  include <sys/resource.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <time.h>
#  include <unistd.h>
#  include <pthread.h>
#endif

#ifdef __APPLE__
#  include <mach-o/dyld.h>           /* _NSGetExecutablePath：macOS 没有 /proc */
#endif

/* POSIX 上 '\' 是合法文件名字符，只有 Windows 把它当分隔符。 */
#ifdef _WIN32
#  define NM_IS_SEP(c) ((c) == '/' || (c) == '\\')
#else
#  define NM_IS_SEP(c) ((c) == '/')
#endif

/* ============================ 路径 ============================ */

char nm_sep(void)
{
#ifdef _WIN32
    return '\\';
#else
    return '/';
#endif
}

const char *nm_last_sep(const char *path)
{
    if (!path) return NULL;
    const char *best = NULL;
    for (const char *p = path; *p; p++)
        if (NM_IS_SEP(*p)) best = p;
    return best;
}

const char *nm_base_name(const char *path)
{
    const char *s = nm_last_sep(path);
    return s ? s + 1 : path;
}

int nm_is_abs(const char *path)
{
    if (!path || !*path) return 0;
#ifdef _WIN32
    if (isalpha((unsigned char)path[0]) && path[1] == ':') return 1;
#endif
    return NM_IS_SEP(path[0]);
}

int nm_path_join(char *out, size_t cap, const char *a, const char *b)
{
    if (!out || cap == 0 || !a || !b) return -1;
    int n;
    if (!*a)       n = snprintf(out, cap, "%s", b);
    else if (!*b)  n = snprintf(out, cap, "%s", a);
    else           n = snprintf(out, cap, "%s%c%s", a, nm_sep(), b);
    return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}

/* ======================= 文件 / 目录 ======================= */

int nm_is_dir(const char *path)
{
    if (!path) return 0;
#ifdef _WIN32
    DWORD attr = GetFileAttributesA(path);
    return (attr != INVALID_FILE_ATTRIBUTES &&
            (attr & FILE_ATTRIBUTE_DIRECTORY)) ? 1 : 0;
#else
    struct stat st;
    return (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) ? 1 : 0;
#endif
}

int nm_file_exists(const char *path)
{
    if (!path) return 0;
#ifdef _WIN32
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES ? 1 : 0;
#else
    struct stat st;
    return stat(path, &st) == 0 ? 1 : 0;
#endif
}

int nm_executable(const char *path)
{
    if (!path) return 0;
#ifdef _WIN32
    /* Windows 没有可执行位；能不能跑由扩展名决定（调用方补 .exe）。 */
    return nm_file_exists(path) && !nm_is_dir(path) ? 1 : 0;
#else
    return access(path, X_OK) == 0 ? 1 : 0;
#endif
}

int nm_stat_file(const char *path, uint64_t *size, int64_t *mtime)
{
    if (!path) return -1;
#ifdef _WIN32
    /* MinGW-w64 提供 POSIX stat()（off_t 为 64 位，足以覆盖大图文件）；
       MSVC 用 _stat64（等价于被取代的 _stati64）。字段名 st_size/st_mtime 一致。 */
#  if defined(_MSC_VER)
    struct _stat64 st;
    if (_stat64(path, &st) != 0) return -1;
#  else
    struct stat st;
    if (stat(path, &st) != 0) return -1;
#  endif
    if (size)  *size  = (uint64_t)st.st_size;
    if (mtime) *mtime = (int64_t)st.st_mtime;
    return 0;
#else
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    if (size)  *size  = (uint64_t)st.st_size;
    if (mtime) *mtime = (int64_t)st.st_mtime;
    return 0;
#endif
}

int nm_mkdir(const char *path)
{
    if (!path || !*path) return -1;
#ifdef _WIN32
    return CreateDirectoryA(path, NULL) ? 0 : -1;
#else
    return mkdir(path, 0755) == 0 ? 0 : -1;
#endif
}

int nm_mkdir_p(const char *path)
{
    if (!path || !*path) return -1;
    char buf[4096];
    size_t n = strlen(path);
    if (n >= sizeof(buf)) return -1;
    memcpy(buf, path, n + 1);
    for (size_t i = 1; i <= n; i++) {
        if (!NM_IS_SEP(buf[i]) && buf[i] != '\0') continue;
        char save = buf[i];
        buf[i] = '\0';
        if (!nm_is_dir(buf) && nm_mkdir(buf) != 0) {
            /* 已存在不算失败（并发创建/别人建过都走这条） */
#ifdef _WIN32
            int exists = (GetLastError() == ERROR_ALREADY_EXISTS);
#else
            int exists = (errno == EEXIST);
#endif
            buf[i] = save;
            if (!exists) return -1;
            continue;
        }
        buf[i] = save;
    }
    return 0;
}

/* 单层删目录。POSIX rmdir / Windows RemoveDirectoryA 都只删空目录。 */
static int nm_rmdir_one(const char *path)
{
#ifdef _WIN32
    return RemoveDirectoryA(path) ? 0 : -1;
#else
    return rmdir(path) == 0 ? 0 : -1;
#endif
}

int nm_rmtree(const char *path)
{
    if (!path || !*path) return -1;
    NmDir *d = nm_dir_open(path);
    if (!d) {
        /* 不是目录：是文件就删掉，什么都没有算成功 */
        if (nm_file_exists(path)) return nm_unlink(path);
        return 0;
    }
    const char *name;
    char child[4096];
    int rc = 0;
    while ((name = nm_dir_next(d)) != NULL) {
        if (nm_path_join(child, sizeof(child), path, name) != 0) { rc = -1; continue; }
        if (nm_rmtree(child) != 0) rc = -1;
    }
    nm_dir_close(d);
    if (nm_rmdir_one(path) != 0) rc = -1;
    return rc;
}

int nm_unlink(const char *path)
{
    if (!path) return -1;
#ifdef _WIN32
    return DeleteFileA(path) ? 0 : -1;
#else
    return unlink(path) == 0 ? 0 : -1;
#endif
}

int nm_create_exclusive(const char *path, const char *content)
{
    if (!path || !*path) return -1;
#ifdef _WIN32
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    DWORD n = 0;
    BOOL ok = WriteFile(h, content ? content : "", content ? (DWORD)strlen(content) : 0,
                        &n, NULL) && (!content || n == (DWORD)strlen(content));
    CloseHandle(h);
    return ok ? 0 : -1;
#else
    int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0644);
    if (fd < 0) return -1;
    size_t len = content ? strlen(content) : 0;
    ssize_t w = len ? write(fd, content, len) : 0;
    int ok = (w == (ssize_t)len);
    if (close(fd) != 0) ok = 0;
    return ok ? 0 : -1;
#endif
}

int nm_rename(const char *from, const char *to)
{
    if (!from || !to) return -1;
#ifdef _WIN32
    /* rename() 在 Windows 上目标存在即失败；显式要"覆盖替换"语义。 */
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
#else
    return rename(from, to) == 0 ? 0 : -1;
#endif
}

/* ---------------- 文件映射 ---------------- */

#ifdef _WIN32
/* MapViewOfFile 的视图在映射句柄关闭前有效，所以句柄不能随手关——
 * 用一张小注册表按基址找回它。条目几百个足够（每省 ~10 个文件），
 * 满了就退化为"能映射、句柄关不掉"，绝不因此让映射失败。 */
#  define NM_MAX_MAPS 512
typedef struct { void *base; HANDLE h; } NmMapEnt;
static NmMapEnt g_nm_maps[NM_MAX_MAPS];
static int      g_nm_nmaps;

static int nm_map_reg_add(void *base, HANDLE h)
{
    for (int i = 0; i < g_nm_nmaps; i++)
        if (g_nm_maps[i].base == base) { g_nm_maps[i].h = h; return 1; }
    if (g_nm_nmaps >= NM_MAX_MAPS) return 0;
    g_nm_maps[g_nm_nmaps].base = base;
    g_nm_maps[g_nm_nmaps].h    = h;
    g_nm_nmaps++;
    return 1;
}
#endif

void *nm_map_file(const char *path, size_t *out_len)
{
    if (!path) return NULL;
#ifdef _WIN32
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(f, &sz) || sz.QuadPart <= 0) { CloseHandle(f); return NULL; }
    HANDLE m = CreateFileMappingA(f, NULL, PAGE_READONLY, 0, 0, NULL);
    CloseHandle(f);                    /* 映射建成后文件句柄可以关 */
    if (!m) return NULL;
    void *p = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
    if (!p) { CloseHandle(m); return NULL; }
    if (!nm_map_reg_add(p, m)) {        /* 注册表满：视图仍然可用，句柄留在这里 */
        UnmapViewOfFile(p);
        CloseHandle(m);
        return NULL;
    }
    if (out_len) *out_len = (size_t)sz.QuadPart;
    return p;
#else
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) { close(fd); return NULL; }
    void *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return NULL;
    if (out_len) *out_len = (size_t)st.st_size;
    return p;
#endif
}

void nm_unmap_file(void *addr, size_t len)
{
    if (!addr) return;
#ifdef _WIN32
    for (int i = 0; i < g_nm_nmaps; i++) {
        if (g_nm_maps[i].base == addr) {
            UnmapViewOfFile(addr);
            CloseHandle(g_nm_maps[i].h);
            g_nm_maps[i] = g_nm_maps[--g_nm_nmaps];
            return;
        }
    }
    UnmapViewOfFile(addr);             /* 注册表里没有（理论不到） */
    (void)len;
#else
    (void)munmap(addr, len);
#endif
}

/* ---------------- 目录枚举 ---------------- */

struct NmDir {
#ifdef _WIN32
    HANDLE           h;
    WIN32_FIND_DATAA fd;
    int              pending;          /* 还有一条没交付 */
    char             ent[260];         /* 复用的返回缓冲（避开 cFileName 被 FindNextFileA 覆盖） */
#else
    DIR             *d;
#endif
};

NmDir *nm_dir_open(const char *path)
{
    if (!path || !*path) return NULL;
    NmDir *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
#ifdef _WIN32
    char pat[4096];
    int n = snprintf(pat, sizeof(pat), "%s%s*", path,
                     nm_last_sep(path) && nm_last_sep(path)[1] == '\0' ? "" : "\\");
    if (n < 0 || (size_t)n >= sizeof(pat)) { free(d); return NULL; }
    d->h = FindFirstFileA(pat, &d->fd);
    if (d->h == INVALID_HANDLE_VALUE) { free(d); return NULL; }
    d->pending = 1;
    return d;
#else
    d->d = opendir(path);
    if (!d->d) { free(d); return NULL; }
    return d;
#endif
}

const char *nm_dir_next(NmDir *d)
{
    if (!d) return NULL;
#ifdef _WIN32
    while (d->pending) {
        if (strcmp(d->fd.cFileName, ".") != 0 &&
            strcmp(d->fd.cFileName, "..") != 0) {
            /* 复制到自有缓冲再推进 fd：FindNextFileA 会覆盖 d->fd.cFileName，
               直接返回别名指针会指向错误条目；且必须在交付前推进，否则会
               无限返回同一条（死循环）。 */
            strncpy(d->ent, d->fd.cFileName, sizeof(d->ent) - 1);
            d->ent[sizeof(d->ent) - 1] = '\0';
            d->pending = FindNextFileA(d->h, &d->fd) ? 1 : 0;
            return d->ent;
        }
        d->pending = FindNextFileA(d->h, &d->fd) ? 1 : 0;
    }
    return NULL;
#else
    struct dirent *de;
    while ((de = readdir(d->d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        return de->d_name;
    }
    return NULL;
#endif
}

void nm_dir_close(NmDir *d)
{
    if (!d) return;
#ifdef _WIN32
    if (d->h != INVALID_HANDLE_VALUE) FindClose(d->h);
#else
    if (d->d) closedir(d->d);
#endif
    free(d);
}

/* ======================= 进程 / 时间 ======================= */

int nm_exe_path(char *out, size_t cap)
{
    if (!out || cap == 0) return -1;
#ifdef _WIN32
    DWORD n = GetModuleFileNameA(NULL, out, (DWORD)cap);
    if (n == 0 || (size_t)n >= cap) return -1;
    return 0;
#elif defined(__APPLE__)
    uint32_t sz = (uint32_t)cap;
    if (_NSGetExecutablePath(out, &sz) != 0) return -1;   /* 缓冲区不够 */
    return 0;
#else
    ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    if (n <= 0) return -1;
    out[n] = '\0';
    return 0;
#endif
}

int nm_get_pid(void)
{
#ifdef _WIN32
    return (int)GetCurrentProcessId();
#else
    return (int)getpid();
#endif
}

int nm_pid_alive(int pid)
{
    if (pid <= 0) return 0;             /* 0 是调用方进程组、-1 是全部进程，都别查 */
#ifdef _WIN32
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!h) return 0;
    DWORD code = 0;
    int alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
#else
    return (kill((pid_t)pid, 0) == 0 || errno == EPERM) ? 1 : 0;
#endif
}

int nm_find_in_path(const char *name, char *out, size_t cap)
{
    if (!name || !*name) return -1;
    const char *path = getenv("PATH");
    if (!path || !*path) return -1;
    char copy[8192];
    size_t n = strlen(path);
    if (n >= sizeof(copy)) return -1;
    memcpy(copy, path, n + 1);

    char *save = NULL;
#ifdef _WIN32
    const char *delim = ";";          /* Windows 的 PATH 用分号分隔 */
#else
    const char *delim = ":";
#endif
    for (char *seg = strtok_r(copy, delim, &save); seg;
         seg = strtok_r(NULL, delim, &save)) {
        if (nm_which(seg, name, out, cap) == 0) return 0;
    }
    return -1;
}

int nm_which(const char *dir, const char *name, char *out, size_t cap)
{
    if (!dir || !name || !*name) return -1;
    char cand[4096];
    if (nm_path_join(cand, sizeof(cand), dir, name) == 0 && nm_executable(cand)) {
        if (out && cap) {
            size_t n = strlen(cand);
            if (n >= cap) return -1;
            memcpy(out, cand, n + 1);
        }
        return 0;
    }
#ifdef _WIN32
    char with_ext[512];
    int k = snprintf(with_ext, sizeof(with_ext), "%s.exe", name);
    if (k > 0 && (size_t)k < sizeof(with_ext) &&
        nm_path_join(cand, sizeof(cand), dir, with_ext) == 0 && nm_executable(cand)) {
        if (out && cap) {
            size_t n = strlen(cand);
            if (n >= cap) return -1;
            memcpy(out, cand, n + 1);
        }
        return 0;
    }
#endif
    return -1;
}

int nm_spawn_wait(const char *exe, char *const argv[], int *exit_code)
{
    if (!exe || !argv || !argv[0]) return -1;
#ifdef _WIN32
    /* CreateProcess 要一整条可写的命令行；路径可能带空格（用户目录），
     * 所以参数逐个判断加引号。 */
    char cmd[32768];
    size_t off = 0;
    for (int i = 0; argv[i]; i++) {
        int quote = (strpbrk(argv[i], " \t") != NULL);
        int k = snprintf(cmd + off, sizeof(cmd) - off, "%s%s%s%s",
                         (i ? " " : ""), (quote ? "\"" : ""), argv[i], (quote ? "\"" : ""));
        if (k < 0 || (size_t)k >= sizeof(cmd) - off) return -1;
        off += (size_t)k;
    }
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    if (!CreateProcessA(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
        return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    if (!GetExitCodeProcess(pi.hProcess, &code)) code = (DWORD)-1;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (exit_code) *exit_code = (int)code;
    return 0;
#else
    fflush(NULL);                       /* fork 会复制缓冲区，先冲干净 */
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execv(exe, argv);
        _exit(127);                     /* 只有 execv 失败才会到这 */
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) { }
    if (!WIFEXITED(status)) return -1;
    if (exit_code) *exit_code = WEXITSTATUS(status);
    return 0;
#endif
}

/* ---------------------------------------------------------------------------
 * 常驻子进程 + 管道
 *
 * Windows：CreateProcess + CreatePipe，把写端接到子进程的 stdin。
 *   ★ 子进程的 stdout/stderr 必须**丢弃**（连到 NUL）——否则管道写满后
 *     子进程会阻塞在写 stdout 上，我们这边却还在等它读下一行 ⇒ 死锁。
 * POSIX：pipe + fork + dup2。
 * ------------------------------------------------------------------------- */
#ifdef _WIN32
struct NmProc {
    HANDLE  hProc;
    HANDLE  hThread;
    HANDLE  hIn;        /* 写端（我们的 stdin 管道） */
    int     closed;      /* stdin 已关 */
    DWORD   code;
};
#else
struct NmProc {
    pid_t pid;
    int   fd;           /* 写端 */
    int   closed;
    int   status;
};
#endif

NmProc *nm_proc_spawn(const char *exe, char *const argv[])
{
    if (!exe || !argv || !argv[0]) return NULL;
#ifdef _WIN32
    /* CreateProcess 要一整条可写的命令行；路径可能带空格（用户目录）。 */
    char cmd[32768];
    size_t off = 0;
    for (int i = 0; argv[i]; i++) {
        int quote = (strpbrk(argv[i], " \t") != NULL);
        int k = snprintf(cmd + off, sizeof(cmd) - off, "%s%s%s%s",
                         (i ? " " : ""), (quote ? "\"" : ""), argv[i], (quote ? "\"" : ""));
        if (k < 0 || (size_t)k >= sizeof(cmd) - off) return NULL;
        off += (size_t)k;
    }

    SECURITY_ATTRIBUTES sa;
    memset(&sa, 0, sizeof sa);
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;

    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return NULL;
    /* ★ 读端 rd **必须可继承** —— 它要作为子进程的 hStdInput 传下去
     *   （设成不可继承的话，子进程拿到的是无效 stdin ⇒ 立刻读到 EOF 退出，
     *   父进程随后 WriteFile 得 ERROR_NO_DATA(232)）。
     *   写端 wr 反过来必须**不可继承**：否则子进程里也有一份写端，
     *   管道永远不会收到 EOF，子进程的读循环就永远等不到结束。 */
    SetHandleInformation(wr, HANDLE_FLAG_INHERIT, 0);

    /* 子进程的 stdout/stderr 接到 NUL：防止管道写满造成死锁。
     * ★ NUL 也必须**不可继承**（bInheritHandle=FALSE）—— 它不是标准句柄，
     *   继承它只是泄漏句柄。 */
    HANDLE nul = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE | FILE_SHARE_READ,
                              NULL, OPEN_EXISTING, 0, NULL);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si); si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = rd;
    si.hStdOutput = nul;
    si.hStdError  = nul;

    if (!CreateProcessA(exe, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(rd); CloseHandle(wr);
        if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
        return NULL;
    }
    /* 父进程不需要读端与 nul；子进程也不需要写端。 */
    CloseHandle(rd);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);

    NmProc *p = (NmProc *)malloc(sizeof *p);
    if (!p) { CloseHandle(pi.hProcess); CloseHandle(pi.hThread); CloseHandle(wr); return NULL; }
    p->hProc = pi.hProcess;
    p->hThread = pi.hThread;
    p->hIn = wr;
    p->closed = 0;
    p->code = 0;
    return p;
#else
    int fds[2];
    if (pipe(fds) != 0) return NULL;
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return NULL; }
    if (pid == 0) {
        close(fds[1]);
        dup2(fds[0], STDIN_FILENO);
        close(fds[0]);
        /* stdout/stderr 丢到 /dev/null：防止管道写满造成死锁。 */
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) { dup2(dn, STDOUT_FILENO); dup2(dn, STDERR_FILENO); close(dn); }
        execv(exe, argv);
        _exit(127);
    }
    close(fds[0]);
    NmProc *p = (NmProc *)malloc(sizeof *p);
    if (!p) { close(fds[1]); kill(pid, SIGKILL); waitpid(pid, NULL, 0); return NULL; }
    p->pid = pid;
    p->fd = fds[1];
    p->closed = 0;
    p->status = 0;
    return p;
#endif
}

int nm_proc_write(NmProc *p, const char *text)
{
    if (!p || !text || p->closed) return -1;
#ifdef _WIN32
    DWORD n = 0;
    if (!WriteFile(p->hIn, text, (DWORD)strlen(text), &n, NULL)) return -1;
    return (int)n;
#else
    size_t len = strlen(text);
    size_t off = 0;
    while (off < len) {
        ssize_t k = write(p->fd, text + off, len - off);
        if (k <= 0) return -1;
        off += (size_t)k;
    }
    return (int)off;
#endif
}

int nm_proc_alive(NmProc *p)
{
    if (!p) return 0;
#ifdef _WIN32
    if (p->closed) return 0;
    return WaitForSingleObject(p->hProc, 0) == WAIT_TIMEOUT ? 1 : 0;
#else
    if (p->closed) return 0;
    int st = 0;
    pid_t r = waitpid(p->pid, &st, WNOHANG);
    return (r == 0) ? 1 : 0;
#endif
}

int nm_proc_close(NmProc *p)
{
    if (!p) return -1;
#ifdef _WIN32
    if (!p->closed) { CloseHandle(p->hIn); p->hIn = NULL; p->closed = 1; }
    WaitForSingleObject(p->hProc, INFINITE);
    DWORD code = 0;
    if (!GetExitCodeProcess(p->hProc, &code)) code = (DWORD)-1;
    CloseHandle(p->hProc);
    CloseHandle(p->hThread);
    p->code = (int)code;
    int rc = (int)code;
    free(p);
    return rc;
#else
    if (!p->closed) { close(p->fd); p->fd = -1; p->closed = 1; }
    int status = 0;
    while (waitpid(p->pid, &status, 0) < 0 && errno == EINTR) { }
    p->status = status;
    int rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    free(p);
    return rc;
#endif
}

uint64_t nm_now_ms(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    static int freq_ok = -1;
    if (freq_ok < 0) freq_ok = QueryPerformanceFrequency(&freq) ? 1 : 0;
    if (!freq_ok) return (uint64_t)GetTickCount64();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (uint64_t)((c.QuadPart * 1000) / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000u);
#endif
}

uint64_t nm_now_us(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    static int freq_ok = -1;
    if (freq_ok < 0) freq_ok = QueryPerformanceFrequency(&freq) ? 1 : 0;
    if (!freq_ok) return (uint64_t)GetTickCount64() * 1000u;
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (uint64_t)((c.QuadPart * 1000000) / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)(ts.tv_nsec / 1000u);
#endif
}

void nm_sleep_ms(unsigned ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts;
    ts.tv_sec  = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

double nm_peak_rss_mb(void)
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc;
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return 0;
    return (double)pmc.PeakWorkingSetSize / (1024.0 * 1024.0);
#elif defined(__APPLE__)
    struct rusage ru;                   /* macOS 的 ru_maxrss 是字节 */
    if (getrusage(RUSAGE_SELF, &ru) != 0) return 0;
    return (double)ru.ru_maxrss / (1024.0 * 1024.0);
#else
    struct rusage ru;                   /* Linux 的 ru_maxrss 是 KB */
    if (getrusage(RUSAGE_SELF, &ru) != 0) return 0;
    return (double)ru.ru_maxrss / 1024.0;
#endif
}

/* ======================= 语音播报（离线 TTS） =======================
 *
 * 抄写自 OsmAnd：nav-desktop/src/main/osmand/nav/route/Speech.java（2026-10-25）。
 *
 * ★ 为什么必须改成"常驻会话"：原来的实现每播一句就**新起一个 powershell.exe**，
 *   实测启动开销 0.5~1.5 秒。导航播报是**有 deadlines 的**（"前方 200 米左转"
 *   必须在车到路口前说出来），等 1 秒才出声，车早开过去了 —— 播报等于没有。
 *   OsmAnd 的做法是：起**一个**常驻 PowerShell 会话，之后逐行把文本喂给它的
 *   stdin，由它循环 Speak。这样每句的实际延迟只有合成时间（几十毫秒）。
 *
 * 结构（对齐 OsmAnd）：
 *   · 队列：所有文本先入队，后台线程**串行**朗读（OsmAnd: LinkedBlockingQueue）。
 *     串行很重要：两句同时播会互相盖掉，听感是"半句叠半句"。
 *   · 后端探测顺序抄 OsmAnd：Windows → SAPI 常驻会话；
 *     POSIX → spd-say > espeak-ng > espeak > pico2wave > festival。
 *   · 会话起不来 ⇒ 自动回退"每句一个进程"的旧模式（OsmAnd: sessionBroken）。
 *   · 任何异常都不往上传：导航照常工作，只是不出声（OsmAnd: catch Throwable）。
 * ------------------------------------------------------------------ */

/* ---- DRYRUN：无头自测用，把"发声"重定向到文件（确定性） ---- */
static int tts_dryrun_write(const char *text)
{
    const char *dry = getenv("NAVIMAP_TTS_DRYRUN");
    if (!dry || !*dry) return 0;
    FILE *f = fopen(dry, "ab");
    if (!f) return 0;
    fputs(text, f);
    fputc('\n', f);
    fclose(f);
    return 1;
}

/* ---- 单句模式（回退路径）：Windows 走临时 .ps1，POSIX 走命令行 TTS ---- */
static void tts_speak_once(const char *text)
{
#ifdef _WIN32
    char ps[1024];
    if (nm_find_in_path("powershell.exe", ps, sizeof(ps)) != 0) return;
    char tmpl[1024];
    int k = snprintf(tmpl, sizeof(tmpl), "%s/navimap_tts_%d_%llu.ps1",
                     nm_temp_dir(), (int)nm_get_pid(),
                     (unsigned long long)nm_now_ms());
    if (k < 0 || (size_t)k >= sizeof(tmpl)) return;
    FILE *f = fopen(tmpl, "wb");
    if (!f) return;
    static const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
    fwrite(bom, 1, 3, f);
    fputs("Add-Type -AssemblyName System.Speech\n", f);
    fputs("$s = New-Object System.Speech.Synthesis.SpeechSynthesizer\n", f);
    fputs("$s.Speak('", f);
    for (const char *p = text; *p; p++) {       /* 单引号转义为两个单引号 */
        if (*p == '\'') fputs("''", f);
        else            fputc(*p, f);
    }
    fputs("')\n", f);
    fclose(f);
    char *argv[] = { ps, "-NoProfile", "-NonInteractive", "-File", tmpl, NULL };
    int code = 0;
    nm_spawn_wait(ps, argv, &code);
    nm_unlink(tmpl);
#else
    char exe[1024];
    if (nm_find_in_path("spd-say", exe, sizeof(exe)) == 0) {
        char *argv[] = { exe, "-l", "zh", "-t", (char *)text, NULL };
        int c = 0; nm_spawn_wait(exe, argv, &c); return;
    }
    if (nm_find_in_path("espeak-ng", exe, sizeof(exe)) == 0) {
        char *argv[] = { exe, "-v", "zh", "-s", "160", (char *)text, NULL };
        int c = 0; nm_spawn_wait(exe, argv, &c); return;
    }
    if (nm_find_in_path("espeak", exe, sizeof(exe)) == 0) {
        char *argv[] = { exe, "-v", "zh", "-s", "160", (char *)text, NULL };
        int c = 0; nm_spawn_wait(exe, argv, &c); return;
    }
    if (nm_find_in_path("say", exe, sizeof(exe)) == 0) {
        char *argv[] = { exe, (char *)text, NULL };
        int c = 0; nm_spawn_wait(exe, argv, &c); return;
    }
#endif
    /* 没有任何引擎：静默（无头环境不报错） */
}

/* ---- 常驻会话（Windows）：一个 powershell 进程，逐行喂文本 ---- */
#ifdef _WIN32
static NmProc *g_sapi = NULL;      /* 常驻会话（NULL = 还没起或已坏） */
static char     g_sapi_script[1024]; /* 脚本文件路径（NULL 串 = 还没写） */

/* ★ 为什么用 `-File <脚本>` 而不是 `-Command <脚本>`：
 *   `-Command` 模式下 PowerShell **引擎自己也在读 stdin**（它要知道命令还有没有
 *   下一行），与脚本里的 `[Console]::In.ReadLine()` 抢同一个流 ⇒ 实测第一句能播、
 *   第二句卡死（引擎把第二行吞了）。
 *   `-File` 模式下引擎从**文件**读脚本，stdin 完全归脚本自己用 ⇒ 不抢。
 *   （OsmAnd 用的是 `-Command -`，它能工作是因为它的脚本是一行 while，
 *     引擎读脚本时只取一行；但那个写法仍然依赖引擎的读取时机，不稳。）
 *
 * 脚本内容抄 OsmAnd 的 SapiBackend.ensureSession：
 *   设 UTF8 编码（否则中文进管道按 ANSI 码页解码成乱码）、
 *   建 SpeechSynthesizer、选 zh-CN 声音、循环 ReadLine + Speak。 */
static const char *sapi_script_body(void)
{
    return
        "[Console]::InputEncoding = [System.Text.Encoding]::UTF8\n"
        "[Console]::OutputEncoding = [System.Text.Encoding]::UTF8\n"
        "Add-Type -AssemblyName System.Speech\n"
        "$v = New-Object System.Speech.Synthesis.SpeechSynthesizer\n"
        "$v.Rate = 1\n"
        "try { $v.SelectVoiceByHints([System.Speech.Synthesis.VoiceGender]::NotSet,"
        "[System.Speech.Synthesis.VoiceAge]::NotSet,0,"
        "[System.Globalization.CultureInfo]::new('zh-CN')) } catch { }\n"
        "while ($true) {\n"
        "  $l = [Console]::In.ReadLine()\n"
        "  if ($null -eq $l) { break }\n"
        "  if ($l -ne '') { try { $v.Speak($l) } catch { } }\n"
        "}\n";
}

/* 写脚本文件（UTF-8 BOM，否则 PowerShell 按 ANSI 读脚本本身也会乱）。 */
static int sapi_script_write(void)
{
    if (g_sapi_script[0]) return 1;
    int k = snprintf(g_sapi_script, sizeof g_sapi_script,
                     "%s/navimap_tts_session_%d.ps1", nm_temp_dir(), (int)nm_get_pid());
    if (k < 0 || (size_t)k >= sizeof g_sapi_script) { g_sapi_script[0] = '\0'; return 0; }
    FILE *f = fopen(g_sapi_script, "wb");
    if (!f) { g_sapi_script[0] = '\0'; return 0; }
    static const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
    fwrite(bom, 1, 3, f);
    fputs(sapi_script_body(), f);
    fclose(f);
    return 1;
}

static int sapi_session_start(void)
{
    if (g_sapi && nm_proc_alive(g_sapi)) return 1;
    if (g_sapi) { nm_proc_close(g_sapi); g_sapi = NULL; }

    char ps[1024];
    if (nm_find_in_path("powershell.exe", ps, sizeof(ps)) != 0) return 0;
    if (!sapi_script_write()) return 0;

    char *argv[] = { ps, "-NoProfile", "-NonInteractive", "-File", g_sapi_script, NULL };
    g_sapi = nm_proc_spawn(ps, argv);
    if (!g_sapi) return 0;
    /* 探测会话是否真的起来了：写一行空行，若管道已断则写失败。 */
    if (nm_proc_write(g_sapi, "\n") < 0) {
        nm_proc_close(g_sapi);
        g_sapi = NULL;
        return 0;
    }
    return 1;
}
#endif /* _WIN32 */

/* ---- POSIX 常驻会话：festival 从 stdin 读文本；其余是一次性的 ---- */
#ifndef _WIN32
static NmProc *g_festival = NULL;

static int festival_session_start(void)
{
    if (g_festival && nm_proc_alive(g_festival)) return 1;
    if (g_festival) { nm_proc_close(g_festival); g_festival = NULL; }
    char exe[1024];
    if (nm_find_in_path("festival", exe, sizeof(exe)) != 0) return 0;
    char *argv[] = { exe, "--tts", NULL };
    g_festival = nm_proc_spawn(exe, argv);
    return g_festival != NULL;
}
#endif

/* ---- 后端：发一句话 ---- */
static void tts_backend_speak(const char *text)
{
#ifdef _WIN32
    if (sapi_session_start()) {
        /* 逐行喂：一行一句。文本里的换行会把它拆成两句 ⇒ 先压成空格。 */
        char buf[1024];
        size_t k = 0;
        for (const char *p = text; *p && k + 1 < sizeof buf; p++) {
            buf[k++] = (*p == '\n' || *p == '\r') ? ' ' : *p;
        }
        buf[k] = '\0';
        if (nm_proc_write(g_sapi, buf) > 0 && nm_proc_write(g_sapi, "\n") > 0) return;
        /* 写失败 ⇒ 会话坏了 ⇒ 回退单句模式（OsmAnd: sessionBroken） */
        nm_proc_close(g_sapi);
        g_sapi = NULL;
    }
    tts_speak_once(text);
#else
    /* festival 会话是常驻进程：没起来就先尝试启动（无 festival 可执行文件时
     * 返回 0、g_festival 仍为 NULL，行为与原来一致，静默退回单句引擎）。
     * 原先漏了这步调用 ⇒ festival_session_start 成了死函数、且 Linux 下
     * festival 路径永远走不到（g_festival 恒为 NULL）。 */
    if (!g_festival) festival_session_start();
    if (g_festival && nm_proc_alive(g_festival)) {
        if (nm_proc_write(g_festival, text) > 0 && nm_proc_write(g_festival, "\n") > 0) return;
        nm_proc_close(g_festival);
        g_festival = NULL;
    }
    tts_speak_once(text);
#endif
}

/* ---- 队列 + 后台朗读线程（抄 OsmAnd 的 LinkedBlockingQueue + worker） ---- */
#define TTS_QUEUE_MAX 32

typedef struct TtsItem {
    char *text;
    struct TtsItem *next;
} TtsItem;

static struct {
    NmMutex *lock;
    NmCond  *not_empty;     /* 有文本可取 */
    NmCond  *not_full;      /* 队列有空间 */
    TtsItem *head, *tail;
    int      count;
    int      running;       /* worker 是否已起 */
    int      stop;
    int      offered;       /* 累计受理句数（观测口，见下） */
    int      delivered;     /* 累计已交付引擎句数（观测口，见下） */
    int      inflight;      /* worker 正在处理的那一句（0=无） */
} g_tts;

/* ★ 受理计数：计数点放在**入队之前**、调用线程内、无竞争。
 *   不能读队列长度 —— 后台线程会异步喝空队列，"入队后立刻读 size()"是
 *   与活消费者赛跑（OsmAnd 踩过同一个坑，见 Speech.OFFERED 的注释）。 */
int nm_tts_offered_count(void) { return g_tts.offered; }

/* ★ 已交付引擎计数：计数点在**把文本交给引擎之后**、后台线程内。
 *   与"受理"配对可断言"队列真的被消费了"—— 只看受理数证明不了 worker
 *   真的在跑（可能线程没起来）。
 *   ★ 注意它**不等于"已播完"**：常驻会话下"写入管道"就返回了，合成还在
 *     子进程那边进行中。要等真正播完用 nm_tts_flush()。 */
int nm_tts_delivered_count(void) { return g_tts.delivered; }

/* ★ 等待"已受理的句子全部播完"。实现：关掉常驻会话的写端 ⇒ 子进程读到
 *   EOF ⇒ 循环退出 ⇒ 子进程退出前会把当前句说完。于是"子进程退出"
 *   就是精确的"全部播完"边界。
 *   返回 0 = 已排空并播完；-1 = 没有会话可关（无引擎）。
 *   ★ 这是判定"播报真的出声了"的唯一可靠手段：常驻会话下写入即返回，
 *     看写入返回证明不了合成完成。 */
int nm_tts_flush(void)
{
    if (!g_tts.running) return -1;
    /* 先等队列排空（worker 把最后一句也交给引擎） */
    for (int i = 0; i < 2000; i++) {
        nm_mutex_lock(g_tts.lock);
        int empty = (g_tts.count == 0 && g_tts.inflight == 0);
        nm_mutex_unlock(g_tts.lock);
        if (empty) break;
        nm_sleep_ms(5);
    }
#ifdef _WIN32
    if (g_sapi) {
        nm_proc_close(g_sapi);      /* 关写端 ⇒ 子进程读到 EOF ⇒ 说完当前句后退出 */
        g_sapi = NULL;
    }
#else
    if (g_festival) {
        nm_proc_close(g_festival);
        g_festival = NULL;
    }
#endif
    return 0;
}

static void tts_queue_push(char *text)
{
    TtsItem *it = (TtsItem *)malloc(sizeof *it);
    if (!it) { free(text); return; }
    it->text = text;
    it->next = NULL;
    if (g_tts.tail) g_tts.tail->next = it;
    else            g_tts.head = it;
    g_tts.tail = it;
    g_tts.count++;
}

static char *tts_queue_pop(void)
{
    TtsItem *it = g_tts.head;
    if (!it) return NULL;
    g_tts.head = it->next;
    if (!g_tts.head) g_tts.tail = NULL;
    g_tts.count--;
    char *t = it->text;
    free(it);
    return t;
}

static void tts_worker(void *p)
{
    (void)p;
    for (;;) {
        nm_mutex_lock(g_tts.lock);
        while (g_tts.count == 0 && !g_tts.stop)
            nm_cond_wait(g_tts.not_empty, g_tts.lock);
        if (g_tts.count == 0 && g_tts.stop) { nm_mutex_unlock(g_tts.lock); break; }
        char *text = tts_queue_pop();
        g_tts.inflight = 1;          /* 取走即算"在飞"（flush 据此判断没排空） */
        nm_cond_signal(g_tts.not_full);
        nm_mutex_unlock(g_tts.lock);

        if (text) {
            tts_backend_speak(text);
            free(text);
            nm_mutex_lock(g_tts.lock);
            g_tts.delivered++;
            g_tts.inflight = 0;
            nm_cond_signal(g_tts.not_full);
            nm_mutex_unlock(g_tts.lock);
        }
    }
}

static int tts_queue_init(void)
{
    if (g_tts.running) return 1;
    g_tts.lock      = nm_mutex_create();
    g_tts.not_empty = nm_cond_create();
    g_tts.not_full  = nm_cond_create();
    if (!g_tts.lock || !g_tts.not_empty || !g_tts.not_full) return 0;
    g_tts.head = g_tts.tail = NULL;
    g_tts.count = 0;
    g_tts.stop = 0;
    g_tts.running = 1;
    if (nm_thread_spawn(tts_worker, NULL) != 0) { g_tts.running = 0; return 0; }
    return 1;
}

int nm_tts_speak(const char *text)
{
    if (!text || !*text) return 0;

    /* DRYRUN 同步写，避免起线程（自测要确定性） */
    if (tts_dryrun_write(text)) return 1;

    /* 本机没有可用引擎就不受理（无头环境静默） */
    if (!nm_tts_available()) return 0;

    if (!tts_queue_init()) {
        /* 起不了队列（内存不足等极端情况）⇒ 退回"每句一个进程"的旧行为。
         * 宁可慢也别不播：这是导航功能。 */
        tts_speak_once(text);
        return 1;
    }

    char *copy = nm_strdup(text);
    if (!copy) return 0;
    nm_mutex_lock(g_tts.lock);
    g_tts.offered++;
    /* 队列满 ⇒ 丢掉最老的（导航播报宁可少一句也不要延迟堆积） */
    while (g_tts.count >= TTS_QUEUE_MAX) {
        char *old = tts_queue_pop();
        if (!old) break;
        free(old);
    }
    tts_queue_push(copy);
    nm_cond_signal(g_tts.not_empty);
    nm_mutex_unlock(g_tts.lock);
    return 1;
}

int nm_tts_available(void)
{
#ifdef _WIN32
    char exe[1024];
    return nm_find_in_path("powershell.exe", exe, sizeof(exe)) == 0 ? 1 : 0;
#else
    char exe[1024];
    if (nm_find_in_path("spd-say", exe, sizeof(exe)) == 0) return 1;
    if (nm_find_in_path("espeak-ng", exe, sizeof(exe)) == 0) return 1;
    if (nm_find_in_path("espeak",   exe, sizeof(exe)) == 0) return 1;
    if (nm_find_in_path("festival", exe, sizeof(exe)) == 0) return 1;
    if (nm_find_in_path("say",      exe, sizeof(exe)) == 0) return 1;
    return 0;
#endif
}

/* 旧的固定两级阈值（60m / 300m）。
 * ★ **已弃用**：导航播报改走 route/voice.c 的"速度 × 秒数"模型
 *   （抄 OsmAnd 的 AnnounceTimeDistances），高速上固定 300 m 会来不及。
 *   这里保留是为了不破坏外部调用方与既有自测，新代码不要再用。 */
int nm_tts_tier_for_distance(double to_go_m)
{
    if (to_go_m <= 60.0)  return 2;   /* 立即层：即将转向 */
    if (to_go_m <= 300.0) return 1;   /* 接近层：提前预告 */
    return 0;
}

/* ============================ 其它 ============================ */

int nm_strcasecmp(const char *a, const char *b)
{
    if (!a || !b) return (a == b) ? 0 : (a ? 1 : -1);
#ifdef _WIN32
    return _stricmp(a, b);
#else
    return strcasecmp(a, b);
#endif
}

char *nm_strdup(const char *s)
{
    if (!s) return NULL;
#ifdef _WIN32
    return _strdup(s);
#else
    return strdup(s);
#endif
}

const char *nm_temp_dir(void)
{
    static char buf[1024];
    static int  done = 0;
    if (done) return buf;
    done = 1;
    const char *pick = NULL;
    static const char *envs[] = { "TMPDIR", "TEMP", "TMP" };
    for (size_t i = 0; i < sizeof(envs) / sizeof(envs[0]) && !pick; i++) {
        const char *e = getenv(envs[i]);
        if (e && *e && nm_is_dir(e)) pick = e;
    }
    if (!pick && nm_is_dir("/tmp")) pick = "/tmp";
    if (!pick) pick = ".";
    snprintf(buf, sizeof(buf), "%s", pick);
    return buf;
}

const char *nm_config_dir(void)
{
    static char buf[1024];
    static int  done = 0;
    if (done) return buf;
    done = 1;
#ifdef _WIN32
    const char *app = getenv("APPDATA");
    if (app && *app) snprintf(buf, sizeof(buf), "%s/navimap", app);
    else             snprintf(buf, sizeof(buf), ".");
#else
    const char *xdg  = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (xdg && *xdg)            snprintf(buf, sizeof(buf), "%s/navimap", xdg);
    else if (home && *home)     snprintf(buf, sizeof(buf), "%s/.config/navimap", home);
    else                        snprintf(buf, sizeof(buf), ".navimap");
#endif
    for (char *p = buf; *p; p++) if (*p == '\\') *p = '/';
    return buf;
}

int nm_setenv(const char *key, const char *val)
{
    if (!key || !*key || !val) return -1;
#ifdef _WIN32
    return _putenv_s(key, val) == 0 ? 0 : -1;
#else
    return setenv(key, val, 1) == 0 ? 0 : -1;
#endif
}

int nm_chmod_x(const char *path)
{
    if (!path) return -1;
#ifdef _WIN32
    return nm_file_exists(path) ? 0 : -1;
#else
    return chmod(path, 0755) == 0 ? 0 : -1;
#endif
}

/* ---- 字体候选链：内置 Noto 系列优先（与 OsmAnd 同源），系统字体兜底 ---- */

#if !defined(_WIN32) && !defined(__APPLE__)
static const char *kLinuxFonts[] = {
    "/usr/share/fonts/truetype/yozo/CESI_SS_GB18030.TTF",  /* CESI宋体  GB18030 */
    "/usr/share/fonts/fonts-cesi/CESI_SS_GB2312.TTF",      /* CESI宋体  GB2312  */
    "/usr/share/fonts/fonts-gb/GB_XBS_GB18030.ttf",        /* 国标小标宋        */
    "/usr/share/fonts/truetype/yozo/FZFSK.TTF",            /* 方正仿宋          */
    "/usr/share/fonts/fonts-gb/GB_HT_GB18030.ttf",         /* 国标黑体（兜底）  */
};
#endif

#ifdef _WIN32
/* %WINDIR% 形如 C:\WINDOWS；路径拼好后统一转成正斜杠（Win32 API 两种都收）。 */
static char        s_winfonts[5][600];
static const char *s_winptrs[5];

static int win_fonts_init(void)
{
    const char *wd = getenv("WINDIR");
    if (!wd || !*wd) wd = "C:/Windows";
    static const char *names[] = {
        "simsun.ttc",   /* 宋体（衬线，优先）   */
        "msyh.ttc",     /* 微软雅黑             */
        "simhei.ttf",   /* 黑体                 */
        "simfang.ttf",  /* 仿宋                 */
        "simkai.ttf",   /* 楷体                 */
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        int k = snprintf(s_winfonts[i], sizeof(s_winfonts[i]), "%s/Fonts/%s",
                         wd, names[i]);
        if (k < 0 || (size_t)k >= sizeof(s_winfonts[i])) return 0;
        for (char *p = s_winfonts[i]; *p; p++)
            if (*p == '\\') *p = '/';
        s_winptrs[i] = s_winfonts[i];
    }
    return 1;
}
#endif

#ifdef __APPLE__
static const char *kMacFonts[] = {
    "/System/Library/Fonts/Supplemental/Songti.ttc",   /* 宋体（衬线，优先） */
    "/System/Library/Fonts/STHeiti Medium.ttc",
    "/System/Library/Fonts/PingFang.ttc",
    "/Library/Fonts/Arial Unicode.ttf",                /* 覆盖面兜底         */
};
#endif

/* 内置 Noto 字体集（相对字体目录的文件名，按 per-glyph 回退顺序）：
 *   Latin/西里尔/希腊   → NotoSans-Regular（同时覆盖大量符号）
 *   粗体                → NotoSans-Bold
 *   CJK 中文/日文/韩文  → DroidSansFallback（TrueType；覆盖最广的 CJK 兜底）
 *   各文字系统          → NotoSans{SouthAsian,SoutheastAsian,Tibetan,NastaliqUrdu}
 *   阿拉伯/波斯         → Estedad
 *   兜底（覆盖最广）    → DroidSansFallback
 * 注：stb_truetype 只支持 glyf(TrueType) 轮廓，不支持 CFF(.otf/OTTO)，故原先的
 *   notocjk.otf(CFF) 不可用，CJK 改走 DroidSansFallback.ttf（也是 TrueType）。
 * font.c 打开时会逐个试这些文件组成字体集合；渲染时按码点选第一个含该字形的。
 * 顺序即优先级：拉丁先走 NotoSans（与 OsmAnd 一致），其余脚本走对应 Noto，
 * 最后 DroidSansFallback 接住任何漏网字形。 */
static const char *kBundledNoto[] = {
    "NotoSans-Regular.ttf",
    "NotoSans-Bold.ttf",
    "DroidSansFallback.ttf",
    "NotoSansSouthAsian-Regular.ttf",
    "NotoSansSoutheastAsian-Regular.ttf",
    "NotoSansTibetan-Regular.ttf",
    "NotoSansNastaliqUrdu-Regular.ttf",
    "Estedad-Regular.ttf",
    "DroidSansFallback.ttf",
};

/* 解析内置字体目录：env NAVMAP_FONT_DIR 优先；否则取 exe 所在目录下的
 * fonts/ 或 ../fonts/（与可执行文件同发，便于跨机器部署）。失败返回 -1。 */
static int get_font_dir(char *out, size_t cap)
{
    const char *e = getenv("NAVMAP_FONT_DIR");
    if (e && *e) { snprintf(out, cap, "%s", e); return 0; }
    char exep[1024];
    if (nm_exe_path(exep, sizeof(exep)) == 0) {
        char *end = exep + strlen(exep);
        while (end > exep && end[-1] != '/' && end[-1] != '\\') end--;
        if (end > exep) {
            *end = '\0';                       /* 剥到目录 */
            char tmp[1200];
            snprintf(tmp, sizeof(tmp), "%s/fonts", exep);
            NmDir *d = nm_dir_open(tmp);
            if (d) { nm_dir_close(d); snprintf(out, cap, "%s", tmp); return 0; }
            snprintf(tmp, sizeof(tmp), "%s/../fonts", exep);
            d = nm_dir_open(tmp);
            if (d) { nm_dir_close(d); snprintf(out, cap, "%s", tmp); return 0; }
        }
    }
    return -1;
}

/* 合并候选链：先内置 Noto（存在才加入），后系统字体兜底。首次调用缓存。 */
#define SYS_MAX 64
static char        s_cand_buf[SYS_MAX][1024];
static const char *s_cand_ptr[SYS_MAX];
static size_t      s_cand_n = 0;
static int         s_cand_init = 0;

const char *const *nm_font_candidates(size_t *n)
{
    if (!s_cand_init) {
        s_cand_init = 1;
        size_t k = 0;
        char fdir[1024];
        if (get_font_dir(fdir, sizeof(fdir)) == 0) {
            for (size_t i = 0; i < sizeof(kBundledNoto) / sizeof(kBundledNoto[0]) && k < SYS_MAX; i++) {
                snprintf(s_cand_buf[k], sizeof(s_cand_buf[k]), "%s/%s", fdir, kBundledNoto[i]);
                FILE *fp = fopen(s_cand_buf[k], "rb");
                if (fp) { fclose(fp); s_cand_ptr[k] = s_cand_buf[k]; k++; }
            }
        }
        /* 系统字体兜底（原平台列表） */
        const char *const *sys = NULL;
        size_t sysn = 0;
#if defined(_WIN32)
        if (win_fonts_init()) { sys = s_winptrs; sysn = sizeof(s_winptrs) / sizeof(s_winptrs[0]); }
#elif defined(__APPLE__)
        sys = kMacFonts; sysn = sizeof(kMacFonts) / sizeof(kMacFonts[0]);
#else
        sys = kLinuxFonts; sysn = sizeof(kLinuxFonts) / sizeof(kLinuxFonts[0]);
#endif
        for (size_t i = 0; i < sysn && k < SYS_MAX; i++)
            s_cand_ptr[k++] = sys[i];
        s_cand_n = k;
    }
    if (n) *n = s_cand_n;
    return s_cand_ptr;
}

/* ============================ 并发 ============================ */

#if defined(_WIN32)

struct NmMutex { HANDLE h; };

NmMutex *nm_mutex_create(void)
{
    HANDLE h = CreateMutexA(NULL, FALSE, NULL);
    if (!h) return NULL;
    NmMutex *m = (NmMutex *)malloc(sizeof *m);
    if (!m) { CloseHandle(h); return NULL; }
    m->h = h;
    return m;
}

void nm_mutex_destroy(NmMutex *m)
{
    if (!m) return;
    CloseHandle(m->h);
    free(m);
}

void nm_mutex_lock(NmMutex *m)
{
    if (!m) return;
    WaitForSingleObject(m->h, INFINITE);
}

void nm_mutex_unlock(NmMutex *m)
{
    if (!m) return;
    ReleaseMutex(m->h);
}

/* 条件变量：Windows 用**自动重置事件** + 一个"世代计数"实现
 * "signal 只唤醒一个等待者"的语义。
 *
 * ★★ 关键：世代计数 gen **由 NmMutex 保护**，不用独立的临界区。
 *   契约（与 POSIX 版一致）：调用 nm_cond_wait 时**必须已持有** m；
 *   nm_cond_signal/broadcast 也应当在持有 m 时调用。
 *   这样"读 gen"与"递增 gen"由同一把锁串起来 ⇒ 不会丢唤醒：
 *   等待者持锁读 g → 放锁 → 等事件；
 *   唤醒者要拿锁才能递增 gen ⇒ 必然发生在等待者放锁之后 ⇒ SetEvent 不会落空。
 *
 * ★★ 等待期间**必须放掉 m**。否则：等待者持 m 等事件，唤醒者要拿 m 才能
 *   signal ⇒ 互等死锁。这个 bug 实测过（首句能过、第二句卡死在入队）。
 *
 * 为什么不用纯事件量：自动重置事件被一个等待者唤醒后就复位，
 * 另一个等待者会一直等下去（"signal 只唤醒一个"是对的，但"广播"做不到）。
 * 世代计数让被"抢走"信号的等待者醒来后发现 gen 没变 ⇒ 继续等。 */
struct NmCond {
    HANDLE ev;        /* 自动重置事件 */
    LONG  gen;        /* 世代计数（由配对的 NmMutex 保护） */
    int   waiters;    /* 当前等待者数（broadcast 时要知道要唤醒几个） */
};

NmCond *nm_cond_create(void)
{
    NmCond *c = (NmCond *)malloc(sizeof *c);
    if (!c) return NULL;
    c->ev = CreateEventA(NULL, FALSE, FALSE, NULL);   /* 自动重置、初始未置位 */
    if (!c->ev) { free(c); return NULL; }
    c->gen = 0;
    c->waiters = 0;
    return c;
}

void nm_cond_destroy(NmCond *c)
{
    if (!c) return;
    CloseHandle(c->ev);
    free(c);
}

void nm_cond_wait(NmCond *c, NmMutex *m)
{
    if (!c) return;
    /* 调用方已持有 m：此时读 gen 是安全的（gen 由 m 保护）。 */
    LONG g = c->gen;
    c->waiters++;

    nm_mutex_unlock(m);                 /* ★ 等待期间必须放锁，否则与唤醒者死锁 */
    WaitForSingleObject(c->ev, INFINITE);
    nm_mutex_lock(m);

    /* 醒来后若 gen 没变 ⇒ 信号被别的等待者抢走了（signal 只唤醒一个）⇒ 继续等。 */
    while (c->gen == g) {
        nm_mutex_unlock(m);
        WaitForSingleObject(c->ev, INFINITE);
        nm_mutex_lock(m);
    }
    c->waiters--;
}

void nm_cond_signal(NmCond *c)
{
    if (!c) return;
    c->gen++;                           /* 由调用方持有的 NmMutex 保护 */
    SetEvent(c->ev);
}

void nm_cond_broadcast(NmCond *c)
{
    if (!c) return;
    c->gen++;                           /* 由调用方持有的 NmMutex 保护 */
    /* 自动重置事件一次只唤醒一个 ⇒ 按等待者数循环置位。
     * 世代计数保证其余等待者醒来后看到 gen 已变 ⇒ 不再阻塞。 */
    for (int i = 0; i < c->waiters; i++) SetEvent(c->ev);
}

struct NmThreadArg { void (*f)(void *); void *arg; };

static DWORD WINAPI nm_thread_tramp(LPVOID p)
{
    struct NmThreadArg *a = (struct NmThreadArg *)p;
    void (*f)(void *) = a->f;
    void *arg = a->arg;
    free(a);
    f(arg);
    return 0;
}

int nm_thread_spawn(void (*f)(void *), void *arg)
{
    struct NmThreadArg *a = (struct NmThreadArg *)malloc(sizeof *a);
    if (!a) return -1;
    a->f = f; a->arg = arg;
    HANDLE h = CreateThread(NULL, 0, nm_thread_tramp, a, 0, NULL);
    if (!h) { free(a); return -1; }
    CloseHandle(h);          /* 线程自己会跑完，句柄不必留着 */
    return 0;
}

#else /* POSIX */

struct NmMutex { pthread_mutex_t m; };

NmMutex *nm_mutex_create(void)
{
    NmMutex *m = (NmMutex *)malloc(sizeof *m);
    if (!m) return NULL;
    if (pthread_mutex_init(&m->m, NULL) != 0) { free(m); return NULL; }
    return m;
}

void nm_mutex_destroy(NmMutex *m)
{
    if (!m) return;
    pthread_mutex_destroy(&m->m);
    free(m);
}

void nm_mutex_lock(NmMutex *m)
{
    if (!m) return;
    pthread_mutex_lock(&m->m);
}

void nm_mutex_unlock(NmMutex *m)
{
    if (!m) return;
    pthread_mutex_unlock(&m->m);
}

struct NmCond { pthread_cond_t c; };

NmCond *nm_cond_create(void)
{
    NmCond *c = (NmCond *)malloc(sizeof *c);
    if (!c) return NULL;
    if (pthread_cond_init(&c->c, NULL) != 0) { free(c); return NULL; }
    return c;
}

void nm_cond_destroy(NmCond *c)
{
    if (!c) return;
    pthread_cond_destroy(&c->c);
    free(c);
}

void nm_cond_wait(NmCond *c, NmMutex *m)
{
    if (!c) return;
    /* 契约：调用方**已持有** m（pthread_cond_wait 自己管放锁/重获）。 */
    if (m) pthread_cond_wait(&c->c, &m->m);
    else {
        /* 没有锁可配：POSIX 不允许裸 wait，用一个局部锁兜住。 */
        pthread_mutex_t tmp;
        pthread_mutex_init(&tmp, NULL);
        pthread_cond_wait(&c->c, &tmp);
        pthread_mutex_destroy(&tmp);
    }
}

void nm_cond_signal(NmCond *c)
{
    if (!c) return;
    pthread_cond_signal(&c->c);
}

void nm_cond_broadcast(NmCond *c)
{
    if (!c) return;
    pthread_cond_broadcast(&c->c);
}

struct NmThreadArg { void (*f)(void *); void *arg; };

static void *nm_thread_tramp(void *p)
{
    struct NmThreadArg *a = (struct NmThreadArg *)p;
    void (*f)(void *) = a->f;
    void *arg = a->arg;
    free(a);
    f(arg);
    return NULL;
}

int nm_thread_spawn(void (*f)(void *), void *arg)
{
    struct NmThreadArg *a = (struct NmThreadArg *)malloc(sizeof *a);
    if (!a) return -1;
    a->f = f; a->arg = arg;
    pthread_t t;
    if (pthread_create(&t, NULL, nm_thread_tramp, a) != 0) { free(a); return -1; }
    pthread_detach(t);
    return 0;
}

#endif
