#include "route/revidx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

/* rev.bin 头（64 字节，之后紧跟 start[n+1] 与 list[list_count]，全是 u32 小端） */
#define REV_MAGIC   "NMREV01"
/* list 每条 4 字节，只存 from 节点；边下标靠运行时扫 from 的出边找 EF_TO==nd。
   ★ 只有 v1：v2（8 字节/条，直接存 edge_idx）试过并被实测否决（PLAN.md §44）。 */
#define REV_VERSION 1u
#define REV_HDR     64u

typedef struct {
    char     magic[8];
    uint32_t version;
    uint32_t hdr_bytes;
    uint64_t node_count;
    uint64_t edge_count;
    uint64_t list_count;
    uint64_t file_size;
    uint64_t body_sum;      /* FNV-1a 64 of start+list */
    uint64_t reserved[1];
} RevHdr;

static uint64_t rev_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)(ts.tv_nsec / 1000000);
}

/* FNV-1a 64：正文（start + list）的校验和。
   ★ **默认不跑**，只在 NAVIMAP_REV_STRICT=1 时跑。实测（hebei 97 MB）：
     加了它一次算路从 17 ms 涨到 571 ms——比现场重建（365 ms）还慢，
     把这一刀的收益全吃掉。所以默认只做 O(1) 的结构校验（magic / 规模 /
     start[n] 尾哨兵 / 文件大小），正文完整性交给 revbuild_cli --verify
     （每次验收都跑，见 regress.sh）与"删掉让它重建"。
     ★ 注意这确实留了个口子：正文单字节损坏（实测改坏 hebei 的 offset=100）
     结构校验挡不住，可能给出错误路径。取舍记录在 PLAN.md §35。 */
#define FNV_SEED 0xcbf29ce484222325ull
static uint64_t rev_fnv1a_of(const uint8_t *p, size_t n, uint64_t h)
{
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}
/* ★ start 和 list 是两次独立 malloc，不连续。要算"两段拼起来"的校验和，
   必须用 FNV 的可续算性一段一段喂——直接 (uint8_t*)start 读 n+1+lc 个
   u32 会越界（实测写 shanxi 时崩在这儿）。文件里两者是连续的，所以读盘
   那边一次算完即可，两边结果一致（FNV 是流式的）。 */
static uint64_t rev_fnv1a(const uint8_t *p, size_t n)
{
    return rev_fnv1a_of(p, n, FNV_SEED);
}

/* 严格模式开关：NAVIMAP_REV_STRICT=1 时加载 rev.bin 要验正文校验和。
   只在 rev_load 开头查一次（不是热路径），不要写成每条边调一次——
   getenv 在这台机器上实测 135 ns（PLAN.md §34.2）。 */
static int rev_strict(void)
{
    static int v = -1;
    if (v < 0) v = getenv("NAVIMAP_REV_STRICT") != NULL;
    return v;
}

/* ------------------------------ 内存构建 ------------------------------ */

