#ifndef NAVIMAP_VOICE_H
#define NAVIMAP_VOICE_H

#include <stddef.h>
#include <stdint.h>
#include "route/maneuver.h"
#include "route/route.h"

/* ---------------------------------------------------------------------------
 * 语音播报策略（抄写自 OsmAnd）
 *
 * ★ 分层抄，每层各取一个官方源（都在 E:\Osmand 里）：
 *
 *   1) 分级提前量  ← OsmAnd/OsmAnd/src/net/osmand/plus/routing/data/
 *                     AnnounceTimeDistances.java
 *                  官方唯一的"速度 × 秒数"模型（nav-desktop 自己的注释也写着
 *                  "对标官方 VoiceRouter + AnnounceTimeDistances"）。
 *   2) 中文话术    ← nav-desktop/src/main/osmand/nav/route/VoiceRouter.java
 *                    + nav-desktop/src/main/osmand/nav/model/Alarm.java
 *                    + nav-desktop/src/main/osmand/nav/model/AlarmType.java
 *                  这是 OsmAnd 仓库里**唯一的中文桌面语音导航实现**（纯 Java、
 *                  无 Android 依赖），与本项目同构：同样的"分级 / 路口合并 /
 *                  同文本去抖 / 告警两级 / 限速去抖"。而且它的 action() 词表
 *                  与本项目面板文案（maneuver_kind_text）逐条对得上
 *                  ⇒ 面板显示什么，语音就说什么。
 *   3) 沿路告警    ← nav-desktop/src/main/osmand/nav/route/Alarms.java
 *                    + nav-desktop/src/main/osmand/nav/route/Navigator.java
 *                    （findAlarm）+ nav-desktop/src/main/osmand/nav/core/Units.java
 *
 * 为什么值得抄：原来的播报是"固定 300m / 60m 两级 + 一句直白文案"，
 * 高速上根本来不及提醒（120 km/h 时 300 m 只剩 9 秒），而且没有"现在左转"、
 * 没有相邻路口合并、没有去抖 ⇒ 连续两个路口会连着刷屏。OsmAnd 的模型是
 * "**提前量 = 速度 × 秒数**"：距路口的距离随车速自动放大，快路上提前说、
 * 慢路上不啰嗦。
 *
 * 本模块是**纯逻辑、零平台依赖**：只决定"该说什么"，不碰发声。
 * 发声在 utils/plat.c（nm_tts_speak）。这样判据能在 CLI 里无头跑。
 * ------------------------------------------------------------------------- */

/* ---- 播报层级 ----
 * 数值越大 = 越接近路口 = 越该说。VOICE_NONE = 不播。
 * ★ 判定顺序必须"从最紧急往回查"：命中就返回**最紧急**的那一级。
 *   （反过来查会把"超远预告"当成"提前预告"返回 ⇒ 高速下提前量错判。） */
enum {
    VOICE_NONE = -1,
    VOICE_PREPARE_LONG = 0,   /* 超远预告：只给高速路（官方 issue 1411） */
    VOICE_PREPARE      = 1,   /* 提前预告："前方 800 米左转" */
    VOICE_TURN_IN      = 2,   /* 临近："前方 200 米左转" */
    VOICE_TURN_NOW     = 3,   /* 立即："现在左转" */
};

/* ---- 沿路告警类型（抄 AlarmType.java）----
 * 本项目路网里没有测速摄像头那类**点标注**（OSM 标签没进 products），
 * 只有隧道/收费站/桥梁这类**边标志位**能认（EF_TUNNEL/EF_TOLL/EF_BRIDGE），
 * 所以只抄能落地的那几类（词表仍保留全部官方类型，便于话术对照与将来接数据）。 */
