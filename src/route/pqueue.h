#ifndef NAVIMAP_PQUEUE_H
#define NAVIMAP_PQUEUE_H
/* A* / Dijkstra 的优先队列：数组实现的 **d 叉最小堆**（懒删除，无 decrease-key）。
 *
 * ★ 叉数不是拍脑袋定的，是实测扫出来的（京→郑 693 km：push 89 万 / pop 88 万 /
 *   下沉 854 万层，堆峰值只有 6019 个元素 ≈ 96 KB，整个堆常年待在 L2）：
 *
 *      D=2  36.4 ms    D=3  24.7 ms    **D=4  13.2 ms**    D=5  19.3 ms
 *      D=6  25.1 ms    D=8  25.9 ms     D=16 37.7 ms
 *
 *   两个原因让 D=4 一枝独秀（它比邻居 D=3/D=5 好一大截，不是单调曲线）：
 *   ① 元素是 16 字节（double f + uint64 key），4 个子节点正好 **64 字节 = 一条
 *      cache line**，一次扫描只碰一行；D=3/5/6 的子节点跨行。
 *   ② D 是 2 的幂时，`i*D+1` 和 `(i-1)/D` 都是移位，编译器不用生成乘法。
 *   ⇒ 别再往上调：D=8 的下沉层数只有 D=4 的 71%，却**更慢**（每层要扫 8 个
 *      元素、跨 2 条 cache line），D=16 比二叉堆还慢。
 *
 * 下沉是整个堆的绝对大头（每次 pop 9.69 层，上浮只有 0.32 层/次 —— 插入在
 * 二叉堆里是摊还 O(1)，所以"减少插入开销"这类优化在这里没油水）。
 *
 * 元素的载荷是**状态表的 StEnt 下标**（uint32），不是坐标 key。这样 pop 之后
 * 直接 `&st->ents[idx]` 就能拿到条目，**省掉一次哈希查找**（在跨省 A* 里那是
 * 2~3 次随机访存，实测占京→郑 13.8% / 164 ms，是主循环里最大的单项）。
 * 下标在 ents 数组 realloc 后依然有效（只 append、不删除），所以可以安全地
 * 在堆里长期持有。sizeof(PQEnt) 仍是 **16 字节**，上面 D=4 的 cache line
 * 论证不受影响。
 *
 * 正确性判据见 tools/heap_selftest.c：随机操作序列下每次 pop 都必须取出当前
 * 最小值；另有一条负向对照（坏实现必须 FAIL），保证这条判据真有分辨力。
 */
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

#define PQ_D 4

/*
 * ★ 载荷是 uint64 但**实际只存 uint32 的 StEnt 下标**：这样 sizeof(PQEnt) 仍是
 *   16 字节（没有匿名 padding），一方面保住上面 D=4 的 cache line 论证，
 *   另一方面 heap_selftest 用 memcmp 比对 (f, 载荷) 多重集时才不会踩未初始化的
 *   填充字节。别为了"省 4 字节"把它改成 uint32 —— 那会让多重集比对随机失败。
 */
typedef struct { double f; uint64_t idx; } PQEnt;

typedef struct {
    PQEnt *a;
    size_t n, cap;
} PQueue;

static inline void *pq_xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n);
    if (!q) {
        fprintf(stderr, "路由: 内存不足（需要 %zu 字节）\n", n);
        exit(2);
    }
    return q;
}

static inline void pq_push(PQueue *q, double f, uint64_t idx)
{
    if (q->n == q->cap) {
        q->cap = q->cap ? q->cap * 2 : 4096;
        q->a = pq_xrealloc(q->a, q->cap * sizeof(PQEnt));
    }
    PQEnt *h = q->a;
    size_t i = q->n++;
    h[i].f = f; h[i].idx = idx;
    while (i > 0) {
        size_t p = (i - 1) / PQ_D;
        if (h[p].f <= h[i].f) break;
        PQEnt t = h[p]; h[p] = h[i]; h[i] = t;
        i = p;
    }
}

/* 返回状态表下标。调用前必须保证 q->n > 0（调用点都有判空）
 *
 * ★ 下沉用的是 **hole（空穴）法**，不是逐层交换：先把尾元素从 h[0] 位置上"挖
 *   出来"拿在寄存器里，每下降一层只做 1 次 16 字节搬运（h[i] = h[m]），最后才
 *   把尾元素落进真正的空位。逐层交换每层要 3 次搬运（t=h[m]; h[m]=h[i]; h[i]=t）。
 *   每次 pop 平均下沉 9.69 层 ⇒ 写流量降到约 1/3（PLAN.md §43）。
 *   ★ 注意比较的基准是**尾元素的 f**（fm = last.f），不是 h[i].f —— 空位 i 上的
 *     值是上一层刚搬上来的副本，用它比较会得到错误的最小值。
 */
static inline uint64_t pq_pop(PQueue *q, double *f_out)
{
    PQEnt *h = q->a;
    uint64_t idx = h[0].idx;
    if (f_out) *f_out = h[0].f;
    if (--q->n == 0) return idx;

    PQEnt last = h[q->n];
    size_t i = 0, n = q->n;
    for (;;) {
        size_t c0 = PQ_D * i + 1;
        if (c0 >= n) break;
        size_t m = i;
        double fm = last.f;
        size_t cmax = c0 + PQ_D < n ? c0 + PQ_D : n;
        for (size_t c = c0; c < cmax; c++)
            if (h[c].f < fm) { fm = h[c].f; m = c; }
        if (m == i) break;
        h[i] = h[m];
        i = m;
    }
    h[i] = last;
    return idx;
}

/* 只看堆顶的 f，不弹出。空堆返回 +inf。 */
static inline double pq_peek_f(const PQueue *q)
{
    return q->n ? q->a[0].f : (double)(1.0 / 0.0);
}

static inline void pq_clear(PQueue *q) { q->n = 0; }
static inline void pq_free(PQueue *q)  { free(q->a); q->a = NULL; q->n = q->cap = 0; }

#endif /* NAVIMAP_PQUEUE_H */
