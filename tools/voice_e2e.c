/* voice_e2e —— 语音播报**端到端**自测（走生产代码本身，不是副本）
 *
 * 为什么还要这个工具（voice_selftest 已经覆盖了策略层）：
 *   voice_selftest 测的是 route/voice.c 的**单个函数**（提前量、话术、冷却）。
 *   但真正容易出错的是**接线**：nav_voice_frame() 会不会
 *   · 用错"距下一条指令的距离"（to_go）
 *   · 用错"已走里程"（沿路告警扫描的基准）
 *   · 把播报顺序排错（官方是 speedLimit → arrival → alarm → turn）
 *   · 每帧重复播同一句（冷却没生效）
 *   · 把"到达收尾语"吞掉（★ 实测真 bug：GUI 里从来没响过那句）
 *   这些都是**跨模块**的错，单函数自测抓不到。
 *
 * ★★★ 本工具跑的就是**生产路径本身**：
 *     navsim_advance() → navsim_next_maneuver() → **voice_wire.c 的
 *     nav_voice_frame()** → nm_tts_speak()
 *   唯一不同的是"发声"的**观测方式**：GUI 让 TTS 引擎念，这里把
 *   NAVIMAP_TTS_DRYRUN 打开，让 nm_tts_speak() 同步把话术写进文件，
 *   于是判据断言的是"**窗口真正交出去的那句话**"。
 *   ⇒ 接线坏掉（发声循环断了、收尾语被吞了）自测必然变红。
 *   ★ 这一条是本工具存在的核心理由：上一版自测把接线**复刻**了一份，
 *     于是"把 window.c 的发声循环改成 i<0"这种注入照样全绿 ⇒ 判据假绿。
 *     （负向对照第 8 项就是抓这个的，现在改成直接注入 voice_wire.c。）
 *
 * ★ 两类用例的分工：
 *   A 类（navsim 驱动）：真·端到端。距离/速度由真实模拟推进产生，
 *      验证"接线的口径对不对"。
 *   B 类（直接喂 voice_tick）：把状态直接摆好，验证**精确的语义**
 *      （官方播报顺序、同文本冷却的 15 秒窗口）——这些在 A 类里
 *      要么被采样离散性干扰，要么根本构造不出同帧场景。
 *   ★ B 类调的是**生产的 voice_tick**，不是副本。
 *
 * ★ 为什么用**合成路线**而不是真实路网：
 *   真实路网的转向类型、路名、隧道位置每次构建都可能变 ⇒ 期望值写不稳。
 *   合成路线让"距下一条 300 m ⇒ 应当播「前方300 米左转」"这类断言完全确定。
 *   ★ 真实路网那一层由 voice_selftest 的**官方数值**覆盖
 *     （提前量 1437/275/83/3750 全部来自官方源码），两者互补。
 *
 * 用法：./voice_e2e            （跑全部用例，全过打印 VOICE-E2E-OK）
 */
#include "route/voice.h"
#include "route/navsim.h"
#include "route/maneuver.h"
#include "ui/voice_wire.h"
#include "utils/plat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "map/road_tags.h"

static int g_fail = 0;
#define CHECK(cond, msg) do {                                            \
    if (!(cond)) {                                                       \
        fprintf(stderr, "ASSERT-FAIL %s (%s:%d)\n", msg, __FILE__, __LINE__); \
        g_fail++;                                                       \
    }                                                                   \
} while (0)

/* ---------------------------------------------------------------------------
 * 合成一条路线：nseg 段，段长/限速可逐段指定。
 *
 * ★ 直线的意义：转向全靠我们**手工指定**的 ManeuverList，不依赖几何算出来的
 *   转向类型 ⇒ 期望值完全可控。段长/耗时用来喂 navsim（决定速度与里程）。
 * ------------------------------------------------------------------------- */
#define MAXPTS 64

typedef struct {
    RouteResult rr;
    ManeuverList ml;
    VoiceRouter  vr;                 /* 播报器状态（生产代码里的同一份） */
    double      lat[MAXPTS], lon[MAXPTS];
    int32_t     lat_e7[MAXPTS], lon_e7[MAXPTS];
    uint32_t    seg_m[MAXPTS];
    uint32_t    seg_ds[MAXPTS];
    uint8_t     edge_flags[MAXPTS];
    Maneuver    mans[MAXPTS];
} Fixture;

/* 造一条路线：len[i] = 第 i 段米数；v[i] = 第 i 段限速 km/h；
 * flags[i] = 该段边标志位（EF_TUNNEL / EF_TOLL / EF_BRIDGE）。 */
