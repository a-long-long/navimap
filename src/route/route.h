#ifndef NAVIMAP_ROUTE_H
#define NAVIMAP_ROUTE_H

#include <stdint.h>
#include "map/mapset.h"

/* ---------------------------------------------------------------------------
 * 跨省路线计算（阶段 8）
 *
 * 核心：★ 搜索状态的主键是坐标（coord_key_e7），不是 (省, 省内下标)。
 * 展开一个坐标时，用 mapset_query_tile 在所有省里查同一 Z14 瓦片，
 * 把落在该坐标上的所有 (省,节点) 的出边一起松弛——一个物理路口只展开一次。
 * 这就是 OsmAnd 的 RoutePlannerContext.cpp:132 `id = (x31<<31)|y31` +
 * loadRouteCalculationSegment 的做法，好处有二：
 *   1. 加一个省不需要重建任何"局部下标 → 全局下标"的映射表；
 *   2. 各省数据互相包含时（实测北京 ⊂ 河北 100%，同一路口在两省各有一份），
 *      重复副本不会让搜索量翻倍——它们本来就是同一个状态。
 *      （初版用 (省,节点) 当主键，北京→天津 展开了 111 万个状态、跑 97 秒，
 *        且路径在京/冀副本之间来回抖 560 次；换坐标键后这两个问题一起消失。）
 *
 * 双向 A*（默认）。正向用 CSR 出边；反向按省惰性构建入边索引（rev_build，
 * 只给反向搜索真正走进去的省建，被完全包含的副本省永远不建）。
 * 启发式用大圆距离 ÷ 130km/h，不低于任何道路的限速，且满足三角不等式
 * （边权 = 时间 ≥ 距离/130km/h），所以 admissible 且 consistent。
 * 终止条件：min_f(正向堆) + min_f(反向堆) ≥ 当前最优 μ。
 *
 * 已知近似：转向限制只在"入边与出边属于同一个省的同一点"时生效。跨省转移
 * 后入边属于另一个省，那一跳不做限制检查——影响极小（限制都在路口内部）。
 * ------------------------------------------------------------------------- */

#define REDGE_SEAM 0xFFFFFFFFu   /* 路径里表示"此处跨了省"（零长度接缝） */

typedef struct { uint32_t prov, node; } RNode;

static inline uint64_t rnode_key(RNode n)
{
    return ((uint64_t)n.prov << 32) | (uint64_t)n.node;
}
static inline RNode rnode_of(uint64_t k)
{
    RNode n; n.prov = (uint32_t)(k >> 32); n.node = (uint32_t)(k & 0xFFFFFFFFu);
    return n;
}

typedef struct {
    int      ok;            /* 1 = 找到路径 */
    double   total_m;       /* 路径总长（米，按边 dist_cm 累加） */
    double   total_s;       /* 预计耗时（秒） */
    uint32_t n;             /* 路径点数 */
    RNode   *pts;           /* n 个：起点 → 终点 */
    int32_t *lat_e7;        /* n 个 */
    int32_t *lon_e7;        /* n 个 */
    uint32_t *edge_idx;     /* n-1 个：pts[i] → pts[i+1] 用的边（pts[i+1].prov 内） */
    uint32_t *from_node;    /* n-1 个：该段边起点在同一省内的节点下标（自检用） */
    uint8_t  *edge_cls;     /* n-1 个：该段道路等级 RC_*，接缝段为 0xFF */
    uint8_t  *edge_flags;   /* n-1 个：该段边的标志位低字节（EF_ROUNDABOUT 等）。
                             * 导航指令要认环岛，几何分辨不出来，只能靠它。 */
    uint32_t *seg_m;        /* n-1 个：该段长度（米） */
    uint32_t *seg_ds;       /* n-1 个：该段耗时（0.1 秒）。按该段限速算，
                             * 与 total_s 同口径（Σ/10 == total_s）。
                             * 导航要它算"剩余 ETA"：剩余时间必须是剩下那些
                             * 段按各自限速求和，用总时长按里程比例摊会错
                             * （前段高速后段城区，摊出来的 ETA 偏乐观）。 */
    uint32_t n_seams;       /* 相邻两段所属省份不同的次数（= 真正的跨省次数） */
    uint64_t expanded;      /* 展开节点数（含接缝合并进来的） */
    uint64_t elapsed_ms;
} RouteResult;

