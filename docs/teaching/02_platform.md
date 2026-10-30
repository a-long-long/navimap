# 第 2 课：平台抽象层

> 模块：`src/utils/plat.{c,h}`、`src/utils/mem.{c,h}`、`src/utils/strpool.{c,h}`
> 前置：C 多线程（pthread 概念）、跨平台（POSIX vs Windows）差异常识。

## 一、教学目的

学完本课，学生应能：

1. 理解"平台差异 100% 收口到 `plat.c`"这条纪律为什么重要，并能在业务代码里坚持"零 `#ifdef _WIN32`"。
2. 在 `plat.c` 内新增一个跨平台小工具函数（如 `nm_sleep_ms`），而不触碰任何业务 `.c`。
3. 说清链接期 `-pthread` 为何由 `navmap_plat` 库统一承载（漏挂会在链接 `tts_selftest` 时报 `pthread_create` 未定义）。
4. 看懂 `nm_pin_allocator` 钉死 glibc mmap 阈值的根因：避免大数组改走 brk 主堆导致 SIGSEGV。
5. 掌握 `StrPool` 字符串去重池的设计（id 0 = "无名"），理解它如何支撑 L3 路名去重与 `names.bin` 落盘。

## 二、功能构成

| 文件 | 关键结构体 / 关键函数（声明） | 行号区间 |
|---|---|---|
| `plat.h` | 不透明 typedef：`NmMutex`/`NmCond`/`NmProc`/`NmDir` | `plat.h:72,81,90,132` |
| `plat.h` | `nm_thread_spawn` / `nm_proc_spawn` / `nm_now_ms` / `nm_tts_speak` / `nm_font_candidates` 声明 | `plat.h:99,135,145,156,196` |
| `plat.h` | `nm_map_file` / `nm_unmap_file` 声明 | `plat.h:66-67` |
| `plat.c` | `struct NmMutex` / `struct NmCond` / `struct NmProc` / `struct NmDir`（真实结构在 `#ifdef` 内） | `plat.c:351-360,1407,1454,571,1539,1568,1579` |
| `plat.c` | `nm_thread_spawn`（pthread / CreateThread 收口） | `plat.c:1526-1535,1623-1632` |
| `plat.c` | `nm_now_ms`（单调时钟） | `plat.c:733-748` |
| `plat.c` | `nm_map_file`（只读映射，POSIX mmap / Win CreateFileMapping） | `plat.c:296-328` |
| `plat.c` | `nm_font_candidates`（中文/系统字体候选链） | `plat.c:1372-1401` |
| `mem.h` | `NM_MMAP_THRESHOLD_BYTES` 宏、注释 | `mem.h:7-22` |
| `mem.c` | `nm_pin_allocator`（构造函数钉死分配器） | `mem.c:16-34` |
| `strpool.h` | `StrPool` 不透明类型与接口声明 | `strpool.h:9-23` |
| `strpool.c` | `struct StrPool`、`fnv1a`、`sp_intern` / `sp_get` / `sp_serialize` | `strpool.c:7-17,19-24,72-110,117-121,126-137` |

> 注：`nm_proc_spawn`、`nm_tts_speak` 的**声明**见 3.1；其完整实现（常驻子进程 + 管道、离线 TTS 队列与后台朗读线程，对应 `plat.c:587-1185`）见 3.2 节末尾的完整源码。

## 三、完整实现代码

### 3.1 `plat.h` 关键声明（typedef 与接口）