enum {
    VOICE_ALARM_NONE = 0,
    VOICE_ALARM_TUNNEL,       /* 隧道（官方 TUNNEL） */
    VOICE_ALARM_TOLL,         /* 收费站（官方 TOLL_BOOTH） */
    VOICE_ALARM_BRIDGE,       /* 桥梁（本项目扩展：官方 AlarmType 无此项） */
    VOICE_ALARM_SPEED_CAMERA, /* 测速摄像头 */
    VOICE_ALARM_STOP,         /* 停车让行 */
    VOICE_ALARM_RAILWAY,      /* 铁路道口 */
    VOICE_ALARM_PEDESTRIAN,   /* 人行横道 */
    VOICE_ALARM_COUNT
};

/* 官方 AlarmType.priority 的数值（越小越优先；官方 Alarms.priority() 用它排序）。
 *   TOLL_BOOTH = 6、TUNNEL = 11 —— 官方原值。
 *   BRIDGE 是本项目扩展，取 12（隧道与桥梁都是"区间型"，量级相当）。 */
#define VOICE_ALARM_PRIO_TOLL      6
#define VOICE_ALARM_PRIO_TUNNEL   11
#define VOICE_ALARM_PRIO_BRIDGE   12
/* 官方 AlarmType.MAXIMUM.priority = 10（"加塞提示"那一档，见 Alarms.priority）。 */
#define VOICE_ALARM_PRIO_MAXIMUM  10

/* ---------------------------------------------------------------------------
 * 1) 分级提前量（抄 AnnounceTimeDistances.java）
 *
 * 官方用"速度 × 秒数"：PREPARE = speed*115s、TURN_IN = speed*22s、
 * TURN_NOW = speed*min(sqrt(speed*3.6), 8)（约 6.7 秒）。
 * 本项目没有"应用模式"（车/自行车/步行），只有一个默认速度，
 * 所以把官方的**汽车档**数值固定下来，同时保留"按当前速度放大"的行为：
 * 官方 isDistanceLess() 是 dist <= max(leadDist, curSpeed/DEFAULT_SPEED*leadDist)
 * —— 车速比默认快，提前量按比例放大。这里照抄这个比例放大。
 * ------------------------------------------------------------------------- */

/* 默认速度（m/s）：官方 **car 档**的 DEFAULT_SPEED = 12.5 m/s（45 km/h）。
 * 官方出处：OsmandSettings.DEFAULT_SPEED.setModeDefaultValue(CAR, 12.5f)。
 * 官方注释里的 car 档数值都由它推出：PREPARE_LONG 3750 m / PREPARE 1438 m /
 * TURN_IN 275 m / TURN_NOW 83 m —— 本模块的公式与之逐项对齐。 */
#define VOICE_DEFAULT_SPEED_MS   12.5
/* 官方 PREPARE = speed*115s（car 1438 m）。 */
#define VOICE_PREPARE_SEC        115.0
/* 官方 PREPARE_LONG = speed*300s（car 3750 m）。 */
#define VOICE_PREPARE_LONG_SEC   300.0
/* 官方 TURN_IN = speed*22s（car 275 m）。 */
#define VOICE_TURN_IN_SEC        22.0
/* 官方 TURN_NOW ≈ speed*min(sqrt(speed*3.6),8) ⇒ car 83 m。
 * 这个常量只是"无速度时的兜底"，实际值按 sqrt 公式算（见 voice.c）。 */
#define VOICE_TURN_NOW_SEC       6.7
/* 官方 POSITIONING_TOLERANCE = 12 m（GPS 定位误差兜底）。 */
#define VOICE_POS_TOLERANCE_M    12.0
/* 官方高速门槛：speed >= 30 m/s 才播 PREPARE_LONG（issue 1411）。 */
#define VOICE_FAST_SPEED_MS      30.0
/* 到达播报距离（抄 nav-desktop VoiceRouter.arrival() 的 distRemaining < 200）。 */
#define VOICE_ARRIVE_M           200.0
/* 相邻两个路口距离小于此值 ⇒ 合并成一句播报（官方 VoiceRouter.turnPhrase 的 150 m）。 */
#define VOICE_MERGE_M            150.0

