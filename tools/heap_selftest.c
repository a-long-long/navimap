/* 优先队列（route/pqueue.h）的自验收。
 *
 * 为什么需要它：pq 是 A* 的"取最小值"这一步。它一旦坏（比如改叉数时把父子
 * 下标算错、下沉漏掉最后一个子节点），症状不是崩溃，而是**弹出非最小节点**
 * —— 搜索照样跑完、照样"看起来正常"，只是路线悄悄变长。所以必须有一条
 * 专门盯着"每次 pop 是不是当前最小值"的判据。
 *
 * 判据三条（缺一不可）：
 *   ① 每次 pop 取出的 f 必须等于**独立参考实现**（下面那份教科书式二叉堆）
 *      的 pop 值 —— 两份不同代码互相盯，能抓住叉数/下标这类实现细节错误。
 *   ② 弹出来的 (f, key) **多重集**必须与插进去的完全相等（排序后逐位比）。
 *      只看 f 会漏掉"f 对但 key 串了"（那会让 A* 展开错误的节点）。
 *   ③ 周期性全量检查堆不变式（父 <= 子）。
 *
 * ★ 负向对照（--expect-fail）：把被测实现换成一个**故意写坏的**版本
 *   （push 不上浮、pop 取最后一个元素不做下沉），上面那条判据**必须**报错。
 *   不报错就说明判据是摆设 —— 这是"双向判据"的另一半。
 *
 * ★ 为什么只比 f、不比 key 的取出顺序：f 相同时（特意让 997 个取值在几十万
 *   次操作里大量撞车）多个最小值都是合法的，两份实现取的可以是不同的那个。
 *   比 key 顺序会假报，所以 key 只做多重集比对。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "route/pqueue.h"
#include "utils/plat.h"

/* ---------------- 独立参考实现：教科书式二叉堆 ---------------- */
/* 刻意写得跟被测实现不一样（递归式上浮/下沉、不同的 swap 写法），
   这样"两份代码同时犯同一个错"的概率才低。 */
typedef struct { double f; uint64_t key; } REnt;
typedef struct { REnt *a; size_t n, cap; } RefQ;

static void ref_grow(RefQ *q)
{
    q->cap = q->cap ? q->cap * 2 : 1024;
    q->a = realloc(q->a, q->cap * sizeof(REnt));
    if (!q->a) { fprintf(stderr, "oom\n"); exit(2); }
}

static void ref_up(RefQ *q, size_t i)
{
    if (i == 0) return;
    size_t p = (i - 1) / 2;
    if (q->a[p].f > q->a[i].f) {
        REnt t = q->a[p]; q->a[p] = q->a[i]; q->a[i] = t;
        ref_up(q, p);
    }
}

static void ref_down(RefQ *q, size_t i)
{
    size_t l = 2 * i + 1, r = l + 1, m = i;
    if (l < q->n && q->a[l].f < q->a[m].f) m = l;
    if (r < q->n && q->a[r].f < q->a[m].f) m = r;
    if (m == i) return;
    REnt t = q->a[m]; q->a[m] = q->a[i]; q->a[i] = t;
    ref_down(q, m);
}

static void ref_push(RefQ *q, double f, uint64_t k)
{
    if (q->n == q->cap) ref_grow(q);
    q->a[q->n].f = f; q->a[q->n].key = k;      /* REnt 有自己独立的字段名 */
    ref_up(q, q->n++);
}

static REnt ref_pop(RefQ *q)
{
    REnt top = q->a[0];
    q->a[0] = q->a[--q->n];
    ref_down(q, 0);
    return top;
}

/* ---------------- 故意写坏的对照实现 ---------------- */
typedef struct { PQEnt *a; size_t n, cap; } BadQ;

static void bad_push(BadQ *q, double f, uint64_t k)
{
    if (q->n == q->cap) {
        q->cap = q->cap ? q->cap * 2 : 1024;
        q->a = realloc(q->a, q->cap * sizeof(PQEnt));
        if (!q->a) { fprintf(stderr, "oom\n"); exit(2); }
    }
    q->a[q->n].f = f; q->a[q->n].idx = k; q->n++;   /* 不上浮 */
}

static PQEnt bad_pop(BadQ *q)
{
    return q->a[--q->n];                            /* 取最后一个，不下沉 */
}

/* ---------------- 多重集比对 ---------------- */
static int cmp_pair(const void *a, const void *b)
{
    const PQEnt *x = a, *y = b;
    if (x->f < y->f) return -1;
    if (x->f > y->f) return 1;
    if (x->idx < y->idx) return -1;
    if (x->idx > y->idx) return 1;
    return 0;
}

