/* tutorial/02_platform/main.c — 第 2 课 · 平台抽象层
 *
 * 累加内容：把"换个平台就不成立"的调用（休眠、时钟）全部收口到 plat.c。
 * 本文件是"业务代码"——注意：全文【没有】任何 #ifdef _WIN32。
 *
 * 编译： gcc -std=c11 -O2 -o tut main.c plat.c && ./tut
 */
#include "../common.h"
#include "plat.h"

#define W 320
#define H 240
static uint32_t buf[W * H];

int main(void) {
    tut_cls(buf, W, H, 0xFFF4F1EA);

    /* 业务代码只用 nm_now_ms / nm_sleep_ms，不知道底下是 Sleep 还是 usleep */
    long long t0 = nm_now_ms();
    for (int i = 0; i < W; i += 4)
        tut_line(buf, W, H, i, 0, i, H - 1, 0xFFD8D2C8);
    long long t1 = nm_now_ms();
    printf("画 %d 条竖线耗时 %lld ms\n", W / 4, t1 - t0);

    /* 跨平台休眠：业务侧零 #ifdef */
    nm_sleep_ms(50);

    tut_line(buf, W, H, 0, H / 2, W - 1, H / 2, 0xFF2A2A2A);
    tut_line(buf, W, H, W / 2, 0, W / 2, H - 1, 0xFF2A2A2A);

    if (tut_write_bmp("out.bmp", buf, W, H) != 0) { fprintf(stderr, "write failed\n"); return 1; }
    printf("OK: wrote out.bmp — 注意 main.c 里没有任何 #ifdef\n");
    return 0;
}