int rev_build_mem(const NMBinFile *nodes, const NMBinFile *edges,
                  const char *prov_name, RevIdx *rv)
{
    memset(rv, 0, sizeof(*rv));
    uint64_t n  = nodes->hdr.rec_count;
    uint64_t ne = edges->hdr.rec_count;
    if (n == 0 || ne == 0) return -1;
    if (ne > 0xFFFFFFFFu) return -1;     /* 反向索引用 u32 偏移，超了就不建 */

    uint64_t t0 = rev_now_ms();
    uint32_t *start = calloc((size_t)n + 1, sizeof(uint32_t));
    uint32_t *cur   = malloc(((size_t)n + 1) * sizeof(uint32_t));
    uint32_t *list  = malloc((size_t)ne * sizeof(uint32_t));
    if (!start || !cur || !list) {
        free(start); free(cur); free(list);
        fprintf(stderr, "路由: 反向索引分配失败\n");
        return -1;
    }
    const uint8_t *nds = nodes->data;
    const uint8_t *eds = edges->data;

    /* 计数：按 from 节点遍历（CSR 天然按 from 分组） */
    for (uint64_t i = 0; i < n; i++) {
        const uint8_t *nr = nds + i * NM_NODE_BYTES;
        uint32_t eb = nm_get_u32(nr + NF_EDGE_BEGIN);
        uint32_t ec = nm_get_u16(nr + NF_EDGE_COUNT);
        for (uint32_t k = 0; k < ec; k++) {
            uint64_t ei = (uint64_t)eb + k;
            if (ei >= ne) break;
            uint32_t to = nm_get_u32(eds + ei * NM_EDGE_BYTES + EF_TO);
            if ((uint64_t)to >= n) continue;      /* 脏数据，忽略 */
            start[to + 1]++;
        }
    }
    /* 前缀和：start[i] = 指向节点 i 的入边区间起点，start[i+1] = 区间终点。
       ★ 不能写成 for (i=0; i<=n; i++) { c=start[i]; start[i]=acc; acc+=c; }——
         那样 start[n] 会漏掉 cnt[n-1]（最后一个节点的入边区间被截成空）。 */
    uint64_t acc = 0;
    for (uint64_t i = 0; i < n; i++) {
        uint32_t c = start[i + 1];
        start[i] = (uint32_t)acc;
        acc += c;
    }
    start[n] = (uint32_t)acc;
    memcpy(cur, start, ((size_t)n + 1) * sizeof(uint32_t));
    for (uint64_t i = 0; i < n; i++) {
        const uint8_t *nr = nds + i * NM_NODE_BYTES;
        uint32_t eb = nm_get_u32(nr + NF_EDGE_BEGIN);
        uint32_t ec = nm_get_u16(nr + NF_EDGE_COUNT);
        for (uint32_t k = 0; k < ec; k++) {
            uint64_t ei = (uint64_t)eb + k;
            if (ei >= ne) break;
            uint32_t to = nm_get_u32(eds + ei * NM_EDGE_BYTES + EF_TO);
            if ((uint64_t)to >= n) continue;
            list[cur[to]++] = (uint32_t)i;
        }
    }
    free(cur);
    rv->built = 1;
    rv->start = start;
    rv->list  = list;
    rv->list_count = start[n];
    fprintf(stderr, "路由: 为 %s 建反向索引（%llu 边 / %llu 节点，%llu ms）\n",
            prov_name ? prov_name : nodes->hdr.source,
            (unsigned long long)ne, (unsigned long long)n,
            (unsigned long long)(rev_now_ms() - t0));
    return 0;
}

/* ------------------------------- 落盘 ------------------------------- */

static int rev_path(const char *dir, char *out, size_t cap)
{
    int k = snprintf(out, cap, "%s/rev.bin", dir);
    return (k > 0 && (size_t)k < cap) ? 0 : -1;
}

int rev_write(const char *dir, const NMBinFile *nodes, const NMBinFile *edges)
{
    RevIdx rv;
    if (rev_build_mem(nodes, edges, NULL, &rv) != 0) return -1;

    uint64_t n = nodes->hdr.rec_count;
    uint64_t lc = rv.list_count;
    size_t body  = ((size_t)n + 1) * 4u + (size_t)lc * 4u;
    size_t total = REV_HDR + body;

    char path[4096], tmp[4096];
    if (rev_path(dir, path, sizeof(path)) != 0) { rev_free(&rv); return -1; }
    int k = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (k <= 0 || (size_t)k >= sizeof(tmp)) { rev_free(&rv); return -1; }

    FILE *f = fopen(tmp, "wb");
    if (!f) { rev_free(&rv); return -1; }

    RevHdr h;
    memset(&h, 0, sizeof(h));
    memcpy(h.magic, REV_MAGIC, 8);
    h.version    = REV_VERSION;
    h.hdr_bytes  = REV_HDR;
    h.node_count = n;
    h.edge_count = edges->hdr.rec_count;
    h.list_count = lc;
    h.file_size  = (uint64_t)total;
    /* ★ 两段拼起来的校验和（start 与 list 是两次独立 malloc，不连续）：
       FNV 是流式的，一段一段喂结果与"文件里连续的一段"一致。 */
    uint64_t hs = rev_fnv1a((const uint8_t *)rv.start, ((size_t)n + 1) * 4u);
    hs = rev_fnv1a_of((const uint8_t *)rv.list, (size_t)lc * 4u, hs);
    h.body_sum   = hs;

    int ok = 1;
    if (fwrite(&h, 1, sizeof(h), f) != sizeof(h)) ok = 0;
    if (ok && fwrite(rv.start, 4, (size_t)n + 1, f) != (size_t)n + 1) ok = 0;
    if (ok && fwrite(rv.list, 4, (size_t)lc, f) != (size_t)lc) ok = 0;
    /* ★ 只 fclose 一次。写成 "if (fclose(f)) { ok=0; f=NULL; } if (f) fclose(f);"
       会在关闭成功时再关一次（FILE 结构被二次释放，报 double free）。
       关闭失败同样算写坏——数据可能还留在 stdio 缓冲里没落盘。 */
    if (fclose(f) != 0) ok = 0;

    if (!ok) { unlink(tmp); rev_free(&rv); return -1; }
    /* 原子替换：中途失败不会留下半截 rev.bin（半截会被 rev_load 的规模校验挡住，
       但能不留垃圾就别留） */
    if (rename(tmp, path) != 0) { unlink(tmp); rev_free(&rv); return -1; }
    rev_free(&rv);
    return 0;
}

