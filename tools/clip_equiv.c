/* clip_equiv — 验证自写光栅的线段裁剪与 SDL_IntersectRectAndLine 逐像素等价。
 *
 * 为什么要它：raster.h 写着"与 SDL 软渲染器逐像素等价是硬要求"。去 SDL 化
 * 时把 SDL_IntersectRectAndLine 换成了自写的 raster_clip_line（ Cohen-
 * Sutherland ），**但两者不是同一个算法**：SDL 那份是参数化裁剪，交点位置
 * 的取整方式不同 ⇒ 线段跨过画布边界时被裁到**不同像素**。
 *
 * 症状：整幅图只差 0.07%，且差异**全部集中在画布最外几圈**（x=0..2 /
 * x=w-3..w-1 / y=0..5 / y=h-6..h-1），地图内部 0 差异 —— 正是"只在裁剪
 * 发生时才有差别"的特征。
 *
 * 做法：随机线段（大量跨越边界），两条裁剪各跑一遍，比裁剪后的端点。
 * 判据：端点必须逐个完全相同。
 */
#include <SDL.h>
#include "render/raster.h"

#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 2;
    }

    const int W = 800, H = 600;
    Raster rs;
    raster_bind_mem(&rs, NULL, 1, 1, 1, 0, 0, 0, 0);  /* 只借 clip 字段 */
    SDL_Rect clip = { 0, 0, W, H };

    unsigned seed = 12345;
    #define RND(n) (seed = seed * 1103515245u + 12345u, (int)((seed >> 16) % (unsigned)(n)))

    int total = 0, mismatch = 0;
    int shown = 0;
    for (int i = 0; i < 200000; i++) {
        int x1 = RND(W + 200) - 100;
        int y1 = RND(H + 200) - 100;
        int x2 = RND(W + 200) - 100;
        int y2 = RND(H + 200) - 100;

        /* SDL 版 */
        int ax = x1, ay = y1, bx = x2, by = y2;
        int sdl_in = SDL_IntersectRectAndLine(&clip, &ax, &ay, &bx, &by);

        /* 自写版 */
        rs.clip.x = 0; rs.clip.y = 0; rs.clip.w = W; rs.clip.h = H;
        int cx = x1, cy = y1, dx = x2, dy = y2;
        int our_in = raster_clip_line(&rs, &cx, &cy, &dx, &dy);

        total++;
        if (sdl_in != our_in || ax != cx || ay != cy || bx != dx || by != dy) {
            mismatch++;
            if (shown < 8) {
                printf("  线段 (%d,%d)-(%d,%d): SDL(in=%d)(%d,%d)-(%d,%d)"
                       "  ours(in=%d)(%d,%d)-(%d,%d)\n",
                       x1, y1, x2, y2, sdl_in, ax, ay, bx, by,
                       our_in, cx, cy, dx, dy);
                shown++;
            }
        }
    }
    printf("compare        %d\n", total);
    printf("mismatch       %d  (%.4f%%)\n", mismatch,
           100.0 * mismatch / (double)(total ? total : 1));
    if (mismatch == 0) {
        printf("CLIP-EQUIV-OK（自写裁剪与 SDL_IntersectRectAndLine 完全一致）\n");
        SDL_Quit();
        return 0;
    }
    printf("CLIP-EQUIV-FAIL：裁剪结果与 SDL 不一致\n");
    SDL_Quit();
    return 1;
}