typedef struct {
    int      mode_time;     /* 1 = 按时间（默认），0 = 按距离 */
    int      avoid_toll;
    int      avoid_unpaved;
    int      bidir;         /* 1 = 双向 A*（默认），0 = 单向（用于 A/B 对照） */
    int      no_restr2;     /* 1 = 关掉 via=way 两段限制，仅供 A/B 对照验收 */
    int      no_restr1;     /* 1 = 关掉 only_* 限制，仅供 A/B 对照验收 */
    int      no_seam_restr; /* 1 = 关掉"双向搜索会合点"处的限制检查，仅供负向对照 */
    /* ★ 只影响 **lm_dijkstra**（建地标表那次全图 Dijkstra），不影响算路：
       1 = 完全不做转向限制检查。诊断用（§52.8）：用它建表可以验证
       "地标距离偏乐观"是不是转向限制引起的。 */
    int      lm_no_restr;

    /* ★ 调用点一律用**指名初始化**（如 RouteOpts o = { .bidir = 1 }）。
       以前用位置初始化，往这中间插一个字段就得把 5 个调用点挨个改对，
       漏一个就是"新字段落到旧槽位"这种静默错位。 */
    /* ★ 最优性的前提是 **h 可采纳**。加载了 lm.bin 之后这个前提不成立
       （§52.8：地标下界只是"加速启发式"，实测 50 条里 2 条给出次优解）
       ⇒ **开表时 --exact 不再保证最优**。无表（h = 直线）时才保证最优。
       对拍/验收要真值请用 `NAVIMAP_LM=0`。 */
    int      exact;         /* 1 = 双向用"保证最优"的终止条件（慢），0 = 默认快速条件 */
    uint64_t max_expand;    /* 展开上限，防跑飞（0 = 用默认 800 万） */

/* 默认启发式权重。**1.4**，实测档位，见 PLAN.md §45（10 条路线扫出来的）。
 *
 * 速度：京→郑 1076→88 ms、青→西 1679→124 ms，10 条里 9 条降 57%~93%。
 * 代价：预计时间 8/10 条只 +0~1.2%，京→郑 +2.8%、大连→北京 +2.6%。
 *
 * ★ 为什么不是更大：w=1.5 会再快一倍，但天津→郑州 / 沈阳→北京各 +2.6%，
 *   大连→北京 +3% —— 从这里开始真的劣化（与旧记的"w≥1.5 真劣化"一致）。
 * ★ 为什么不是更小：w=1.0 与 1.2 在 10 条路线上里程/分钟逐位相同 ⇒ 1.2 是
 *   "无损上限"，往下白白慢 1.8 倍。1.3 只 +0~1% 但只降 30~67%。
 *
 * ★ 例外：**大连→北京 4.26M 展开 / 7.9 s，w 从 1.2 到 1.5 只降 15%**。
 *   绕渤海湾：直线 460.89 km vs 路网 839.65 km ⇒ 绕行系数 **1.82**（别的路线只有
 *   1.09~1.14）。h 是 admissible 的，但**低估**了 1.82 倍 ⇒ 区分度太差、A* 退化成
 *   Dijkstra。注意方向别记反：问题是 h 太小不是太大。
 *   这不是 w 能救的（要压住它得 w≈1.82，而 1.5 就已经让别的路线 +2.6%），
 *   要动就得换启发式本身。
 *
 * ★ 验收/对拍工具（restr_ab、--exact）必须用 1.0，否则里程差异不再只反映
 *   被测因素（转向限制）本身的代价。改这个常量会改变**所有**路线的计算结果
 *   —— 它是产品行为，不是纯性能旋钮。 */
#define ROUTE_HEUR_W_DEFAULT 1.4

/* 自适应第 2 遍的 w：第 1 遍用基准 w 撞到展开上限时，换这个更重的 w 重跑一遍。
 *
 * 1.8 是实测档位（PLAN.md §45.8，无偏随机样本）：难走的那几条从 4~10 s 降到
 * 0.06~0.7 s，代价是预计时间中位 +2.2%、最坏 +5.6%。
 * ★ 关键是**只在撞顶的路线上付这个代价** —— 好走的路线根本走不到第 2 遍，
 *   结果与基准 w 完全一致。全局调成 1.8 会让所有路线都变差，那是不划算的。 */
#define ROUTE_HEUR_W_HARD 1.8

/* 有地标下界表（lm.bin）时的基准 w —— 用户 2026-09-28 拍板，PLAN.md §51.4 / §52.2。
 *
 * ★★ 为什么不能沿用 1.4：**w 的有效贪心度 = w × h/真值**。
 *   没有表时 h（直线）只有真值的 44~74%，1.4 的实际贪心度才 0.7、1.8 才 0.9
 *   —— 这两个数本来就是照着"h 系统性低估一半"调出来的（§45）。
 *   有了表 h 到 92~97%，1.4 变成 1.33、**1.8 变成 1.71（有害）**。
 *   ★★ 同理，**地标越多、h 越紧，w 就得跟着往下调**（§52.2）：
 *     同一个 w 配更紧的 h ⇒ 有效贪心度变大 ⇒ 质量反而变差。
 *     10 地标时最优是 1.3；扩到 30 地标（h 更紧）最优降到 **1.25**。
 *
 * 50 条城市对实测（§52.2，30 地标，与"10 地标 w=1.3"逐条配对）：
 *   w=1.3   中位 103 ms / p90 304 / 最慢 462  | 分钟 21585（好14/差14，持平）
 *   w=1.25  中位 123 ms / p90 753 / 最慢 1503 | 分钟 21503（好25/差10）★
 *   w=1.2   中位 191 ms / p90 887 / 最慢 3783 | 分钟 21477（好24/差11）
 *   ⇒ 1.25 是唯一"速度与质量同时优于旧发货配置"的档位；1.3 快一倍但质量只持平；
 *     1.2 只再多买 26 分钟，尾巴却炸到 3.8 s。
 * ★ 有表时**同时把 heur_w_hi 也兜底成这个值**，等于关掉"撞顶升级"：
 *   升级到 1.8 在这张表上是净亏损（快 2.5×、质量差 5.1%）。 */
#define ROUTE_HEUR_W_LM   1.25

/* 第 1 遍的展开数上限，撞到就说明"这条不好走"，换 ROUTE_HEUR_W_HARD 重跑。
 *
 * ★ 20 万是量出来的分界：好走的路线（城内 5 千、京→郑 10 万、青→西 12 万、
 *   沈阳→北京 14 万、天津→郑州 15 万）都在顶以内；需要加速的那几条是
 *   40 万~600 万。顶定得越高，撞顶时第 1 遍白付的越多（实测 60 万展开 ≈ 1 s）。 */
#define ROUTE_ADAPT_TRY 200000u

    /* 启发式权重 w（OsmAnd 的 heuristicCoefficient，RoutePlanner.cpp:746）：
     *   入堆优先级 f = g + w·h。
     * w=1 是标准 A*（保证最优，默认）；w>1 是"加权 A*"——堆更偏向目标方向，
     * 展开数骤降，代价是解不再保证最优（误差有界，见 PLAN.md §27）。
     * 这是用"路径可能略长"换"算得快"的档位，与 exact 是两个独立的旋钮：
     * exact 管终止条件（要不要把两侧都搜到最优），w 管搜索的贪心程度。 */
    double   heur_w;
    /* 自适应：第 1 遍用 heur_w、展开封顶 adapt_try；撞顶就换 heur_w_hi 重跑。
     * 兜底值见 ROUTE_HEUR_W_HARD / ROUTE_ADAPT_TRY（指名初始化漏写时填）。
     * ★ 想关掉加速、只保留"撞顶再搜一遍"：把 heur_w_hi 设成与 heur_w 相同
     *   （restr_ab 的对拍就这么干 —— 它必须两边同 w，里程差才只反映转向限制）。 */
    double   heur_w_hi;
    uint64_t adapt_try;
} RouteOpts;