```h
/* 目录枚举。返回的条目名属于 NmDir，下次 nm_dir_next 前有效；
 * "." 与 ".." 已跳过。顺序由文件系统决定，需要可复现就自己排序
 * （mapset_load / places_build 都先收集再 qsort）。 */
typedef struct NmDir NmDir;
NmDir      *nm_dir_open(const char *path);
const char *nm_dir_next(NmDir *d);
void        nm_dir_close(NmDir *d);

/* 只读映射整个文件，长度写入 *out_len，返回基址；失败返回 NULL。
 * 空文件返回 NULL（与旧行为一致：size<=0 不当可映射）。 */
void *nm_map_file(const char *path, size_t *out_len);
void  nm_unmap_file(void *addr, size_t len);

/* 互斥锁（跨平台）。业务代码只调 nm_mutex_*，不直接碰 pthread / CRITICAL_SECTION。
 * 单一所有权：创建者负责 nm_mutex_destroy。锁内不要做阻塞/耗时操作。 */
typedef struct NmMutex NmMutex;
NmMutex *nm_mutex_create(void);
void     nm_mutex_destroy(NmMutex *m);
void     nm_mutex_lock(NmMutex *m);
void     nm_mutex_unlock(NmMutex *m);

/* 条件变量（跨平台）。用来做"生产者-消费者"队列：等待方必须已持有
 * nm_mutex_lock，nm_cond_wait 返回时**重新持有**该锁。
 * 单一所有权：创建者负责 nm_cond_destroy。 */
typedef struct NmCond NmCond;
NmCond  *nm_cond_create(void);
void     nm_cond_destroy(NmCond *c);
void     nm_cond_wait(NmCond *c, NmMutex *m);   /* 释放锁 → 等信号 → 重获锁 */
void     nm_cond_signal(NmCond *c);              /* 唤醒一个等待者 */
void     nm_cond_broadcast(NmCond *c);           /* 唤醒全部等待者 */

/* 起一个脱离式（detached）线程跑 f(arg)。成功返回 0，失败返回 -1。
 * 线程自行结束，调用方不 join；f 内部自行处理自己的资源。 */
int nm_thread_spawn(void (*f)(void *), void *arg);

/* 常驻子进程 + 管道（跨平台）
 *
 * 用途：TTS 后端要**一次启动、多次喂文本**。每句都起一个进程的话，
 * Windows 上 PowerShell 启动要 0.5~1.5 秒 —— 导航播报会因此错过时机
 * （车都开过路口了声音才出来）。所以起一个常驻子进程，把它的 stdin
 * 接一条管道，逐行喂文本。
 *
 * 用法：spawn → 反复 write → close（关管道让子进程看到 EOF 并退出）。
 * 单一所有权：创建者负责 nm_proc_close。 */
typedef struct NmProc NmProc;
/* 起常驻子进程并把它的 stdin 接上管道。成功返回句柄，失败返回 NULL。
 * argv 必须以 NULL 结尾；argv[0] 是程序路径。 */
NmProc *nm_proc_spawn(const char *exe, char *const argv[]);
/* 往子进程 stdin 写一段文本（不追加换行，调用方自己决定）。
 * 返回写入字节数；失败返回 -1。 */
int     nm_proc_write(NmProc *p, const char *text);
/* 关闭 stdin 并等子进程退出，释放句柄。p 为 NULL 时什么都不做。
 * 返回子进程退出码；取不到返回 -1。 */
int     nm_proc_close(NmProc *p);
/* 子进程是否还活着。p 为 NULL 或已退出返回 0。 */
int     nm_proc_alive(NmProc *p);

uint64_t nm_now_ms(void);             /* 单调时钟，毫秒 */
uint64_t nm_now_us(void);             /* 单调时钟，微秒（基准测试要亚毫秒精度） */
void     nm_sleep_ms(unsigned ms);
double   nm_peak_rss_mb(void);        /* 峰值驻留内存（MB） */

/* ---------------- 语音播报（离线 TTS） ----------------
 * 跨平台离线发声：Windows 走 PowerShell + System.Speech（SAPI），
 * 其它平台按 PATH 探测 espeak-ng / espeak / say。真正的发声在分离线程里
 * 做（nm_thread_spawn），调用方（导航每帧推进）绝不被阻塞。
 * 设 NAVIMAP_TTS_DRYRUN=文件路径 时只把文本写进该文件、不真正发声，
 * 用于无头自测（本机没有音频设备 / 没有 TTS 引擎时也能断言集成是否正确）。 */
int  nm_tts_speak(const char *text);          /* 返回 1=已提交(或 dryrun 已写) / 0=无引擎 */

/* 逐平台字体候选链（宋体优先，理由见 render/font.c 的注释）。
 * 返回的数组与字符串都是静态存储，勿 free。 */
const char *const *nm_font_candidates(size_t *n);
```