static void fx_build(Fixture *f, const double *len, const double *v,
                     const uint8_t *flags, int nseg)
{
    memset(f, 0, sizeof *f);
    int n = nseg + 1;
    if (n > MAXPTS) n = MAXPTS;
    nseg = n - 1;

    double lat0 = 39.90, lon0 = 116.40;
    double dlat = 1.0 / 111200.0;           /* 1 度纬度 ≈ 111200 m */
    double dlon = 1.0 / (111200.0 * 0.75);  /* 北京纬度 cos ≈ 0.75 */
    double acc = 0.0;

    for (int i = 0; i < n; i++) {
        f->lat[i]    = lat0 + dlat * acc;
        f->lon[i]    = lon0 + dlon * acc;
        f->lat_e7[i] = (int32_t)(f->lat[i] * 1e7);
        f->lon_e7[i] = (int32_t)(f->lon[i] * 1e7);
        if (i < nseg) acc += len[i];
    }
    double total_m = 0, total_s = 0;
    for (int i = 0; i < nseg; i++) {
        f->seg_m[i] = (uint32_t)len[i];
        f->seg_ds[i] = (uint32_t)(len[i] / (v[i] / 3.6) * 10.0);  /* 0.1 秒 */
        f->edge_flags[i] = flags ? flags[i] : 0;
        total_m += len[i];
        total_s += f->seg_ds[i] / 10.0;
    }
    f->rr.ok         = 1;
    f->rr.n          = (uint32_t)n;
    f->rr.total_m    = total_m;
    f->rr.total_s    = total_s;
    f->rr.lat_e7     = f->lat_e7;
    f->rr.lon_e7     = f->lon_e7;
    f->rr.seg_m      = f->seg_m;
    f->rr.seg_ds     = f->seg_ds;
    f->rr.edge_flags = f->edge_flags;
}

/* 造一段等长同速的路线（最常用的形态）。 */
static void fx_uniform(Fixture *f, int nseg, double len_m, double v_kmh,
                       const uint8_t *flags)
{
    double len[MAXPTS], v[MAXPTS];
    for (int i = 0; i < nseg; i++) { len[i] = len_m; v[i] = v_kmh; }
    fx_build(f, len, v, flags, nseg);
}

/* 手工指定指令：type[i] 与 road[i]；gap[i] = 第 i 条距第 i-1 条的路程。 */
static void fx_maneuvers(Fixture *f, const int *type, const char *const *road,
                         const double *gap, int n)
{
    f->ml.v = f->mans;
    f->ml.n = n;
    f->ml.cap = MAXPTS;
    double sum = 0;
    for (int i = 0; i < n; i++) {
        memset(&f->mans[i], 0, sizeof f->mans[i]);
        f->mans[i].type   = type[i];
        f->mans[i].idx    = (uint32_t)i;
        f->mans[i].dist_m = (i == 0) ? 0.0 : gap[i];
        if (road && road[i]) {
            size_t l = strlen(road[i]);
            if (l >= sizeof f->mans[i].road) l = sizeof f->mans[i].road - 1;
            memcpy(f->mans[i].road, road[i], l);
            f->mans[i].road[l] = '\0';
        }
        sum += f->mans[i].dist_m;
    }
    f->ml.sum_m = sum;
}

/* ---------------------------------------------------------------------------
 * 记录器：把"窗口真正交出去的话"收集起来。
 *
 * ★ 怎么捕获：nm_tts_speak() 在 NAVIMAP_TTS_DRYRUN 指向的文件里**同步**
 *   追加一行（见 plat.c 的 tts_dryrun_write）⇒ 每帧播报完立刻读文件，
 *   拿到的就是这一帧真正发出去的话，顺序也一样。
 *   这比"在 e2e 里复刻一遍接线再收集"强得多：复刻版和实现可以一起错。
 * ------------------------------------------------------------------------- */
#define MAXLOG 64
typedef struct {
    char   line[MAXLOG][192];
    double t[MAXLOG];          /* 该句播出的模拟时刻（秒） */
    int    n;
} Log;

static char g_dry_path[1024];
static long g_dry_off = 0;    /* 已读到的字节偏移 */

static void dry_reset(void);

static void dry_open(void)
{
    snprintf(g_dry_path, sizeof g_dry_path, "%s/navimap_voice_e2e_%d.txt",
             nm_temp_dir(), (int)nm_get_pid());
    nm_setenv("NAVIMAP_TTS_DRYRUN", g_dry_path);
    dry_reset();
}

/* 清空 dryrun 文件（每个用例开始前调）。 */
static void dry_reset(void)
{
    FILE *f = fopen(g_dry_path, "wb");
    if (f) fclose(f);
    g_dry_off = 0;
}