/* 当前状态下的**基准**分级提前量（米）。返回 4 个阈值，依次是
 * PREPARE_LONG / PREPARE / TURN_IN / TURN_NOW。
 *
 * ★ 基准 = 按**默认速度**算（官方就是这么做的：阈值是"车型档位"的属性，
 *   不是当前车速的属性），当前车速只用来在 voice_announce_state() 里
 *   做等比缩放。所以这个函数的参数是"默认速度"而不是"当前速度"。
 * default_speed_ms <= 0 时用 VOICE_DEFAULT_SPEED_MS。
 * ★ 与官方 isDistanceLess 一致：高速时按 cur/DEFAULT 的比例放大。 */
void voice_lead_distances(double default_speed_ms, double out_lead[4]);

/* 给定距下一条指令的距离，算该播第几级（VOICE_*）。
 * speed_ms <= 0 用默认速度。VOICE_NONE = 现在不该播。
 * ★ 与官方 getImminentTurnStatus 同义：命中就返回**最紧急**的一级。 */
int  voice_announce_state(double to_go_m, double speed_ms);

/* 便捷判定：是不是"现在"级别（该说"现在左转"而不是"前方 X 米左转"）。 */
int  voice_is_now(double to_go_m, double speed_ms);

/* ---------------------------------------------------------------------------
 * 2) 话术模板（抄 VoiceRouter.turnPhrase / action / Alarm.phrase）
 *
 * 文案风格照 OsmAnd 桌面版中文：
 *   远："前方 800 米左转，进入建国路"
 *   近："现在左转，进入建国路"
 *   合并："前方 200 米左转，然后紧接着右转"
 *   环岛："前方 300 米进入环岛，从第 3 个出口驶出"
 *   到达："前方 100 米到达目的地"
 *   告警："前方 500 米有测速摄像头"（官方原码会拼成"前方 500 米前方有…"，
 *         本模块去掉重复前缀，见 voice.c 的说明）
 * ------------------------------------------------------------------------- */

/* 转向动作短语（抄 nav-desktop VoiceRouter.action()）。
 * type 取 MAN_*；exit_no > 0 时环岛会带"从第 N 个出口驶出"。
 * ★ 返回值可能是静态缓冲，调用方必须**立即**用掉。 */
const char *voice_action_text(int man_type, int exit_no);

/* 距离口语化（抄 nav-desktop core/Units.spoken() → dist() → metersText()）。
 * |d| < 10 m   ⇒ "5.0 米"
 * |d| < 1000 m ⇒ 整十米（"240 米"）
 * |d| < 100 km ⇒ 一位小数公里（"1.5 公里"）
 * else         ⇒ 整数公里（"120 公里"）
 * 与 maneuver.c 的 fmt_distance 不同：语音用"240 米"而不是"237 米"，
 * 免得 TTS 念出一串拗口数字。 */
void voice_spoken_distance(double meters, char *buf, size_t n);

/* 完整转向句。to_go_m = 距该指令的距离（米），now != 0 ⇒ 强制"现在"句式。
 * road 可为 NULL/空。buf 至少 256 字节。
 * 返回写入的字节数。 */
int  voice_turn_phrase(const Maneuver *m, double to_go_m, int now,
                       char *buf, size_t n);

/* 告警句（抄 nav-desktop model/Alarm.phrase()）。
 * dist = 距告警点距离（米）；alarm 取 VOICE_ALARM_*；
 * tunnel_len_m 仅隧道用（<=0 表示未知）。 */
int  voice_alarm_phrase(int alarm, double dist, double tunnel_len_m,
                        char *buf, size_t n);

