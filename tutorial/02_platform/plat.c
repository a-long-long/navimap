/* tutorial/02_platform/plat.c — 平台差异 100% 收口到这里（唯一的 #ifdef 所在） */
#include "plat.h"

#ifdef _WIN32
#  include <windows.h>
void nm_sleep_ms(int ms) { Sleep((DWORD)ms); }
long long nm_now_ms(void) { return (long long)GetTickCount64(); }

#else /* POSIX */
#  include <time.h>
#  include <unistd.h>
void nm_sleep_ms(int ms) { usleep((useconds_t)ms * 1000); }
long long nm_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

#endif
