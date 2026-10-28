#ifndef NAVIMAP_NAMEPOOL_H
#define NAVIMAP_NAMEPOOL_H

#include <stdint.h>

/* 路名池（names.bin 的解析结果）。每省目录一份，name_id 只在本省内
 * 有意义，所以随 MapSrc 走。
 *
 * ★ 为什么住 map 层（2026-10-28 从 render/paint.h + render/labels.c 搬来）：
 *   它是**纯地图数据概念**——每省 names.bin 的解析结果，随 MapSrc 生命周期走，
 *   解析过程只用 map/binfmt.h 的 nmbin_open / nm_get_u32 / nmbin_close，零 render 依赖。
 *   之前它定义在 render/paint.h、实现落在 render/labels.c，导致 map/mapset.h 为了
 *   拿这个类型被迫反向 include render 的头 —— 形成 map→render 的**模块级倒置**，
 *   让 navmap_map 库名义上不依赖 render、实际却要 render 的符号才能链接。
 *   下沉到 map 后：map→render 边消失，render→map 仍是既有正确方向。
 *   （施工记录见 PLAN.md §53）
 *
 * payload 布局见 namepool.c 的解析。 */
typedef struct {
    uint32_t    n;
    uint32_t   *off;      /* n+1，off[id] → blob 内偏移 */
    char       *blob;
    int         ready;
} NamePool;

/* 打开/关闭路名池。names_path 指向该省的 names.bin。
 * 失败返回 -1，且 np 已被清成安全空态（可重复调用 name_pool_close）。 */
int  name_pool_open(NamePool *np, const char *names_path);
void name_pool_close(NamePool *np);

#endif
