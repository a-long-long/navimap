# 第 2 课 · 平台抽象层

**累加内容**：把"换个平台就不成立"的调用（休眠、时钟）100% 收口到一个 `plat.c` 里。
本课给出两个文件：`plat.h`/`plat.c`（承载 `#ifdef`）和 `main.c`（业务代码，**零 `#ifdef`**）。

**运行**：
```bash
gcc -std=c11 -O2 -o tut main.c plat.c && ./tut
```

**你会看到**：stdout 打印画线耗时（用了 `nm_now_ms`），随后生成 `out.bmp`。关键观察是
`main.c` 里**没有任何 `#ifdef _WIN32`**——平台差异被完全隔离了。

**为什么重要（项目铁律）**：
1. **业务代码零 `#ifdef _WIN32`**：所有"换平台就不成立"的调用只在 `plat.c` 出现。这是
   可移植性 / 依赖倒置的范本——你在 Windows 和 Linux 上编同一份业务代码。
2. **链接纪律**：真实项目里 `plat.c` 的线程走 pthread，所以 `-pthread` 由 `navmap_plat` 库
   统一承载；漏挂会在链接时报 `pthread_create` 未定义。本课的休眠/时钟不依赖 pthread，故无需。

**生产最终形态**：`src/utils/plat.c` 里的 `NmMutex`/`NmCond`/`nm_thread_spawn`/`nm_tts_speak`/
`nm_font_candidates` 都遵循同一收口原则。

**练习**：在 `plat.c` 里加一个 `nm_mkdir_p(const char*)`（Windows 用 `_mkdir`、POSIX 用 `mkdir`），
并在 `main.c` 里调用它创建一个目录——体会"只改 plat.c、业务侧零改动"。