### 3.2 `plat.c` 实现

**文件只读映射 `nm_map_file`（POSIX 用 `mmap`，Windows 用 `CreateFileMapping` + `MapViewOfFile`）：**

```c
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
```

**单调时钟 `nm_now_ms`（POSIX 用 `clock_gettime(CLOCK_MONOTONIC)`，Windows 用 `QueryPerformanceCounter`）：**

```c
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
```

**并发层：不透明结构与 `nm_thread_spawn` 的 pthread / CreateThread 双实现（平台差异收口于此）。**

Windows 分支：

```c
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
 * "signal 只唤醒一个等待者"的语义。 */
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
    LONG g = c->gen;
    c->waiters++;
    nm_mutex_unlock(m);                 /* ★ 等待期间必须放锁，否则与唤醒者死锁 */
    WaitForSingleObject(c->ev, INFINITE);
    nm_mutex_lock(m);
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
```

POSIX 分支（pthread 收口）：

```c
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
```

**`NmDir` 与 `NmProc` 的真实结构（同样只在 `plat.c` 内可见）：**

```c
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
```

**字体候选链 `nm_font_candidates`（首次调用缓存，先内置 Noto 后系统字体兜底）：**

```c
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
```

> 完整源码 · `src/utils/plat.c:587-1185`（常驻子进程 + 管道、离线 TTS 队列与后台朗读线程）

```c
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
```

### 3.3 `mem.h` 与 `mem.c`（钉死 glibc 分配器）

```h
/* ---------------------------------------------------------------------------
 * 本机（UOS 20 / glibc 2.28.34-deepin1 / aarch64）实测：
 *   glibc 会动态调整 mmap_threshold —— 每释放一个 mmap 大块，阈值就被抬到该块大小。
 *   于是之后十几 MB 的数组不再走 mmap，改从 brk 主堆分配，而这条路径在本机不可靠，
 *   表现为某个函数里出现"不可能的取值"并 SIGSEGV（上一版 NavMap 正是崩在
 *   append_block_to_map）。进程启动时钉死分配器即可，与具体取值无关，
 *   "显式设置这两个参数"这件事本身才是关键（置上 no_dyn_threshold）。
 * ------------------------------------------------------------------------- */
#define NM_MMAP_THRESHOLD_BYTES (128 * 1024)

/* 只读文件映射；返回映射地址，长度写入 *out_len。失败返回 NULL。
 * 实现走平台层（utils/plat.c）：POSIX mmap / Windows CreateFileMapping。 */
void *nm_mmap_file(const char *path, size_t *out_len);

/* 解除映射（Windows 上还要关掉映射句柄，所以地址和长度都要原样传回）。 */
int nm_munmap(void *addr, size_t len);
```

```c
#include "utils/mem.h"
#include "utils/plat.h"

#include <malloc.h>
#include <stdio.h>

/* 必须在任何业务分配之前生效，所以用构造函数（优先级 101，早于大多数库）。
 *
 * ★ 只在"参数确实存在"的平台编译：mallopt/M_MMAP_THRESHOLD 是 glibc 的
 *   （musl / macOS / Windows 没有或语义不同），__attribute__((constructor))
 *   是 GCC/Clang 的（MSVC 没有）。两个条件都不满足时整段消失——不能留个
 *   空函数，-Wall -Wextra 会把未使用的 static 函数当警告（零警告门禁）。
 *
 * 映射本身走平台层（nm_map_file/nm_unmap_file）：POSIX 是 mmap，
 * Windows 是 CreateFileMapping + MapViewOfFile。 */
#if !defined(_WIN32) && defined(M_MMAP_THRESHOLD) && defined(__GNUC__)
__attribute__((constructor(101)))
static void nm_pin_allocator(void)
{
    mallopt(M_MMAP_THRESHOLD, NM_MMAP_THRESHOLD_BYTES);
    mallopt(M_TRIM_THRESHOLD, -1);
}
#endif

void *nm_mmap_file(const char *path, size_t *out_len)
{
    return nm_map_file(path, out_len);
}

int nm_munmap(void *addr, size_t len)
{
    nm_unmap_file(addr, len);
    return 0;
}
```