/* -------------------------------- 读盘 -------------------------------- */

/* NAVIMAP_REV_MEM=1：不用 rev.bin，强制走内存构建。用来重现"没有 rev.bin"
   时的降级路径（也用于回归里的对照组）。 */
static int rev_force_mem(void)
{
    static int v = -1;
    if (v < 0) v = getenv("NAVIMAP_REV_MEM") != NULL;
    return v;
}

int rev_load(const char *dir, const NMBinFile *nodes, const NMBinFile *edges,
             RevIdx *rv)
{
    memset(rv, 0, sizeof(*rv));
    if (rev_force_mem()) return -1;
    char path[4096];
    if (rev_path(dir, path, sizeof(path)) != 0) return -1;

    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return -1; }

    uint64_t n  = nodes->hdr.rec_count;
    uint64_t ne = edges->hdr.rec_count;
    size_t need_min = REV_HDR + ((size_t)n + 1) * 4u;
    if ((size_t)st.st_size < need_min) { close(fd); return -1; }

    uint8_t *base = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (base == MAP_FAILED) return -1;

    RevHdr h;
    memcpy(&h, base, sizeof(h));
    if (memcmp(h.magic, REV_MAGIC, 8) != 0 ||
        h.version != REV_VERSION ||
        h.hdr_bytes != REV_HDR ||
        h.node_count != n || h.edge_count != ne ||
        h.file_size != (uint64_t)st.st_size) {
        munmap(base, (size_t)st.st_size);
        return -1;
    }
    size_t body = ((size_t)n + 1) * 4u + (size_t)h.list_count * 4u;
    if (REV_HDR + body != (size_t)st.st_size) {
        munmap(base, (size_t)st.st_size);
        return -1;
    }
    uint32_t *start = (uint32_t *)(void *)(base + REV_HDR);
    /* 自洽：尾哨兵必须等于 list_count，否则这份文件是坏的 */
    if ((uint64_t)start[n] != h.list_count) {
        munmap(base, (size_t)st.st_size);
        return -1;
    }
    /* 正文校验和：默认不跑（太贵，见 rev_fnv1a 的注释），只在严格模式下跑 */
    if (rev_strict() && rev_fnv1a(base + REV_HDR, body) != h.body_sum) {
        munmap(base, (size_t)st.st_size);
        return -1;
    }

    rv->built = 1;
    rv->mapped = 1;
    rv->start = start;
    rv->list_count = h.list_count;
    rv->list = start + (n + 1);
    rv->map_base = base;
    rv->map_len  = (size_t)st.st_size;
    return 0;
}

void rev_free(RevIdx *rv)
{
    if (!rv) return;
    if (rv->mapped) {
        if (rv->map_base) munmap(rv->map_base, rv->map_len);
    } else {
        free(rv->start);
        free(rv->list);
    }
    memset(rv, 0, sizeof(*rv));
}

int rev_equal(const RevIdx *a, const RevIdx *b, uint64_t n)
{
    if (!a->built || !b->built) return 0;
    if (a->list_count != b->list_count) return 0;
    if (memcmp(a->start, b->start, ((size_t)n + 1) * sizeof(uint32_t)) != 0) return 0;
    if (a->list_count &&
        memcmp(a->list, b->list, (size_t)a->list_count * sizeof(uint32_t)) != 0)
        return 0;
    return 1;
}
