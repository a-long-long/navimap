#include "map/namepool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "map/binfmt.h"

/* 路名池（每省一份）的解析。
 *
 * names.bin 是 NAVMBIN 包裹的（128B 头 + 字段表），payload 布局：
 *   u32 count
 *   u32 off[count+1]      —— 每条路名在 blob 里的字节偏移
 *   char blob[...]        —— 全部路名，NUL 分隔
 * 解析结果是 NamePool：off 表 + 一整块 blob（省内存、避免逐名 strdup）。
 *
 * ⚠️ 注释里不要嵌套块注释：内层的结束标记会提前闭合外层，后面的中文会被
 * 当成代码（编译器报 stray '\274' 之类）。要分段就用多个单行注释。 */

/* 2026-10-28 从 render/labels.c 搬来（PLAN.md §53 R1）。
 * 搬动理由：实现只用 map/binfmt.h 的 nmbin_*，属 map 层；留在 render 会让
 * map/mapset.h 为了拿 NamePool 反向 include render/ ⇒ 模块级倒置。 */

int name_pool_open(NamePool *np, const char *names_path)
{
    memset(np, 0, sizeof(*np));

    NMBinFile nf;
    if (nmbin_open(names_path, &nf) != 0) return -1;

    const uint8_t *d = nf.data;
    uint32_t cnt = nm_get_u32(d);
    size_t offs_bytes = (size_t)(cnt + 1) * 4;
    if (nf.data_len < 4 + offs_bytes) { nmbin_close(&nf); return -1; }

    np->n = cnt;
    np->off = calloc(cnt + 1, sizeof(uint32_t));
    np->blob = malloc(nf.data_len - 4 - offs_bytes + 1);
    if (!np->off || !np->blob) {
        name_pool_close(np);
        nmbin_close(&nf);
        return -1;
    }
    for (uint32_t i = 0; i <= cnt; i++)
        np->off[i] = nm_get_u32(d + 4 + (size_t)i * 4);
    memcpy(np->blob, d + 4 + offs_bytes, nf.data_len - 4 - offs_bytes);
    np->blob[nf.data_len - 4 - offs_bytes] = '\0';

    nmbin_close(&nf);
    np->ready = 1;
    return 0;
}

void name_pool_close(NamePool *np)
{
    if (!np) return;
    free(np->off);
    free(np->blob);
    memset(np, 0, sizeof(*np));   /* 失败路径的指针都已置空，重复调用安全 */
}
