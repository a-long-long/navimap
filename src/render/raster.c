#include "render/raster.h"

/* 位偏移：把 r/g/b 按 surface 的 mask 拼成一个 32 位字。
   本机 SDL_CreateRGBSurface(0,w,h,32,0,0,0,0) 给的是 SDL_PIXELFORMAT_RGB888
   （Rmask 0x00ff0000、Amask 0），所以拼出来是 0x00RRGGBB，与 SDL_MapRGBA
   的结果一致（Amask=0 时 alpha 分量贡献 0）。 */
static int shift_of(uint32_t mask)
{
    if (!mask) return 0;
    int s = 0;
    while (!((mask >> s) & 1)) s++;
    return s;
}

int raster_bind_mem(Raster *rs, void *px, int w, int h, size_t stride,
                    uint32_t rmask, uint32_t gmask, uint32_t bmask, uint32_t amask)
{
    if (!px || w <= 0 || h <= 0) return -1;
    rs->px = (uint32_t *)px;
    rs->w = w;
    rs->h = h;
    rs->stride = stride;
    rs->clip.x = 0; rs->clip.y = 0; rs->clip.w = w; rs->clip.h = h;
    rs->amask = amask;
    rs->rmask = rmask; rs->gmask = gmask; rs->bmask = bmask;
    rs->rsh = (uint8_t)shift_of(rmask);
    rs->gsh = (uint8_t)shift_of(gmask);
    rs->bsh = (uint8_t)shift_of(bmask);
    return 0;
}

int raster_bind(Raster *rs, SDL_Surface *s)
{
    if (!s || !s->pixels || !s->format) return -1;
    if (s->format->BitsPerPixel != 32 || s->format->BytesPerPixel != 4) return -1;
    if (s->pitch % 4) return -1;
    /* 需要锁的表面（RLE/硬件）不能直接写像素 */
    if (SDL_MUSTLOCK(s)) return -1;

    return raster_bind_mem(rs, s->pixels, s->w, s->h, (size_t)(s->pitch / 4),
                           s->format->Rmask, s->format->Gmask,
                           s->format->Bmask, s->format->Amask);
}
