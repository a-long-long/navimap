#ifndef NAVIMAP_REVIDX_H
#define NAVIMAP_REVIDX_H

#include <stdint.h>
#include <stddef.h>
#include "map/binfmt.h"

/* ---------------------------------------------------------------------------
 * 按省的反向入边索引（rev.bin）
 *
 * 边记录只有 EF_TO 没有 EF_FROM（见 binfmt.h），反向搜索拿不到入边，所以按省
 * 建一份反向 CSR：start[nd]..start[nd+1] 是"指向 nd 的入边"区间，list 里存
 * 每条入边的 from 节点下标（边下标不存——拿到 from 后扫它的出边找 EF_TO==nd，
 * 出度是个位数，比再存 4 字节/边划算）。
 *
 * ★ 试过让 list 再存一份 edge_idx（v2，8 字节/条），好让反向搜索不用扫出边；
 *   **实测否决，已回退**，见 PLAN.md §44：SIGPROF 归因说那步"扫出边"值 207 ms，
 *   端到端实测只值 45~55 ms（3%），代价却是 +146 MB 磁盘。原因是那次"扫出边"
 *   顺带把 nodes[u] 记录读进了缓存，紧随其后的 node_coord 因此几乎免费
 *   （b_coord 1.0%）；省掉它之后 node_coord 变成冷随机访存（b_coord 11.0%），
 *   省下的全赔进去了。
 *
 * ★ 它 100% 由 nodes.bin + edges.bin 决定，所以可以落盘：构建期写 rev.bin，
 *   运行时 mmap。以前是**每次算路**惰性构建（实测 hebei 1318 万节点要 323 ms，
 *   占短途端到端 401 ms 的 80%），见 PLAN.md §34。
 *
 * ★ 写盘、读盘、内存构建三条路共用 rev_build_mem() 这一份构建代码 —— 判据
 *   "落盘结果 == 内存构建结果" 才有意义（各写一份的话比的是两份 bug）。
 * ------------------------------------------------------------------------- */

typedef struct {
    int       built;      /* 1 = 可用 */
    uint32_t *start;      /* n+1 */
    uint32_t *list;       /* list_count 个 from 节点 */
    uint64_t  list_count; /* = start[n]，可能 < edge_count（脏边被跳过） */
    int       mapped;     /* 1 = start/list 指向 mmap 区，释放用 munmap 不是 free */
    uint8_t  *map_base;
    size_t    map_len;
} RevIdx;

/* 内存构建（回退路径 + --verify 的对照组）。0 = 成功 */
int  rev_build_mem(const NMBinFile *nodes, const NMBinFile *edges,
                   const char *prov_name, RevIdx *rv);

/* 从 <dir>/rev.bin 映射。结构或规模不匹配返回非 0（调用方回退到构建）。 */
int  rev_load(const char *dir, const NMBinFile *nodes, const NMBinFile *edges,
              RevIdx *rv);

/* 构建并写 <dir>/rev.bin（先写 .tmp 再 rename，写失败不影响调用方）。0 = 成功 */
int  rev_write(const char *dir, const NMBinFile *nodes, const NMBinFile *edges);

void rev_free(RevIdx *rv);

/* 逐元素对比两份索引（含 list_count）。1 = 完全相同，用于 --verify */
int  rev_equal(const RevIdx *a, const RevIdx *b, uint64_t n);

#endif
