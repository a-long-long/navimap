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
    struct _stati64 st;
    if (_stati64(path, &st) != 0) return -1;
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
        const char *name = d->fd.cFileName;
        d->pending = FindNextFileA(d->h, &d->fd) ? 1 : 0;
        if (strcmp(name, ".") != 0 && strcmp(name, "..") != 0) return name;
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

/* ---- 字体候选链：宋体优先，理由见 render/font.c ---- */

static const char *kLinuxFonts[] = {
    "/usr/share/fonts/truetype/yozo/CESI_SS_GB18030.TTF",  /* CESI宋体  GB18030 */
    "/usr/share/fonts/fonts-cesi/CESI_SS_GB2312.TTF",      /* CESI宋体  GB2312  */
    "/usr/share/fonts/fonts-gb/GB_XBS_GB18030.ttf",        /* 国标小标宋        */
    "/usr/share/fonts/truetype/yozo/FZFSK.TTF",            /* 方正仿宋          */
    "/usr/share/fonts/fonts-gb/GB_HT_GB18030.ttf",         /* 国标黑体（兜底）  */
};

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

const char *const *nm_font_candidates(size_t *n)
{
#if defined(_WIN32)
    if (!win_fonts_init()) { if (n) *n = 0; return NULL; }
    if (n) *n = sizeof(s_winptrs) / sizeof(s_winptrs[0]);
    return s_winptrs;
#elif defined(__APPLE__)
    if (n) *n = sizeof(kMacFonts) / sizeof(kMacFonts[0]);
    return kMacFonts;
#else
    if (n) *n = sizeof(kLinuxFonts) / sizeof(kLinuxFonts[0]);
    return kLinuxFonts;
#endif
}