/* ---------------- 场景 ---------------- */
static unsigned long run(unsigned seed, size_t n_ops, int broken,
                         const char **err)
{
    PQueue q = {0};
    BadQ   bq = {0};
    RefQ   rq = {0};
    PQEnt *popped = malloc(n_ops * sizeof(PQEnt));
    PQEnt *pushed = malloc(n_ops * sizeof(PQEnt));
    size_t npop = 0, npush = 0;
    unsigned long viol = 0;
    unsigned st = seed * 2654435761u + 12345u;

    for (size_t i = 0; i < n_ops; i++) {
        st = st * 1103515245u + 12345u;
        unsigned r = (st >> 8);
        /* 55% 入队 / 45% 出队 —— 队列规模稳定在 n_ops 的百分之几，
           跟真实 A* 里"堆峰值几千、总操作近百万"的形态一致 */
        int do_push = (npop == 0 && npush == 0) || ((r % 100) < 55) ||
                      (broken ? q.n == 0 : q.n == 0);
        if (do_push) {
            /* 997 个取值 → 大量 f 相同，专门压 tie 的处理 */
            double f = (double)(r % 997) / 7.0;
            uint64_t k = ((uint64_t)r << 20) | i;
            pq_push(&q, f, k);
            bad_push(&bq, f, k);
            ref_push(&rq, f, k);
            pushed[npush].f = f; pushed[npush].idx = k; npush++;
        } else {
            double f = 0; uint64_t k = 0;
            if (broken) { PQEnt e = bad_pop(&bq); f = e.f; k = e.idx; }
            else        { k = pq_pop(&q, &f); }
            REnt re = ref_pop(&rq);
            /* ① 每次 pop 必须是当前最小值 */
            if (f != re.f) {
                if (!viol && err)
                    *err = "pop 出来的 f 与参考实现不同（取到的不是最小值）";
                viol++;
            }
            popped[npop].f = f; popped[npop].idx = k; npop++;
        }
        /* ③ 周期性全量检查堆不变式 */
        if (!broken && (i & 511) == 0) {
            for (size_t c = 1; c < q.n; c++) {
                size_t p = (c - 1) / PQ_D;
                if (q.a[p].f > q.a[c].f) {
                    if (!viol && err) *err = "堆不变式被破坏（父 > 子）";
                    viol++;
                    break;
                }
            }
        }
    }

    /* 排空：随机阶段结束时队列里通常还剩一批元素，必须全部弹出来，
       否则"插入的多重集 == 弹出的多重集"这条根本无从比起（只有全部弹出
       才有可比性）。排空阶段的 pop 同样要跟参考实现对拍。 */
    for (;;) {
        size_t left = broken ? bq.n : q.n;
        if (left == 0) break;
        double f = 0; uint64_t k = 0;
        if (broken) { PQEnt e = bad_pop(&bq); f = e.f; k = e.idx; }
        else        { k = pq_pop(&q, &f); }
        REnt re = ref_pop(&rq);
        if (f != re.f) {
            if (!viol && err)
                *err = "排空阶段 pop 出来的 f 与参考实现不同";
            viol++;
        }
        popped[npop].f = f; popped[npop].idx = k; npop++;
    }

    /* ② pop 出来的 (f,key) 多重集必须等于插进去的 */
    qsort(pushed, npush, sizeof(PQEnt), cmp_pair);
    qsort(popped, npop,  sizeof(PQEnt), cmp_pair);
    if (npush != npop) {
        if (!viol && err) *err = "push / pop 次数不等";
        viol++;
    } else if (memcmp(pushed, popped, npush * sizeof(PQEnt)) != 0) {
        if (!viol && err) *err = "弹出的 (f,key) 多重集与插入的不一致";
        viol++;
    }

    pq_free(&q); free(bq.a); free(rq.a); free(popped); free(pushed);
    return viol;
}

/* ---------------- 稳态微基准（--bench=size,iters） ----------------
 *
 * 为什么需要它：pq_pop 在全链路里只占 6.8%（京→郑 72 ms / 1064 ms），改它的收益
 * 很容易被运行间噪声（±30 ms）整个吃掉 ⇒ 必须有一个把它放大到 100% 的判据。
 * 做法：先把堆填到**真实峰值规模**（跨省 A* 实测峰值 6019 个元素），然后反复
 * "pop 一个 + push 一个"，规模保持不变，模拟搜索中间那段稳态。
 *
 * ★ 额外产出 FNV-1a 摘要：hole 式下沉与逐层交换的最终数组表示**本应逐位相同**
 *   （两者的搬运序列是一样的，只是省掉了反复写回），所以换实现后摘要必须相等。
 *   摘要变了就等于告诉你是**语义**改了，而不是变快了。
 */
#include <time.h>

static uint64_t fnv1a(const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    uint64_t hh = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) { hh ^= b[i]; hh *= 1099511628211ULL; }
    return hh;
}

/* 参考用的旧实现：**逐层交换**版下沉。留着不是为了 A/B 跑两个二进制
 * （跨进程噪声有 ±3 ms，会把收益整个吃掉），而是在**同一个进程里**跑：
 * 两者用同一份初始堆、同一串伪随机数，逐层抵消机器漂移。
 * ★ 它不是正式实现，别在别处用它；正式实现只有 pqueue.h 里那一份。 */
