#ifndef NAVIMAP_LMTABLE_H
#define NAVIMAP_LMTABLE_H
/* 地标下界表（PLAN.md §48 / §49 / ★§52.8）
 *
 * ★★★ 它不是**严格**的下界，是"加速启发式"（2026-09-28 实测确认，用户拍板接受）：
 *   `scripts/lmadmissible.py` 用"开表 w=1.0 == 无表 w=1.0"验收（后者 h 是直线，
 *   可采纳 ⇒ 真值）。50 条里 **2 条开表更差**：南阳→天津 501 vs 500 分、
 *   太原→石家庄 129 vs 128 分；距离口径放大到 895.16 vs 878.41 km（+1.9%）。
 *   ⇒ 带 lm.bin 时 **`--exact` / w=1.0 不再保证最优**。
 *   已证伪三个假设：加大 penalty（+40 km 不收敛）、双向目标格取错（单向同样越界）、
 *   转向限制（用 --no-restr 建的表照样越界）。剩余候选：
 *   **u 侧要"格内 max"而不是 min**（`d(L,u) ≥ 格内min` 方向反了；格内两点的
 *   *路程* 可远大于格对角，penalty 按格对角算覆盖不住）。
 *   修法（若哪天要修）是把表改成 min+max 两份。详见 PLAN.md §52.8。
 *
 * ★ 实际影响有限：发货是时间口径 w=1.25，本就不承诺最优；而开表比无表
 *   质量**好 3.21%**（50 条 32 好 / 11 差）⇒ 整体仍是明显正收益。
 *   `regress.sh` 的 `lm_admissible` 项把"最多差 1 分"钉住，防止恶化。
 *
 * 每个网格单元存"单元内所有节点 → 地标 L 的最小**路程**"（uint16，单位 100 m）。
 * 查询时取下界（单侧形式，实测与双侧等价 ⇒ 不用建反向表，见 §49.4）：
 *
 *     d(u,t) ≥ d(L,t) − d(L,u)
 *            ≥ [ D(cel_t,L) − diam_t ] − [ D(cel_u,L) + diam_u ]
 *
 * 其中 D(cel,L) = 单元内 d(x,L) 的最小值，diam 是单元对角（任一点到单元内
 * 另一点的距离**直线**上界 —— ★ 这正是越界的来源：格内两点的**路程**可以
 * 远大于直线距离，见上面 §52.8）。三个罚项（diam_u + diam_t + 取整误差）
 * 构建期一次算好，写在 header 的 penalty_m 里。
 *
 * ★ 取整必须**向下**（floor）：
 *   - u 侧要的是下界 ⇒ 存的值 ≤ 真值，floor 满足；
 *   - t 侧要的是上界 ⇒ 真值 ≤ 存的值 + 100 m，这 100 m 已经算进 penalty_m。
 *   反过来用 ceil 会让 u 侧失效（存的值 ≥ 真值，减出来偏大 ⇒ 下界越界）。
 *
 * ★ 单位：表是**米**（用 --mode=dist 建），用时按 `d * 3.6 / H_SPEED_KMH`
 *   换成秒，与 heur() 对直线距离的处理完全一致 —— 不能建时间口径的表：
 *   那样 diam 罚项要按**最慢**限速换算，7 km 就能罚出一个多小时。
 */
#include <stdint.h>
#include <stddef.h>

#define LM_MAGIC    0x4C4D4150u   /* "LMAP" */
/* v2：加了图指纹（nprov + total_nodes）。
   ★ 为什么必须钉死：省数据重抽之后旧表**不崩、不报错**，只是悄悄给出偏大的
     下界 ⇒ 路线次优甚至错，而且极难归因（表现是"算路偶尔绕远"）。
     sync_maps.sh 的 LM_WANT_VER 必须跟着改。 */
#define LM_VERSION  2u
#define LM_UNIT_M   100u          /* 存的是 米 / 100 */
#define LM_NONE     0xFFFFu       /* 该单元没有路网节点 / 该地标够不到 */
#define LM_MAXVAL   0xFFFEu       /* 6553.4 km，超过就按它存 */

typedef struct {
    uint32_t magic, version;
    int32_t  lat0_e7, lon0_e7;   /* 网格原点（西南角），向下取整到格 */
    uint32_t cell_e7;            /* 格边长（e7 度） */
    uint32_t nrow, ncol;
    uint32_t nlm;                /* 地标个数 */
    uint32_t unit_m;             /* = LM_UNIT_M */
    uint32_t penalty_m;          /* 罚项（米）：2×格对角 + 取整误差 */
    uint32_t nprov;              /* 图指纹：建表时加载的省数 */
    uint64_t total_nodes;        /* 图指纹：Σ 各省 nodes.bin 记录数（含副本省） */
} LmHeader;

typedef struct {
    LmHeader h;
    uint16_t *v;                 /* nrow*ncol*nlm，行优先 */
    size_t    ncell;
    int       loaded;
} LmTable;

/* 从 <dir>/lm.bin 载入。没有文件或版本不对时返回 -1（调用方退回纯直线启发式，
   不算错误）。成功返回 0。 */
int  lm_table_load(LmTable *t, const char *dir);
void lm_table_free(LmTable *t);

/* 图指纹校验：当前路网的省数 / 总节点数与建表时记的是否一致。
   ★ 表本身没有"图变了"的感知能力 —— 不校验的话，重抽过 PBF 之后它会继续被
     加载，给出的下界偏大（admissible 被破坏），路线悄悄变差。
   返回 1 = 匹配。 */
int  lm_table_match(const LmTable *t, uint32_t nprov, uint64_t total_nodes);

/* 格号。落在网格外返回 (size_t)-1。 */
size_t lm_cell_of(const LmTable *t, int32_t lat_e7, int32_t lon_e7);

/* u → 目标格 tc 的地标下界（**米**）。不可用（任意一侧没数据）返回负数。 */
double lm_bound_m(const LmTable *t, size_t tc, int32_t lat_e7, int32_t lon_e7);

/* 写端（skedaddle_lmbuild_cli 用）：分配/填充/落盘。 */
int  lm_table_init(LmTable *t, int32_t lat0_e7, int32_t lon0_e7, uint32_t cell_e7,
                   uint32_t nrow, uint32_t ncol, uint32_t nlm, uint32_t penalty_m,
                   uint32_t nprov, uint64_t total_nodes);
int  lm_table_save(const LmTable *t, const char *path);

#endif