typedef struct Router Router;

Router *router_create(const MapSet *ms, const RouteOpts *opt);
void    router_destroy(Router *r);

/* 地标下界表有没有真的加载上（0 = 没有，退回纯直线启发式）。
 * 表是可选的，所以"表没加载"不报错、只是算路变慢 —— 验收脚本靠这个问出来。 */
int     router_lm_loaded(const Router *r);

/* 表里有几个地标（0 = 没表）。地标个数直接决定路线结果，必须可验收。 */
int     router_lm_nlm(const Router *r);

/* 实际生效的 w（兜底之后的），给诊断/验收打印用。 */
double  router_heur_w(const Router *r);

/* 把经纬度吸附到最近的可路由节点。radius 为 0 时用默认（0..3 圈 Z14 瓦片）。
 * 返回 0 成功；-1 = 附近没有任何路网节点（比如落在海上）。 */
int     router_nearest(Router *r, double lat, double lon, RNode *out, double *dist_m);

/* 同上，但一次给出**最近的 k 个**候选（按距离升序），返回实际个数（0 = 附近没路网）。
 * ★ 存在意义：nodes.bin 里有"有出边却悬在小连通块里"的节点（PLAN.md §51.1），
 *   只取最近一个会整条路线"不连通"，得有得换。k 上限 SNAP_TRY。 */