/* 把 dryrun 文件里新增的行收进 lg。 */
static void dry_collect(Log *lg, double t)
{
    FILE *f = fopen(g_dry_path, "rb");
    if (!f) return;
    if (fseek(f, g_dry_off, SEEK_SET) != 0) { fclose(f); return; }
    char buf[512];
    while (fgets(buf, sizeof buf, f)) {
        size_t l = strlen(buf);
        while (l > 0 && (buf[l - 1] == '\n' || buf[l - 1] == '\r')) buf[--l] = '\0';
        if (!l) continue;
        if (lg->n >= MAXLOG) break;
        snprintf(lg->line[lg->n], sizeof lg->line[0], "%s", buf);
        lg->t[lg->n] = t;
        lg->n++;
    }
    g_dry_off = ftell(f);
    fclose(f);
}

/* 整个序列里是否出现过含 sub 的句子 */
static int log_has(const Log *lg, const char *sub)
{
    for (int i = 0; i < lg->n; i++)
        if (strstr(lg->line[i], sub)) return 1;
    return 0;
}

/* 出现次数 */
static int log_count(const Log *lg, const char *sub)
{
    int c = 0;
    for (int i = 0; i < lg->n; i++)
        if (strstr(lg->line[i], sub)) c++;
    return c;
}

/* ---------------------------------------------------------------------------
 * 从一句话术里取出"前方"的距离（米）。
 *
 * ★ 距离怎么从话术里取出来：
 *   不能用"跳过'前方'两个字符"的办法 —— 本文件是 UTF-8，"前"占 3 字节，
 *   按字符跳会跳到半个字上（实测踩过：atof 读到 0，距离判据全假红）。
 *   ⇒ 从 "前方" 之后的第一个数字字符开始用 strtod 解析。
 *   返回 1 = 解析成功（*out 有值）；0 = 这句没有距离（"现在左转"那种）。
 *
 * ★ 单位换算：官方 Units.spoken() 在 >=1000 m 时用"公里"（一位小数），
 *   这里换算回米，好让判据能跨单位比较。 */
static int line_dist(const char *line, double *out)
{
    const char *p = strstr(line, "前方");
    if (!p) return 0;
    const char *q = p;
    while (*q && !((*q >= '0' && *q <= '9') || *q == '.')) q++;
    if (!*q) return 0;
    double v = strtod(q, NULL);
    /* 单位：后面紧跟 "公里" ⇒ 换算成米 */
    const char *u = q;
    while (*u && *u != ' ') u++;          /* 跳过数字本身 */
    if (strstr(u, "公里")) v *= 1000.0;
    if (out) *out = v;
    return 1;
}

/* ---------------------------------------------------------------------------
 * 找"含 sub、且前面距离落在 [lo,hi] 米"的句子。
 *
 * ★ 为什么不能把距离写死成"500 米"：
 *   navsim 是**按时间步**推进的（0.5 s × 16.67 m/s ≈ 8.3 m/步），
 *   所以第一次进入某个层级时的距离是 490 而不是 500 —— 官方同理
 *   （它的 Navigator 也是 200 ms 一 tick）。判据必须允许这个离散量，
 *   否则就是"判据在测采样相位"而不是在测行为。
 *   ⇒ 判据用"落在合理区间内"，而不是逐字相等。 */