### 3.4 `strpool.h` 与 `strpool.c`（字符串去重池，id 0 = 无名）

```h
/* 字符串池：路名去重。id 0 保留 = "无名"。 */

typedef struct StrPool StrPool;

StrPool *sp_create(size_t cap_hint);
void     sp_destroy(StrPool *p);

/* 加入字符串并返回 id（已存在则复用）。空串/NULL 返回 0。 */
uint32_t sp_intern(StrPool *p, const char *s, size_t len);
uint32_t sp_intern_cstr(StrPool *p, const char *s);

const char *sp_get(StrPool *p, uint32_t id);
size_t      sp_count(StrPool *p);        /* 不含 id 0 */
size_t      sp_bytes(StrPool *p);        /* 字符区字节数（含结尾 0） */

/* 序列化成 [u32 count][u32 off[count+1]][bytes]，供 names.bin 落盘 */
void sp_serialize(StrPool *p, uint8_t **out, size_t *out_len);
```

```c
#include "utils/strpool.h"
#include <stdlib.h>
#include <string.h>

#define SP_EMPTY_ID 0u

struct StrPool {
    char    *blob;
    size_t   blob_len, blob_cap;

    uint32_t *off;        /* id → blob 内偏移（id 从 1 开始；off[0] = blob_len） */
    size_t    n, cap;

    /* 哈希：blob 偏移 → id。开放寻址，容量取 2 的幂。 */
    int32_t  *tab;
    size_t    tab_mask;
};

static uint64_t fnv1a(const char *s, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 1099511628211ull; }
    return h;
}

StrPool *sp_create(size_t cap_hint)
{
    StrPool *p = calloc(1, sizeof(StrPool));
    if (!p) return NULL;
    p->blob_cap = cap_hint ? cap_hint : 1u << 16;
    p->cap      = 1024;
    p->blob     = malloc(p->blob_cap);
    p->off      = malloc(p->cap * sizeof(uint32_t));
    p->tab_mask = 2047;
    p->tab      = malloc((p->tab_mask + 1) * sizeof(int32_t));
    if (!p->blob || !p->off || !p->tab) { sp_destroy(p); return NULL; }
    memset(p->tab, 0xff, (p->tab_mask + 1) * sizeof(int32_t));
    p->n = 1;              /* id 0 = 空串 */
    p->off[0] = 0;
    p->blob_len = 1;       /* blob[0] = '\0' */
    p->blob[0] = '\0';
    return p;
}

void sp_destroy(StrPool *p)
{
    if (!p) return;
    free(p->blob); free(p->off); free(p->tab); free(p);
}

uint32_t sp_intern(StrPool *p, const char *s, size_t len)
{
    if (!s || len == 0) return SP_EMPTY_ID;

    uint64_t h = fnv1a(s, len);
    size_t j = (size_t)(h & p->tab_mask);
    while (p->tab[j] >= 0) {
        uint32_t id = (uint32_t)p->tab[j];
        const char *e = p->blob + p->off[id];
        if (strlen(e) == len && memcmp(e, s, len) == 0) return id;
        j = (j + 1) & p->tab_mask;
    }

    /* 新串 */
    if (p->blob_len + len + 1 > p->blob_cap) {
        size_t ncap = p->blob_cap;
        while (ncap < p->blob_len + len + 1) ncap *= 2;
        char *nb = realloc(p->blob, ncap);
        if (!nb) return SP_EMPTY_ID;
        p->blob = nb; p->blob_cap = ncap;
    }
    if (p->n + 2 > p->cap) {   /* 还要写 off[n+1]，多留一格 */
        size_t ncap = p->cap * 2;
        uint32_t *no = realloc(p->off, ncap * sizeof(uint32_t));
        if (!no) return SP_EMPTY_ID;
        p->off = no; p->cap = ncap;
    }
    uint32_t id = (uint32_t)p->n;
    p->off[id] = (uint32_t)p->blob_len;
    memcpy(p->blob + p->blob_len, s, len);
    p->blob_len += len;
    p->blob[p->blob_len++] = '\0';
    p->off[p->n + 1] = (uint32_t)p->blob_len;   /* 便于取长度 */
    p->n++;

    p->tab[j] = (int32_t)id;
    sp_grow_tab(p);
    return id;
}

uint32_t sp_intern_cstr(StrPool *p, const char *s)
{
    return s ? sp_intern(p, s, strlen(s)) : SP_EMPTY_ID;
}

const char *sp_get(StrPool *p, uint32_t id)
{
    if (!p || id == 0 || id >= p->n) return "";
    return p->blob + p->off[id];
}

size_t sp_count(StrPool *p) { return p ? p->n - 1 : 0; }
size_t sp_bytes(StrPool *p) { return p ? p->blob_len : 0; }

void sp_serialize(StrPool *p, uint8_t **out, size_t *out_len)
{
    size_t cnt = p->n;                      /* 含 id 0 */
    size_t len = 4 + 4 * (cnt + 1) + p->blob_len;
    uint8_t *buf = malloc(len);
    if (!buf) { *out = NULL; *out_len = 0; return; }
    uint32_t *u32 = (uint32_t *)buf;
    u32[0] = (uint32_t)cnt;
    for (size_t i = 0; i <= cnt; i++) u32[1 + i] = p->off[i];
    memcpy(buf + 4 + 4 * (cnt + 1), p->blob, p->blob_len);
    *out = buf; *out_len = len;
}
```