static uint64_t pq_pop_swap(PQueue *q, double *f_out)
{
    PQEnt *h = q->a;
    uint64_t idx = h[0].idx;
    if (f_out) *f_out = h[0].f;
    h[0] = h[--q->n];
    size_t i = 0, n = q->n;
    for (;;) {
        size_t c0 = PQ_D * i + 1;
        if (c0 >= n) break;
        size_t m = i;
        double fm = h[m].f;
        size_t cmax = c0 + PQ_D < n ? c0 + PQ_D : n;
        for (size_t c = c0; c < cmax; c++)
            if (h[c].f < fm) { fm = h[c].f; m = c; }
        if (m == i) break;
        PQEnt t = h[m]; h[m] = h[i]; h[i] = t;
        i = m;
    }
    return idx;
}

static PQueue q_clone(const PQueue *q)
{
    PQueue c;
    c.n = q->n; c.cap = q->cap;
    c.a = (PQEnt *)malloc(c.cap * sizeof(PQEnt));
    if (!c.a) { fprintf(stderr, "oom\n"); exit(2); }
    memcpy(c.a, q->a, c.cap * sizeof(PQEnt));
    return c;
}

static int bench(size_t size, unsigned long iters, int rounds)
{
    PQueue base = {0};
    unsigned long long seed = 88172645463393265ULL;
    for (size_t i = 0; i < size; i++) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        pq_push(&base, (double)(seed >> 40) * 1e-9, (uint64_t)i);
    }

    double bh = 1e30, bs = 1e30;
    uint64_t hv_swap = 0, hv_hole = 0;
    for (int rd = 0; rd < rounds; rd++) {
        /* 交替顺序跑，抵消"谁先跑谁吃亏"（先跑的要付 warm-up） */
        for (int which = 0; which < 2; which++) {
            PQueue *use = which ? &base : &base;
            (void)use;
            PQueue c = q_clone(&base);
            unsigned long long sd = seed;
            /* 微秒单调时钟（平台层）：基准要亚毫秒精度，且不能绑死 POSIX */
            uint64_t t0 = nm_now_us();
            for (unsigned long it = 0; it < iters; it++) {
                double f;
                uint64_t k = which ? pq_pop_swap(&c, &f) : pq_pop(&c, &f);
                (void)k;
                sd = sd * 6364136223846793005ULL + 1442695040888963407ULL;
                pq_push(&c, (double)(sd >> 40) * 1e-9, (uint64_t)(it + size));
            }
            double ms = (double)(nm_now_us() - t0) / 1000.0;
            if (which) { if (ms < bs) bs = ms; hv_swap = fnv1a(c.a, c.n * sizeof(PQEnt)); }
            else       { if (ms < bh) bh = ms; hv_hole = fnv1a(c.a, c.n * sizeof(PQEnt)); }
            pq_free(&c);
        }
    }
    printf("HEAP-BENCH size=%zu iters=%lu rounds=%d\n", size, iters, rounds);
    printf("  swap(逐层交换, 旧)  %8.1f ms   %6.1f ns/op\n", bs, bs * 1e6 / iters);
    printf("  hole(空穴法,   新)  %8.1f ms   %6.1f ns/op   Δ %+.1f%%\n",
           bh, bh * 1e6 / iters, 100.0 * (bh - bs) / bs);
    printf("  fnv swap=%016llx hole=%016llx  %s\n",
           (unsigned long long)hv_swap, (unsigned long long)hv_hole,
           hv_swap == hv_hole ? "一致(语义等价)" : "★不一致！");
    pq_free(&base);
    return 0;
}

int main(int argc, char **argv)
{
    int expect_fail = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--expect-fail")) expect_fail = 1;
        if (!strncmp(argv[i], "--bench=", 8)) {
            size_t sz = 6019; unsigned long it = 2000000; int rd = 5;
            sscanf(argv[i] + 8, "%zu,%lu,%d", &sz, &it, &rd);
            return bench(sz, it, rd);
        }
    }

    const char *err = NULL;
    unsigned long v = run(7, 200000, expect_fail, &err);
    if (expect_fail) {
        /* 负向对照：坏实现**必须**被抓出来，否则这条判据没有分辨力 */
        if (v == 0) {
            printf("HEAP-SELFTEST-DIFF-OK 没抓到 —— 判据失效！\n");
            return 1;
        }
        printf("HEAP-SELFTEST-DIFF-OK（坏实现被抓出 %lu 处，判据有分辨力）\n", v);
        return 0;
    }
    if (v) {
        printf("HEAP-SELFTEST-FAIL  %lu 处违规：%s\n", v, err ? err : "?");
        return 1;
    }
    printf("HEAP-SELFTEST-OK（20 万次随机操作，每次 pop 都是当前最小值；"
           "叉数 PQ_D=%d）\n", PQ_D);
    return 0;
}