/* ---------------------------------------------------------------------------
 * 3) 沿路告警扫描（抄 Alarms.java + Navigator.findAlarm + Alarms.priority）
 *
 * 本项目路网里的告警来源是**边标志位**（EF_TUNNEL / EF_TOLL / EF_BRIDGE），
 * 官方是**点标注**（speed_camera / toll_booth…）。数据结构不同，但
 * "沿里程扫一遍、把连续同类的合成区间型告警、再按优先级挑一个"的骨架一致：
 *   · 官方 Alarms.fromSegments：连续 tunnel 段合并成一条区间告警，
 *     长度 < MIN_TUNNEL_M(60m) 就丢掉（避免地下通道刷屏）。
 *   · 官方 Navigator.findAlarm：遍历告警列表，用 Alarms.priority() 打分，
 *     取最优的一个（分数小者优，同分取近的）。
 *   · 官方 Alarms.priority(a, timeSec, dist)：距离 >1500m 不提示；
 *     timeSec<6 或 dist<75 ⇒ 该类优先级；摄像头类再放宽到 timeSec<15/dist<150；
 *     收费站再放宽到 timeSec<30/dist<500；最后 timeSec<7 或 dist<100 ⇒
 *     优先级 + MAXIMUM 的 10。都不满足 ⇒ 不提示。
 * ------------------------------------------------------------------------- */

/* 官方 Alarms.MIN_TUNNEL_M：隧道太短不值得提示。 */
#define VOICE_ALARM_MIN_TUNNEL_M   60.0
/* 官方 Alarms.priority() 的最远提示距离。 */
#define VOICE_ALARM_MAX_DIST_M     1500.0

/* 一个沿路告警实例。 */
typedef struct {
    int      type;      /* VOICE_ALARM_* */
    int      inst;      /* 实例号：同类型的不同位置算不同实例
                         *（官方用 Alarm 对象的身份做 key，见下） */
    double   dist_m;    /* 距当前位置还有多远（米） */
    double   len_m;     /* 区间型（隧道/收费路段/桥梁）总长；点状为 0 */
} VoiceAlarm;

/* 扫前方沿路告警，返回"当前最值得提示"的那一个。
 * rr      路线（认 edge_flags 里的 EF_TUNNEL/EF_TOLL/EF_BRIDGE）
 * from_m  当前已走里程（米，沿路线）
 * speed_ms 当前速度（m/s；官方 priority() 要用它算 timeSec）
 * 返回 1 = 找到（out 填好）；0 = 没有值得提示的。
 * ★ 官方用 IdentityHashMap<Alarm,Integer> 给**每个告警实例**记已播级别，
 *   所以一条路上有两个隧道时第二个仍会播；本模块用 inst 复刻这一点。 */
int  voice_scan_alarm(const RouteResult *rr, double from_m, double speed_ms,
                      VoiceAlarm *out);

/* ---------------------------------------------------------------------------
 * 4) 播报节奏（抄 nav-desktop VoiceRouter 的状态与冷却）
 *
 * 结构体持有全部"播过什么"的记忆，调用方每帧喂一次状态、它决定要不要播。
 * ------------------------------------------------------------------------- */

/* 冷却时长（毫秒），抄 nav-desktop VoiceRouter 的常量。 */
#define VOICE_COOLDOWN_SAME_TEXT_MS   15000   /* 同文本冷却 */
#define VOICE_COOLDOWN_LIMIT_MS       60000   /* 限速变化播报冷却 */
#define VOICE_COOLDOWN_OVER_MS        45000   /* 超速播报冷却（官方 OVER_COOLDOWN） */
#define VOICE_COOLDOWN_ARRIVE_MS      60000   /* 到达播报冷却 */
#define VOICE_COOLDOWN_ALARM_MS        3000   /* 告警与转向播报错开 */

/* 告警实例的已播级别表（官方 IdentityHashMap 的等价物）。
 * 官方按 Alarm 对象身份记，本模块按 (type, inst) 记。
 * ★ inst = 该告警在路线里的**段下标**（位置恒定，与扫描到哪儿无关）。
 *   ⇒ 用 int32_t：长途路线段数可过 32767，int16_t 会溢出成负数，
 *     撞上别的告警的槽位。
 * ★ 槽位数取 32：官方 IdentityHashMap 是无界的（每条路线有多少告警就记多少）。
 *   本表是定长的，满了之后会复用"已播到最高级"的槽 ⇒ 极长路线上
 *   很老的告警可能被当成新告警重播一次。32 个槽足够覆盖绝大多数路线。 */