int     router_nearest_k(Router *r, double lat, double lon, RNode *out, int k,
                         double *dist_m);

/* 经纬度 → 经纬度算路，吸附撞上死节点时自动换下一个候选重试。
 * 返回 0（out 有效；fa/fb/da/db = 实际采用的那对端点，均可为 NULL）；
 *      -1 = 附近无路网；-2 = 所有候选都不连通；-3 = 内存/规模超限。 */
int     router_route_ll(Router *r, double flat, double flon, double tlat, double tlon,
                        RouteResult *out, RNode *fa, RNode *fb,
                        double *da, double *db);

/* 计算 from → to。返回 0 = 找到（out->ok=1）；-1 = 不连通；-2 = 内存/规模超限 */
int     router_plan(Router *r, RNode from, RNode to, RouteResult *out);
void    route_result_free(RouteResult *rr);

/* 路径自检：每段长度与边记录一致、相邻点坐标连续、接缝处坐标相同，
 * 且路径不踩任何转向限制（含 via=way 两段序列）。
 * 返回 0 = 全部通过（打印细节到 stdout）；非 0 = 有断言失败。
 * ★ 不是 const：限制表是惰性构建的，自检会顺带把它建起来。 */
int     route_check(Router *r, const RouteResult *rr);

/* 诊断/验收用：某省 via=way 两段限制的条数。
   注意是惰性构建的——只有搜索真正走进过这个省才有值（见 restr_build）。 */
size_t route_restr2_count(const Router *r, uint32_t prov);

/* ---------------- 地标距离：一对多 Dijkstra ----------------
 *
 * 从**一个**地标节点跑到**全图**（跑到堆空为止），给出每个 (省, 节点) 的
 * 最小代价。用途是给 A* 做地标下界（PLAN.md §48）：
 *
 *     d(u,t) ≥ d(L,t) − d(L,u)          （只要"从 L 出发"的距离，本表就是）
 *
 * 与 router_plan 的差别只有三条：没有目标点（h=0）、不提前终止、不回溯路径。
 *
 * ★ 为什么必须自己做、不能直接复用 A*：A* 的状态表是 StEnt（48 B/条目），
 *   跑全图 4560 万个坐标要 ~3.2 GB，本机放不下。这里改成**按 (省, 节点) 的平行
 *   数组**（各 4 B），代价是同一坐标的多省成员各存一份 —— 用 pop 时的零代价
 *   转移（members_of）把它们的距离拉齐，语义等价于"坐标即状态"。
 *
 * ★ 单位与 g 一致：opt.mode_time=1（默认）时是**秒**，否则是米。
 *   存成 uint32，比例 LM_SCALE（见 route.c）。
 */
#define LM_UNREACHABLE 0xFFFFFFFFu

typedef struct {
    uint64_t *off;    /* 每省在 flat 数组里的起点，n+1 个（最后一个 = total） */
    uint32_t *cost;   /* flat(prov,node) → 最小代价×LM_SCALE；LM_UNREACHABLE = 没走到 */
    uint32_t *arr;    /* 到达该 (省,节点) 用的边下标（省内），REDGE_SEAM = 起点/跨缝 */
    uint32_t *prev2;  /* 再前一条边（via=way 两段限制要用），REDGE_SEAM = 没有 */
    uint8_t  *done;   /* 位图：已出堆（Dijkstra 的最终值已定） */
    uint64_t  total;
    uint32_t  nprov;
} LmDist;

/* 跑一次一对多 Dijkstra。返回 0 成功；非 0 = 起点无效 / 内存不足。
   出堆/入堆计数放在 cnt 里（可为 NULL），仅供验收打印。 */
int  lm_dijkstra(Router *r, RNode from, LmDist *out,
                 uint64_t *n_pop, uint64_t *n_push);
void lm_dist_free(LmDist *d);

/* 取某个 (省, 节点) 的代价（已经乘过 LM_SCALE）。够不到返回 LM_UNREACHABLE。 */
uint32_t lm_cost_at(const LmDist *d, uint32_t prov, uint32_t node);

#endif