static int log_has_dist(const Log *lg, const char *sub, double lo, double hi)
{
    for (int i = 0; i < lg->n; i++) {
        if (!strstr(lg->line[i], sub)) continue;
        double v;
        if (!line_dist(lg->line[i], &v)) continue;
        if (v >= lo && v <= hi) return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * 推进模拟车并逐帧收集播报 —— **走生产接线**。
 *
 * ★★★ 这里的帧循环与 src/ui/window.c 的 app_nav_sim_tick() **同一条路径**：
 *     navsim_advance → navsim_locate → nav_voice_frame → nm_tts_speak
 *   差别只有两处：
 *     · 时钟是合成的（now_ms 由步长累加），冷却判据可复现；
 *     · 发声被 DRYRUN 截下来供断言（GUI 里是真念出来）。
 *   ★ 顺序照抄：到达那一帧**先播报后置 paused**（见 window.c 的注释），
 *     否则"您已到达目的地附近，导航结束"会被吞掉。
 * ------------------------------------------------------------------------- */
static void run_sim(Fixture *f, double dt, double max_seconds, Log *lg)
{
    NavSim sim;
    if (navsim_start(&sim, &f->rr) != 0) return;
    sim.speed_mul = 1.0;

    uint8_t ann_finished = 0;
    NavVoiceCtx c;
    c.vr           = &f->vr;
    c.rr           = &f->rr;
    c.man          = &f->ml;
    c.sim          = &sim;
    c.voice_on     = 1;
    c.paused       = 0;
    c.ann_finished = &ann_finished;

    uint64_t now_ms = 1000;
    double t = 0.0;
    while (t < max_seconds) {
        int arrived = navsim_advance(&sim, dt);
        t += dt;
        now_ms += (uint64_t)(dt * 1000.0);
        /* 车头就是定位点（照抄 window.c：模拟 GNSS 永远给出"我在路上"的位置） */
        navsim_locate(&sim, sim.car_lat, sim.car_lon);

        nav_voice_frame(&c, now_ms);
        dry_collect(lg, t);

        if (arrived) {
            /* ★ 照抄 window.c：先播报、后置 paused。 */
            c.paused = 1;
            break;
        }
    }
}

static void dump(const char *tag, const Log *lg)
{
    printf("--- %s 播报序列（%d 句）---\n", tag, lg->n);
    for (int i = 0; i < lg->n; i++)
        printf("  [%d] t=%.1fs %s\n", i, lg->t[i], lg->line[i]);
}

/* ===========================================================================
 * A 类：navsim 驱动的真·端到端用例（走生产接线 nav_voice_frame）
 * ========================================================================= */

static void case_a1_turn_levels(void)
{
    /* 路线：4 段 × 500 m = 2000 m，限速 60 km/h。
     * 指令：起点(0) → 左转(距起点 500 m) → 到达(距左转 1500 m)。
     *
     * 期望（官方口径，60 km/h = 16.67 m/s）：
     *   · PREPARE = 1437 × 16.67/12.5 = 1916 m ⇒ 距左转 500 m 已在提前级
     *   · TURN_IN = 275 × 16.67/12.5 = 367 m ⇒ 逼近时再播一次
     *   · TURN_NOW = 83 × 16.67/12.5 = 111 m ⇒ 播"现在左转"
     *   · 到达：剩余 <200 m 播"前方即将到达目的地"
     */
    Fixture f;
    fx_uniform(&f, 4, 500.0, 60.0, NULL);
    int type[3] = { MAN_START, MAN_LEFT, MAN_ARRIVE };
    const char *road[3] = { "起始路", "建国路", NULL };
    double gap[3] = { 0.0, 500.0, 1500.0 };
    fx_maneuvers(&f, type, road, gap, 3);
    voice_router_reset(&f.vr);

    Log lg;
    memset(&lg, 0, sizeof lg);
    dry_reset();
    run_sim(&f, 0.5, 200.0, &lg);      /* 60 km/h 跑完 2000 m 要 120 秒 */
    dump("A1", &lg);

    CHECK(lg.n > 0, "A1 至少应播一句");
    CHECK(log_has_dist(&lg, "左转，进入建国路", 400.0, 520.0),
          "A1 距左转约 500 m 应播提前级\"前方…米左转，进入建国路\"");
    CHECK(log_has(&lg, "现在左转，进入建国路"),
          "A1 距左转 <=111 m 应播立即级\"现在左转，进入建国路\"");
    CHECK(log_has(&lg, "前方即将到达目的地"),
          "A1 剩余 <200 m 应播\"前方即将到达目的地\"");

    /* ★ 层级递进：转向恰好播 3 句（提前/临近/立即各一次），距离单调递减。
     *   这是官方"层级只升不降"规则的端到端体现。 */
    int n_left = 0, mono = 1;
    double d_prev = 1e9;
    for (int i = 0; i < lg.n; i++) {
        if (!strstr(lg.line[i], "左转，进入建国路")) continue;
        n_left++;
        double v;
        if (line_dist(lg.line[i], &v)) {
            if (v >= d_prev) mono = 0;      /* 距离必须越来越小 */
            d_prev = v;
        }
    }
    CHECK(n_left == 3, "A1 转向应播 3 句（提前/临近/立即各一次）");
    CHECK(mono == 1, "A1 转向播报的距离应单调递减（层级递进）");
    CHECK(log_count(&lg, "现在左转") == 1, "A1 \"现在左转\"只播一次");
    CHECK(log_count(&lg, "前方即将到达目的地") == 1, "A1 到达播报只播一次");
}

static void case_a2_alarm_instance(void)
{
    /* 两条**长度不同**的隧道（A = 500..1000，长 500 m，B = 1300..1600，长 300 m），
     * 限速 108 km/h。
     *
     * ★ 抓的是"告警按**实例**去重"这条规则（官方 IdentityHashMap 语义）：
     *   早期版本按**类型**记 ⇒ 第二个隧道永远不播。
     *   判据：两条隧道的全长都要出现在话术里（"全长 500 米" 与 "全长 300 米"）。
     *   ★ 为什么两条隧道要**不同长**：官方 Alarm.phrase() 的近级句
     *   "进入隧道，全长 N 米" **不带距离** ⇒ 等长的话两条隧道的近级句
     *   逐字相同，就会和"同文本冷却"搅在一起，判据分不清是哪个规则在起作用。
     *   不同长 ⇒ 两句文本必然不同 ⇒ 判据只测实例去重这一条规则。
     */
    Fixture f;
    double len[5] = { 500, 500, 300, 300, 400 };   /* 隧道A 500..1000 / 隧道B 1300..1600 */
    double v[5]   = { 108, 108, 108, 108, 108 };
    uint8_t flags[5] = { 0, EF_TUNNEL, 0, EF_TUNNEL, 0 };
    fx_build(&f, len, v, flags, 5);
    int type[3] = { MAN_START, MAN_LEFT, MAN_ARRIVE };
    const char *road[3] = { "起始路", "建国路", NULL };
    double gap[3] = { 0.0, 500.0, 1500.0 };
    fx_maneuvers(&f, type, road, gap, 3);
    voice_router_reset(&f.vr);

    Log lg;
    memset(&lg, 0, sizeof lg);
    dry_reset();
    run_sim(&f, 0.25, 200.0, &lg);
    dump("A2", &lg);

    int n_tun = log_count(&lg, "进入隧道");
    CHECK(n_tun >= 3,
          "A2 两条隧道都应播报（告警按实例去重，不是按类型）");
    CHECK(n_tun <= 4, "A2 隧道播报不应超过 4 句（每条隧道远级+近级各一次）");
    CHECK(log_has(&lg, "全长 500 米"),
          "A2 第一条隧道（500 m）应播报");
    CHECK(log_has(&lg, "全长 300 米"),
          "A2 第二条隧道（300 m）应播报（按类型去重的话这条会缺）");

    /* ★ 官方同文本冷却的**通用不变量**：官方 say() 只与"上一句"比较，
     *   所以真正要保证的是"**相邻两句逐字相同**时，时间差 >= 15 秒"。
     *   （非相邻的同文本是允许的：中间的别的句子已经把 lastText 顶掉了。）
     *   这条不变量在所有 A 类用例里都成立 —— 这里统一查一遍。 */
    for (int i = 1; i < lg.n; i++) {
        if (strcmp(lg.line[i], lg.line[i - 1]) == 0) {
            CHECK(lg.t[i] - lg.t[i - 1] >= 15.0,
                  "A2 相邻两句逐字相同时时间差必须 >=15 秒（官方同文本冷却）");
        }
    }
}

static void case_a3_merge(void)
{
    /* 相邻两路口只隔 100 m ⇒ 官方合并播报"…然后紧接着右转"。 */
    Fixture f;
    fx_uniform(&f, 4, 500.0, 60.0, NULL);
    int type[3] = { MAN_START, MAN_LEFT, MAN_RIGHT };
    const char *road[3] = { "起始路", "建国路", NULL };
    double gap[3] = { 0.0, 500.0, 100.0 };
    fx_maneuvers(&f, type, road, gap, 3);
    voice_router_reset(&f.vr);

    Log lg;
    memset(&lg, 0, sizeof lg);
    dry_reset();
    run_sim(&f, 0.5, 200.0, &lg);
    dump("A3", &lg);

    CHECK(log_has(&lg, "然后紧接着右转"),
          "A3 相邻路口 <150 m 应合并播报（官方措辞）");
}

static void case_a4_speed_limit(void)
{
    /* 两段不同限速（60 → 30 km/h）：官方 speedLimit() 在变化 >=10 km/h
     * 时播"前方限速 30 公里每小时"。 */
    Fixture f;
    double len[4] = { 500, 500, 500, 500 };
    double v[4]   = { 60, 60, 30, 30 };
    fx_build(&f, len, v, NULL, 4);
    int type[3] = { MAN_START, MAN_LEFT, MAN_ARRIVE };
    const char *road[3] = { "起始路", "建国路", NULL };
    double gap[3] = { 0.0, 500.0, 1500.0 };
    fx_maneuvers(&f, type, road, gap, 3);
    voice_router_reset(&f.vr);

    Log lg;
    memset(&lg, 0, sizeof lg);
    dry_reset();
    run_sim(&f, 0.5, 300.0, &lg);
    dump("A4", &lg);

    CHECK(log_has(&lg, "前方限速 30 公里每小时"),
          "A4 限速从 60 降到 30 应播限速提示");
    CHECK(log_count(&lg, "前方限速") == 1,
          "A4 限速只变化一次 ⇒ 限速提示只播一次");
}

static void case_a5_tunnel(void)
{
    /* 隧道告警接线：告警扫描的基准必须是"已走里程"。
     * 若把剩余里程当已走里程（或基准算错），就永远扫不到隧道。 */
    Fixture f;
    uint8_t flags[4] = { 0, EF_TUNNEL, 0, 0 };
    fx_uniform(&f, 4, 500.0, 60.0, flags);
    int type[3] = { MAN_START, MAN_LEFT, MAN_ARRIVE };
    const char *road[3] = { "起始路", "建国路", NULL };
    double gap[3] = { 0.0, 500.0, 1500.0 };
    fx_maneuvers(&f, type, road, gap, 3);
    voice_router_reset(&f.vr);

    Log lg;
    memset(&lg, 0, sizeof lg);
    dry_reset();
    run_sim(&f, 0.5, 200.0, &lg);
    dump("A5", &lg);

    CHECK(log_has(&lg, "进入隧道"), "A5 前方有隧道时应播隧道告警");
    CHECK(log_has(&lg, "全长 500 米"), "A5 隧道告警应带全长（官方 Alarm.phrase）");
}

static void case_a6_arrive_once(void)
{
    /* 到终点后窗口每帧仍会调 nav_voice_frame ⇒ 到达播报只能有一次。 */
    Fixture f;
    fx_uniform(&f, 3, 400.0, 60.0, NULL);
    int type[2] = { MAN_START, MAN_ARRIVE };
    const char *road[2] = { "起始路", NULL };
    double gap[2] = { 0.0, 1200.0 };
    fx_maneuvers(&f, type, road, gap, 2);
    voice_router_reset(&f.vr);

    Log lg;
    memset(&lg, 0, sizeof lg);
    dry_reset();
    run_sim(&f, 0.5, 300.0, &lg);
    dump("A6", &lg);

    CHECK(log_count(&lg, "前方即将到达目的地") == 1,
          "A6 到达播报在整段行程里只出现一次");
    CHECK(log_count(&lg, "前方1200 米到达目的地") <= 1,
          "A6 到达转向句不应反复播");
}

/* ★★★ A7：终点收尾语（官方 NavigationDialogs.stopNavigation 的 arrived 分支）
 *
 * 这条锁住一个**实测真 bug**：window.c 的 app_nav_sim_tick() 原先是
 * "先把 sim_paused 置 1、再调 nav_announce()"，而 nav_announce() 开头
 * 有 `if (sim_paused) return;` ⇒ "您已到达目的地附近，导航结束"
 * **永远播不出来**（GUI 里从来没响过这句）。
 *
 * 现在两处都堵上了：window.c 改成"先播报后置位"，voice_wire.c 里
 * 收尾语单独判、不受 paused 影响。
 */
static void case_a7_finish_phrase(void)
{
    Fixture f;
    fx_uniform(&f, 3, 400.0, 60.0, NULL);
    int type[2] = { MAN_START, MAN_ARRIVE };
    const char *road[2] = { "起始路", NULL };
    double gap[2] = { 0.0, 1200.0 };
    fx_maneuvers(&f, type, road, gap, 2);
    voice_router_reset(&f.vr);

    Log lg;
    memset(&lg, 0, sizeof lg);
    dry_reset();
    run_sim(&f, 0.5, 300.0, &lg);
    dump("A7", &lg);

    CHECK(log_count(&lg, "您已到达目的地附近，导航结束") == 1,
          "A7 到达终点应播一次收尾语\"您已到达目的地附近，导航结束\"");
}

/* ★★★ A8：收尾语**不受暂停影响**
 *
 * 官方 stopNavigation(arrived) 的那句是在"到达"这一刻说的，与"暂停"
 * 无关。本项目到达时会把 sim_paused 置 1（模拟停下）⇒ 如果收尾语也
 * 挡在 paused 后面，它就永远出不来（就是 A7 那个 bug 的另一面）。
 *
 * 这里直接把状态摆成"已到达 + 已暂停"，断言收尾语**仍然**要播，
 * 而且只播一次（ann_finished 负责去重）。
 */
static void case_a8_finish_not_blocked_by_pause(void)
{
    Fixture f;
    fx_uniform(&f, 2, 200.0, 60.0, NULL);
    int type[2] = { MAN_START, MAN_ARRIVE };
    const char *road[2] = { "起始路", NULL };
    double gap[2] = { 0.0, 400.0 };
    fx_maneuvers(&f, type, road, gap, 2);
    voice_router_reset(&f.vr);

    NavSim sim;
    CHECK(navsim_start(&sim, &f.rr) == 0, "A8 navsim_start 应成功");
    sim.arrived = 1;

    uint8_t ann_finished = 0;
    NavVoiceCtx c;
    c.vr           = &f.vr;
    c.rr           = &f.rr;
    c.man          = &f.ml;
    c.sim          = &sim;
    c.voice_on     = 1;
    c.paused       = 1;             /* ★ 到达时窗口会同时把模拟停下 */
    c.ann_finished = &ann_finished;

    Log lg;
    memset(&lg, 0, sizeof lg);
    dry_reset();

    nav_voice_frame(&c, 5000);
    dry_collect(&lg, 5.0);
    dump("A8-第1次", &lg);
    CHECK(log_count(&lg, "您已到达目的地附近，导航结束") == 1,
          "A8 已到达 + 已暂停时，收尾语仍然必须播出来");

    /* 再调一帧 ⇒ 不许重复播 */
    nav_voice_frame(&c, 6000);
    dry_collect(&lg, 6.0);
    CHECK(log_count(&lg, "您已到达目的地附近，导航结束") == 1,
          "A8 收尾语只播一次（ann_finished 去重）");
}

/* ★★★ A9：暂停时**常规**播报要闭嘴
 *
 * 与 A8 互补：暂停（偏航重算期间）不该出声，但收尾语例外。
 * ⇒ 判据：paused=1 且未到达 ⇒ 一句都不许发。
 */
static void case_a9_pause_silences_regular(void)
{
    Fixture f;
    fx_uniform(&f, 2, 200.0, 60.0, NULL);
    int type[2] = { MAN_START, MAN_LEFT };
    const char *road[2] = { "起始路", "建国路" };
    double gap[2] = { 0.0, 200.0 };
    fx_maneuvers(&f, type, road, gap, 2);
    voice_router_reset(&f.vr);

    NavSim sim;
    navsim_start(&sim, &f.rr);

    uint8_t ann_finished = 0;
    NavVoiceCtx c;
    c.vr           = &f.vr;
    c.rr           = &f.rr;
    c.man          = &f.ml;
    c.sim          = &sim;
    c.voice_on     = 1;
    c.paused       = 1;
    c.ann_finished = &ann_finished;

    Log lg;
    memset(&lg, 0, sizeof lg);
    dry_reset();
    nav_voice_frame(&c, 5000);
    dry_collect(&lg, 5.0);
    dump("A9", &lg);
    CHECK(lg.n == 0, "A9 暂停时不该有任何常规播报");
}

/* ===========================================================================
 * B 类：直接喂 voice_tick 的精确语义用例
 * ========================================================================= */

static void case_b1_order(void)
{
    /* 官方 VoiceRouter.update() 的顺序：
     *     speedLimit → arrival → alarm → turn
     * 一帧里四类都成立时，先播哪句由这个顺序决定。
     *
     * ★ 构造：先用一个"什么都不播"的调用把限速档位顶到 120 km/h
     *   （官方：首次不播，只记档位），再喂一个四类同时成立的状态。
     *   · 限速：120 → 50，变化 70 >= 10 ⇒ 播
     *   · 到达：剩余 120 m < 200 ⇒ 播
     *   · 告警：隧道在前方 20 m ⇒ 播（近级）
     *   · 转向：距左转 50 m ⇒ 播"现在左转"
     */
    Fixture f;
    double len[4] = { 300, 300, 300, 100 };
    double v[4]   = { 60, 60, 60, 60 };
    uint8_t flags[4] = { 0, 0, 0, EF_TUNNEL };   /* 隧道 900..1000，长 100 m */
    fx_build(&f, len, v, flags, 4);
    int type[3] = { MAN_START, MAN_LEFT, MAN_ARRIVE };
    const char *road[3] = { "起始路", "建国路", NULL };
    double gap[3] = { 0.0, 500.0, 1500.0 };
    fx_maneuvers(&f, type, road, gap, 3);
    voice_router_reset(&f.vr);

    /* 预热：把限速档位顶到 120 km/h（1200 m 外，不会触发任何播报） */
    {
        VoiceFrame vf;
        voice_tick(&f.vr, &f.rr, &f.ml, 1, 20000.0, 33.0, 5000.0, 0, 1000, &vf);
        CHECK(vf.n == 0, "B1 预热调用不应播报（全部条件都不成立）");
    }

    /* 正戏：四类同时成立 */
    VoiceFrame vf;
    voice_tick(&f.vr, &f.rr, &f.ml, 1, 50.0, 13.9, 120.0, 0, 5000, &vf);
    printf("--- B1 一帧播报（%d 句）---\n", vf.n);
    for (int i = 0; i < vf.n; i++) printf("  [%d] %s\n", i, vf.text[i]);

    CHECK(vf.n == 4, "B1 一帧应播 4 句（限速/到达/告警/转向）");
    if (vf.n == 4) {
        CHECK(strcmp(vf.text[0], "前方限速 50 公里每小时") == 0,
              "B1 第 1 句应是限速（官方顺序 speedLimit 最先）");
        CHECK(strcmp(vf.text[1], "前方即将到达目的地") == 0,
              "B1 第 2 句应是到达（官方顺序 arrival 在 alarm 之前）");
        CHECK(strstr(vf.text[2], "进入隧道") != NULL,
              "B1 第 3 句应是告警（官方顺序 alarm 在 turn 之前）");
        CHECK(strstr(vf.text[3], "现在左转") != NULL,
              "B1 第 4 句应是转向（官方把转向放最后）");
    }
}

static void case_b2_same_text_cooldown(void)
{
    /* 官方 say() 的同文本冷却（15 秒）对**所有**类别生效：
     *     if (t.equals(lastText) && now - lastTextAt < 15000) 丢弃
     *
     * ★ 三个收费站（里程 500 / 700 / 900，各 200 m 一段），
     *   逐个从正前方 60 m 处经过。官方"点状告警"取**段起点**，
     *   近级句是 AlarmType.say 整句 "前方有收费站"（**不带距离**）
     *   ⇒ 三次的近级句**逐字相同**。
     *   · 第 1 次（t=1000 ms）⇒ 播
     *   · 第 2 次（t=5000 ms）⇒ 距上次 4 s < 15 s ⇒ **被丢弃**
     *   · 第 3 次（t=20000 ms）⇒ 距上次 19 s > 15 s ⇒ 播
     *
     * ★ 这条判据抓的是"同文本冷却**没有覆盖告警**"这个 bug：
     *   去掉冷却后第 2 次会播出来 ⇒ 断言失败。
     *   ★ 而"告警 3000 ms 错开"这条冷却**不会**误伤它
     *   （5000 − 1000 = 4000 > 3000）⇒ 两次冷却被干净地区分开了。
     */
    Fixture f;
    double len[4] = { 500, 200, 200, 1100 };
    double v[4]   = { 108, 108, 108, 108 };
    uint8_t flags[4] = { 0, EF_TOLL, EF_TOLL, EF_TOLL };
    fx_build(&f, len, v, flags, 4);
    int type[2] = { MAN_START, MAN_ARRIVE };
    const char *road[2] = { "起始路", NULL };
    double gap[2] = { 0.0, 2000.0 };
    fx_maneuvers(&f, type, road, gap, 2);
    voice_router_reset(&f.vr);

    /* 剩余里程 → 已走里程：along = total_m − remain_m。
     *   收费站 500 → along 440 ⇒ remain 1560
     *   收费站 700 → along 640 ⇒ remain 1360
     *   收费站 900 → along 840 ⇒ remain 1160 */
    struct { double remain; uint64_t now; int want; const char *tag; } cases[3] = {
        { 1560.0,  1000, 1, "第1次（应播）" },
        { 1360.0,  5000, 0, "第2次（同文本 15s 冷却内，应丢）" },
        { 1160.0, 20000, 1, "第3次（冷却过期，应播）" },
    };

    for (int k = 0; k < 3; k++) {
        VoiceFrame vf;
        /* to_go 给 20000（不触发转向）；速度固定 ⇒ 限速不播；
         * 剩余 >200 ⇒ 到达不播。⇒ 唯一可能的输出就是告警。 */
        voice_tick(&f.vr, &f.rr, &f.ml, 1, 20000.0, 30.0, cases[k].remain,
                   0, cases[k].now, &vf);
        printf("--- B2 %s：%d 句 ---\n", cases[k].tag, vf.n);
        for (int i = 0; i < vf.n; i++) printf("  [%d] %s\n", i, vf.text[i]);
        CHECK(vf.n == cases[k].want,
              "B2 同文本冷却：这一次的播报条数不对");
        if (vf.n > 0)
            CHECK(strcmp(vf.text[0], "前方有收费站") == 0,
                  "B2 告警文案应是官方 AlarmType.say 整句");
    }
}

int main(void)
{
    dry_open();

    case_a1_turn_levels();
    case_a2_alarm_instance();
    case_a3_merge();
    case_a4_speed_limit();
    case_a5_tunnel();
    case_a6_arrive_once();
    case_a7_finish_phrase();
    case_a8_finish_not_blocked_by_pause();
    case_a9_pause_silences_regular();
    case_b1_order();
    case_b2_same_text_cooldown();

    nm_setenv("NAVIMAP_TTS_DRYRUN", "");
    nm_unlink(g_dry_path);

    if (g_fail == 0) { printf("VOICE-E2E-OK\n"); return 0; }
    fprintf(stderr, "VOICE-E2E FAILED: %d 项\n", g_fail);
    return 1;
}