#define VOICE_ALARM_SLOTS 32
typedef struct {
    int16_t type;      /* VOICE_ALARM_*；-1 = 空槽 */
    int32_t inst;      /* 实例号（= 段下标） */
    uint8_t stage;     /* 0=未播 1=远级 2=近级 */
} VoiceAlarmSlot;

/* 播报器状态。调用方持有它（每条路线一个），随路线重置。 */
typedef struct {
    /* 上一条指令的下标与已播层级：变了下标 ⇒ 层级重置 */
    int      last_man_idx;
    int      last_state;
    /* 上一句文本与时间：同文本在冷却期内不重复播 */
    char     last_text[192];
    uint64_t last_text_ms;
    /* 限速播报：上次播的限速值与时间 */
    int      last_limit_kmh;
    uint64_t last_limit_ms;
    /* 超速播报：上次播的时间 */
    uint64_t last_over_ms;
    /* 到达播报：是否已播 + 时间 */
    int      arrived_said;
    uint64_t last_arrive_ms;
    /* 告警：上次播报时间（与转向播报错开） */
    uint64_t last_alarm_ms;
    /* 告警实例的已播级别表（官方 IdentityHashMap 的等价物） */
    VoiceAlarmSlot alarm_slot[VOICE_ALARM_SLOTS];
    /* ★ "这个冷却计时器有没有起算过"的标志。
     *   不能用"时间戳 == 0 表示没播过"——now_ms 是**调用方注入**的，
     *   自测会从 1000 开始喂，真实时钟也可能很小。官方靠
     *   System.currentTimeMillis()（epoch，永远很大）侥幸没暴露这个问题，
     *   抄过来时必须显式补这个标志。 */
    uint8_t  has_limit_said;
    uint8_t  has_over_said;
    uint8_t  has_arrive_said;
    uint8_t  has_alarm_said;
} VoiceRouter;

/* 初始化（新路线 / 重新规划后调）。 */
void voice_router_reset(VoiceRouter *vr);

/* 本帧是否该播转向。*out_text 写入话术（至少 256 字节），返回 1=该播。
 *
 * man_idx     当前指令下标（变了就重置层级）
 * ml          指令列表（官方 st.current / st.after 都从这里取）
 *             ★ 传 ml 而不是裸 Maneuver*：官方的"路口合并"判据用的是
 *               st.after.distToNext（**下一条**指令到再下一条的距离），
 *               光给一条指令算不出来。ml 可为 NULL ⇒ 不合并。
 * to_go_m     距该指令的距离（米）
 * speed_ms    当前速度（m/s）
 * out_text    输出话术
 * now_ms      当前时刻（毫秒，单调时钟）。**由调用方传入**而不是内部取
 *             —— 这样冷却逻辑能被自测确定性地驱动（自测可以随便喂
 *             "1 秒后""16 秒后"来验证冷却到期，不受真实时间影响）。
 */
int  voice_router_maybe_say(VoiceRouter *vr, int man_idx, const ManeuverList *ml,
                            double to_go_m, double speed_ms,
                            char *out_text, size_t n, uint64_t now_ms);

/* 本帧是否该播限速变化。返回 1=该播（话术写入 out_text）。
 * limit_kmh：当前限速（<=0 视为无限速）；cur_kmh：当前车速。
 * ★ 抄官方 nav-desktop VoiceRouter.speedLimit()：
 *   · 首次不播（一上路就说话太吵）
 *   · 限速变化幅度 >= 10 km/h 才播
 *   · 超速（cur_kmh > limit_kmh + 10）时另播一句"您已超速…"，冷却 45s
 *     （官方 OVER_COOLDOWN = 45000）
 * ★ 官方的 kmh 取整是 round(限速m/s * 3.6 / 5) * 5（对齐 5 的倍数），
 *   这个取整由**调用方**做（本函数收的是已取整的 km/h）。 */