## 四、与其他功能之间的联系

- **被几乎所有模块使用**：`graph_build.c`/`route.c`/`paint.c`/`ui_qt/*` 只调 `nm_*` 接口。`nm_font_candidates` 被 `render/font.c` 组建字体集；`NmMutex`/`NmCond` 支撑 TTS 后台朗读线程与并发加载；`nm_map_file`（经 `nm_mmap_file`）是所有大文件（nodes.bin / 边表）只读映射的底座。
- **与 L1 / L3 交汇**：`StrPool` 是 L3 PBF 路名去重后的落盘容器（`names.bin`，`sp_serialize` 输出 `[u32 count][u32 off[count+1]][bytes]`）；`nm_now_ms`/`nm_thread_spawn` 在 L4 / L5 的并行建图与瓦片加载中反复出现。
- **与 L11 导航语音**：`nm_tts_speak` / `nm_tts_flush` 依赖 `nm_thread_spawn` + `NmMutex`/`NmCond` 实现后台串行朗读线程，调用方（导航每帧推进）绝不被阻塞。
- **链接纪律**：`plat.c` 的线程用 pthread 实现，故 `-pthread` 由 `navmap_plat` 库统一承载（`CMakeLists.txt` 相关目标）。漏挂会在链接 `tts_selftest` 时报 `pthread_create` 未定义。
- **与 L13 架构门禁**：业务代码零 `#ifdef _WIN32` 是跨平台门禁的验收点之一（`cross-platform-portability-audit` 思路）。

## 五、演示、练习与验收

- **演示**：运行 `./build_rel/plat_selftest`。它全接口自验收（路径、文件映射、互斥/条件变量、线程、时间、字体链、TTS dryrun 等），正反双向断言契约。
- **练习**：在 `plat.c` 里加一个 `nm_sleep_ms`（已存在，可仿写 `nm_now_us` 之类的新工具），并在一个 selftest 里调用它——全程不碰任何业务 `.c`。验证 `-pthread` 只在 `navmap_plat` 挂一次：故意在别的库漏掉，看链接 `tts_selftest` 报 `pthread_create` 未定义。
- **阅读验证**：直接读源码确认——`nm_thread_spawn` 在 POSIX 下调 `pthread_create` 并 `pthread_detach`；`nm_pin_allocator` 用 `constructor(101)` 钉死 `M_MMAP_THRESHOLD=128*1024`；`sp_intern` 对空串/NULL 返回 `SP_EMPTY_ID`（0），`sp_get` 对 id 0 / 越界返回 `""`。
- **验收**：能在不碰业务代码的前提下新增一个跨平台小工具函数，并解释 `-pthread` 为何只在 `navmap_plat` 挂一次、`nm_pin_allocator` 为何要钉死分配器、`StrPool` 为何 id 0 固定为"无名"。