int  voice_router_maybe_say_limit(VoiceRouter *vr, int limit_kmh, int cur_kmh,
                                  char *out_text, size_t n, uint64_t now_ms);

/* 本帧是否该播告警。返回 1=该播（话术写入 out_text）。
 * al 取自 voice_scan_alarm()。
 * ★ 与官方一致：每个告警实例最多两级
 *   （进范围报一次"前方 500 米有…"，60 m 内再补一句"有…"）。 */
int  voice_router_maybe_say_alarm(VoiceRouter *vr, const VoiceAlarm *al,
                                  char *out_text, size_t n, uint64_t now_ms);

/* 本帧是否该播"即将到达"。返回 1=该播。dist_remaining = 剩余里程（米）。 */
int  voice_router_maybe_say_arrive(VoiceRouter *vr, double dist_remaining,
                                   char *out_text, size_t n, uint64_t now_ms);

/* ---------------------------------------------------------------------------
 * 5) 每帧编排（抄 nav-desktop VoiceRouter.update()）
 *
 * ★ 为什么把编排也搬进 voice.c：
 *   官方"该播什么"是**一个**方法里的顺序决策（speedLimit → arrival → alarm
 *   → turn），而不是四个各自独立的 if。顺序会影响**同文本冷却**的结果：
 *   官方的 say() 是所有播报的**唯一出口**，同文本冷却对限速/到达/告警/转向
 *   一视同仁；把去抖只做在转向那条路上，等于抄了个"四分之一"的官方行为。
 *   ★ 本模块之前就是四分之一：say_dup() 只在 voice_router_maybe_say() 里调用，
 *     限速/到达/告警都不走冷却 —— 与官方不符，已修（见 voice.c）。
 *
 * ★ 官方 update() 的顺序（VoiceRouter.java:73-77）：
 *       speedLimit(st, speedMps, now);
 *       arrival(st, now);
 *       alarm(st, now);
 *       …转向…
 *   顺序照抄。转向放最后是官方的选择：它是"最重要的一句"，放最后播，
 *   不会被前面的限速/告警盖掉。
 * ------------------------------------------------------------------------- */

/* 一帧最多播几句（官方一帧最多 4 类：限速 / 到达 / 告警 / 转向）。 */
#define VOICE_TICK_MAX 4

/* 一帧的播报决策结果。 */
typedef struct {
    int  n;                              /* 实际条数 */
    char text[VOICE_TICK_MAX][192];      /* 逐条文本 */
} VoiceFrame;

/* 一帧的播报决策（抄官方 VoiceRouter.update()）。
 *
 * vr     播报器状态（会被更新：层级、冷却、告警已播级别）
 * rr     路线（沿路告警要扫 edge_flags）；可为 NULL
 * ml     转向指令列表；可为 NULL
 * man_idx 当前指令下标（官方 State.index 同义）；<0 = 无当前指令
 * to_go_m 距该指令的距离（米）
 * speed_ms 当前速度（m/s）
 * remain_m 剩余里程（米）
 * arrived  是否已停在终点
 * now_ms  当前时刻（毫秒，单调时钟）
 * out    输出（写入本帧该播的句子）
 *
 * 返回本帧播出的条数（0 = 不播）。
 * ★ 官方同文本冷却对**所有**类别生效：这里把四类都过同一个 say_dup()。
 * ★ 官方顺序 speedLimit → arrival → alarm → turn 严格照抄。 */
int  voice_tick(VoiceRouter *vr, const RouteResult *rr, const ManeuverList *ml,
                int man_idx, double to_go_m, double speed_ms, double remain_m,
                int arrived, uint64_t now_ms, VoiceFrame *out);

#endif /* NAVIMAP_VOICE_H */
