/* voice_selftest —— 语音播报策略自测（抄写自 OsmAnd 的那套策略）。
 *
 * 覆盖三块（对应 src/route/voice.c 的三个层次）：
 *   1) 分级提前量：速度 × 秒数模型（官方 AnnounceTimeDistances.java）
 *   2) 话术模板：转向句 / 告警句（官方 nav-desktop VoiceRouter / Alarm）
 *   3) 播报节奏：层级去重、路口合并、同文本/限速/告警冷却
 *
 * 时间**全部由测试注入**（now_ms 参数），不受真实时钟影响 ⇒ 判据确定。
 * 全过打印 VOICE-OK。
 *
 * ★ 期望值全部来自官方源码（数值逐项核对过）：
 *   · 提前量：官方 AnnounceTimeDistances.java 的 (int) 截断值
 *   · 话术：官方 nav-desktop VoiceRouter.turnPhrase/action、Alarm.phrase
 *   · 距离：官方 nav-desktop core/Units.spoken
 */
#include "route/voice.h"

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

/* 造一条转向指令 */
static Maneuver mk_man(int type, const char *road, int exit_no)
{
    Maneuver m;
    memset(&m, 0, sizeof m);
    m.type = type;
    m.exit_no = exit_no;
    if (road) {
        size_t l = strlen(road);
        if (l >= sizeof m.road) l = sizeof m.road - 1;
        memcpy(m.road, road, l);
        m.road[l] = '\0';
    }
    return m;
}

/* 造一条指令列表：dist_m[i] = 距**上一条**指令的路程（官方 distFromStart 链同构）。
 * 返回条数。 */
static int mk_list(ManeuverList *ml, const Maneuver *v, int n, const double *gap)
{
    memset(ml, 0, sizeof *ml);
    ml->v = (Maneuver *)malloc((size_t)(n > 0 ? n : 1) * sizeof(Maneuver));
    for (int i = 0; i < n; i++) {
        ml->v[i] = v[i];
        ml->v[i].dist_m = (i == 0) ? 0.0 : gap[i];
    }
    ml->n = n;
    ml->cap = n;
    double sum = 0;
    for (int i = 0; i < n; i++) sum += ml->v[i].dist_m;
    ml->sum_m = sum;
    return n;
}

int main(void)
{
    char buf[256];

    /* ================= 1) 分级提前量：速度 × 秒数 =================
     *
     * ★ 关键判据：**高速的提前量必须大于低速**。这是抄 OsmAnd 的核心意义 ——
     *   原来的固定 300m/60m 在 120 km/h 时只剩 9 秒，来不及提醒。 */

    double lead_slow[4], lead_fast[4];
    voice_lead_distances(8.0, lead_slow);    /* 基准档：29 km/h */
    voice_lead_distances(33.0, lead_fast);   /* 基准档：119 km/h */

    CHECK(lead_fast[VOICE_PREPARE] > lead_slow[VOICE_PREPARE],
          "高速档的 PREPARE 提前量应大于低速档");
    /* 官方公式：PREPARE = speed * 115s */
    CHECK((int)(lead_slow[VOICE_PREPARE] / 115.0) == 8,
          "PREPARE = 速度×115 秒（官方公式）");
    CHECK((int)(lead_fast[VOICE_PREPARE] / 115.0) == 33,
          "PREPARE = 速度×115 秒（高速）");
    /* TURN_IN = speed * 22s */
    CHECK((int)(lead_slow[VOICE_TURN_IN] / 22.0) == 8,
          "TURN_IN = 速度×22 秒（官方公式）");
    /* TURN_NOW 有 12 m 定位误差兜底（官方 POSITIONING_TOLERANCE） */
    CHECK(lead_slow[VOICE_TURN_NOW] >= VOICE_POS_TOLERANCE_M,
          "TURN_NOW 不低于 12 米定位误差兜底");

    /* ★ 与官方 **源码**（不是注释）逐项对齐。官方是 (int) 截断：
     *   PREPARE_DISTANCE = (int)(12.5*115)   = 1437（官方注释写 1438）
     *   TURN_IN_DISTANCE = (int)(12.5*22)    = 275
     *   TURN_NOW_DISTANCE= (int)(12.5*6.708) = 83
     * 自行车/步行档同样对得上（码 318/127，注 319/128）⇒ 确认是"码截断、
     * 注四舍五入"，以码为准。
     *
     * ★★ 判据必须用**浮点精确相等**，不能写 (int)(x) == 1437：
     *   那样写的话 1437.5 也会通过（(int)1437.5 == 1437），
     *   ⇒ 把官方的截断改回浮点这个 bug 就抓不到（实测过）。
     *   官方值里 PREPARE=1437、PREPARE_LONG=3750 是整数，
     *   浮点版本会得到 1437.5 / 3750.0 ⇒ 精确相等能区分。 */
    {
        double car[4];
        voice_lead_distances(VOICE_DEFAULT_SPEED_MS, car);
        CHECK(car[VOICE_PREPARE] == 1437.0,
              "官方 car 档 PREPARE = 1437 m（(int)(12.5*115)，不是 1437.5）");
        CHECK(car[VOICE_TURN_IN] == 275.0,
              "官方 car 档 TURN_IN = 275 m");
        CHECK(car[VOICE_TURN_NOW] == 83.0,
              "官方 car 档 TURN_NOW = 83 m");
        CHECK(car[VOICE_PREPARE_LONG] == 3750.0,
              "官方 car 档 PREPARE_LONG = 3750 m");
        /* ★ 边界判据：1437.0 命中提前级，1437.5 不命中。
         *   浮点版（1437.5）会让 1437.5 也命中 ⇒ 这条能抓住那个 bug。 */
        CHECK(voice_announce_state(1437.0, 12.5) == VOICE_PREPARE,
              "1437 m 命中提前级（官方截断值）");
        CHECK(voice_announce_state(1437.5, 12.5) != VOICE_PREPARE,
              "1437.5 m 不该命中提前级（截断后阈值就是 1437）");
    }

    /* 分级判定：命中就返回**最紧急**的一级 */
    CHECK(voice_announce_state(5.0, 8.0) == VOICE_TURN_NOW, "5m ⇒ 立即级");
    CHECK(voice_announce_state(100.0, 8.0) == VOICE_TURN_IN, "100m ⇒ 临近级");
    CHECK(voice_announce_state(500.0, 8.0) == VOICE_PREPARE, "500m ⇒ 提前级");
    CHECK(voice_announce_state(5000.0, 8.0) == VOICE_NONE, "5000m ⇒ 不播");

    /* ★ 高速时同一距离应更早触发（官方"快路更早提醒"的核心）。
     *   400 m 处：低速还在"提前级"，高速已经进入"临近级"。 */
    CHECK(voice_announce_state(400.0, 8.0) == VOICE_PREPARE,
          "400m 低速 ⇒ 提前级");
    CHECK(voice_announce_state(400.0, 33.0) == VOICE_TURN_IN,
          "400m 高速 ⇒ 临近级（更早提醒）");

    /* ★★★ 回归判据：**高速下"提前预告"那一级必须能播出来**。
     *   这是本模块修掉的一个真 bug：层级判定若"先查 PREPARE_LONG"，
     *   高速时 PREPARE_LONG 的圈（3750×速度比）远大于 PREPARE 的圈
     *   （1437×速度比），2000 m 处会被判成更弱的 PREPARE_LONG，
     *   ⇒ 中间那级永远播不出来。
     *   120 km/h（33.3 m/s）时 PREPARE 的圈 = 1437×33.3/12.5 = 3829 m，
     *   所以 2000 m 必须是 PREPARE 而不是 PREPARE_LONG。 */
    CHECK(voice_announce_state(2000.0, 33.0) == VOICE_PREPARE,
          "2000m 高速 ⇒ 提前级（不能被超远预告级吞掉）");
    CHECK(voice_announce_state(2000.0, 8.0) == VOICE_NONE,
          "2000m 低速 ⇒ 不播（未进提前级）");

    /* 高速专属的 PREPARE_LONG（官方 issue 1411：只在 >=30 m/s 用）。
     *   5000 m @120km/h：PREPARE 圈 3829 m 不够，PREPARE_LONG 圈
     *   = 3750×33/12.5 = 9900 m ⇒ 命中超远预告级。 */
    CHECK(voice_announce_state(5000.0, 33.0) == VOICE_PREPARE_LONG,
          "5000m 高速 ⇒ 超远预告级");
    CHECK(voice_announce_state(5000.0, 8.0) == VOICE_NONE,
          "5000m 低速 ⇒ 不播（低于高速门槛）");

    /* 立即级边界：官方低速时 TURN_NOW 距离**更小**（越慢越晚说"现在"），
     * 高速时更大（提前留出反应时间）。
     * ★ 求的是 NOW 的**上边界**（最后一个命中 NOW 的距离），不是第一个 ——
     *   命中判定是"距离 <= 阈值"，所以从 5 米往上扫，第一个命中的一定是 5。 */
    double now_slow = 0, now_fast = 0;
    for (double d = 5; d < 400; d += 1) {
        if (voice_announce_state(d, 8.0) == VOICE_TURN_NOW)  now_slow = d;
    }
    for (double d = 5; d < 400; d += 1) {
        if (voice_announce_state(d, 33.0) == VOICE_TURN_NOW) now_fast = d;
    }
    CHECK(now_fast > now_slow, "高速的立即级距离应大于低速（官方按速度放大）");
    CHECK(now_slow < 100.0, "低速的立即级距离应较小（官方低速时缩小）");

    /* ================= 2) 话术模板 ================= */

    /* 距离口语化：官方 nav-desktop core/Units.spoken
     *   <10 m → 一位小数米；<1000 m → 整十米；<100 km → 一位小数公里 */
    voice_spoken_distance(237.0, buf, sizeof buf);
    CHECK(strcmp(buf, "240 米") == 0, "237 米 ⇒ \"240 米\"（官方整十米）");
    voice_spoken_distance(850.0, buf, sizeof buf);
    CHECK(strcmp(buf, "850 米") == 0, "850 米 ⇒ \"850 米\"");
    voice_spoken_distance(1500.0, buf, sizeof buf);
    CHECK(strcmp(buf, "1.5 公里") == 0, "1500 米 ⇒ \"1.5 公里\"");
    voice_spoken_distance(5.0, buf, sizeof buf);
    CHECK(strcmp(buf, "5.0 米") == 0, "5 米 ⇒ \"5.0 米\"（官方一位小数）");
    voice_spoken_distance(120000.0, buf, sizeof buf);
    CHECK(strcmp(buf, "120 公里") == 0, "120 公里 ⇒ \"120 公里\"（整数公里）");

    /* 转向句（远）：官方 "前方" + voiceDist + act + "，进入" + road */
    {
        Maneuver m = mk_man(MAN_LEFT, "建国路", 0);
        voice_turn_phrase(&m, 800.0, 0, buf, sizeof buf);
        CHECK(strcmp(buf, "前方800 米左转，进入建国路") == 0,
              "远距离句 = 官方格式");
    }

    /* 转向句（近）：官方 "现在" + act（无距离） */
    {
        Maneuver m = mk_man(MAN_LEFT, "建国路", 0);
        voice_turn_phrase(&m, 30.0, 1, buf, sizeof buf);
        CHECK(strcmp(buf, "现在左转，进入建国路") == 0,
              "近距离句 = 官方 \"现在左转\"");
    }

    /* 无路名时不出现"进入" */
    {
        Maneuver m = mk_man(MAN_RIGHT, "", 0);
        voice_turn_phrase(&m, 300.0, 0, buf, sizeof buf);
        CHECK(strcmp(buf, "前方300 米右转") == 0, "无路名时只有动作");
        CHECK(strstr(buf, "进入") == NULL, "无路名时不含\"进入\"");
    }

    /* 动作词表：官方 action() 的中文逐条 */
    CHECK(strcmp(voice_action_text(MAN_STRAIGHT, 0), "沿当前道路直行") == 0,
          "直行 ⇒ \"沿当前道路直行\"（官方）");
    CHECK(strcmp(voice_action_text(MAN_LEFT, 0), "左转") == 0, "左转");
    CHECK(strcmp(voice_action_text(MAN_SLIGHT_LEFT, 0), "稍向左转") == 0,
          "稍左 ⇒ \"稍向左转\"（官方 TSLL）");
    CHECK(strcmp(voice_action_text(MAN_SHARP_LEFT, 0), "急向左转") == 0,
          "急左 ⇒ \"急向左转\"（官方 TSHL）");
    CHECK(strcmp(voice_action_text(MAN_UTURN, 0), "掉头") == 0, "掉头");

    /* 环岛：官方 action() 自带出口号 */
    {
        Maneuver m = mk_man(MAN_ROUNDABOUT, "", 3);
        voice_turn_phrase(&m, 300.0, 0, buf, sizeof buf);
        CHECK(strcmp(buf, "前方300 米进入环岛，从第 3 个出口驶出") == 0,
              "环岛句 = 官方格式（含出口号）");
    }

    /* 到达：官方目的地分支 "前方" + dist + "到达目的地"（无"现在"变体） */
    {
        Maneuver m = mk_man(MAN_ARRIVE, "", 0);
        voice_turn_phrase(&m, 100.0, 0, buf, sizeof buf);
        CHECK(strcmp(buf, "前方100 米到达目的地") == 0, "到达句 = 官方格式");
    }

    /* 告警句：官方 Alarm.phrase
     *   近处（<=60 m）= AlarmType.say 整句
     *   远处 = "前方 X 米" + 去前缀核心短语 */
    voice_alarm_phrase(VOICE_ALARM_SPEED_CAMERA, 30.0, 0.0, buf, sizeof buf);
    CHECK(strcmp(buf, "前方有测速摄像头") == 0,
          "近处告警 = 官方 AlarmType.say 整句");
    voice_alarm_phrase(VOICE_ALARM_SPEED_CAMERA, 500.0, 0.0, buf, sizeof buf);
    CHECK(strcmp(buf, "前方500 米有测速摄像头") == 0,
          "远处告警 = \"前方 X 米\" + 核心短语");
    voice_alarm_phrase(VOICE_ALARM_TOLL, 30.0, 0.0, buf, sizeof buf);
    CHECK(strcmp(buf, "前方有收费站") == 0, "收费站告警 = 官方整句");
    voice_alarm_phrase(VOICE_ALARM_RAILWAY, 30.0, 0.0, buf, sizeof buf);
    CHECK(strcmp(buf, "前方有铁路道口，注意观察") == 0,
          "铁路道口告警 = 官方整句");

    /* 隧道是区间型：官方额外报全长 */
    voice_alarm_phrase(VOICE_ALARM_TUNNEL, 30.0, 800.0, buf, sizeof buf);
    CHECK(strcmp(buf, "进入隧道，全长 800 米") == 0,
          "近处隧道 = \"进入隧道，全长 N 米\"（官方）");
    voice_alarm_phrase(VOICE_ALARM_TUNNEL, 500.0, 1200.0, buf, sizeof buf);
    CHECK(strcmp(buf, "前方500 米进入隧道，全长 1.2 公里") == 0,
          "远处隧道带全长（官方）");

    /* ================= 3) 播报节奏 ================= */

    /* 3.1 同一指令同一层级只播一次；进入更近层级 ⇒ 再播一次 */
    {
        VoiceRouter vr;
        voice_router_reset(&vr);
        /* 三条指令（换指令的判据需要能索引到 1 / 2）。
         * gap[i] = 第 i 条距第 i-1 条的路程（与 maneuver.c 的 dist_m 同义）。
         * gap 都取 500 ⇒ 不会触发"相邻路口合并"（官方阈值 150m），
         * 这样本组判据只测层级，不被合并文案干扰。 */
        Maneuver mv[3] = { mk_man(MAN_LEFT, "建国路", 0),
                           mk_man(MAN_LEFT, "建国路", 0),
                           mk_man(MAN_LEFT, "建国路", 0) };
        ManeuverList ml;
        double gap[3] = { 0.0, 500.0, 500.0 };
        mk_list(&ml, mv, 3, gap);
        int r1 = voice_router_maybe_say(&vr, 0, &ml, 500.0, 8.0, buf, sizeof buf, 1000);
        CHECK(r1 == 1, "首次进入提前级应播");
        int r2 = voice_router_maybe_say(&vr, 0, &ml, 480.0, 8.0, buf, sizeof buf, 1100);
        CHECK(r2 == 0, "同一层级不重复播");
        /* 进入更近的层级 ⇒ 应再播一次 */
        int r3 = voice_router_maybe_say(&vr, 0, &ml, 100.0, 8.0, buf, sizeof buf, 1200);
        CHECK(r3 == 1, "进入更近层级应再播");
        CHECK(strstr(buf, "前方") != NULL, "更近层级仍是\"前方\"句式");
        /* 换指令 ⇒ 层级重置，新的指令即使同层级也该播 */
        int r4 = voice_router_maybe_say(&vr, 1, &ml, 500.0, 8.0, buf, sizeof buf, 1300);
        CHECK(r4 == 1, "换指令后应重新播");
        free(ml.v);
    }

    /* 3.2 相邻路口合并：官方 "，然后紧接着" + 下一动作
     * ★ 官方判据读 st.after.distToNext，但官方的 distToNext 生产方
     *   （RouteEngine.finishManeuvers）与消费方（VoiceRouter / NavTop）
     *   差一条索引，官方自测 VoiceRouterProbe:119 也是按消费方意图设值的
     *   ⇒ 本模块取"当前这条 → 下一条"的距离（= 官方 distToNext[current]）。 */
    {
        VoiceRouter vr;
        voice_router_reset(&vr);
        Maneuver mv[2] = { mk_man(MAN_LEFT, "建国路", 0), mk_man(MAN_RIGHT, "", 0) };
        ManeuverList ml;
        double gap[2] = { 0.0, 100.0 };   /* 下一条距当前 100 m */
        mk_list(&ml, mv, 2, gap);
        int r = voice_router_maybe_say(&vr, 0, &ml, 200.0, 8.0, buf, sizeof buf, 1000);
        CHECK(r == 1, "应播");
        CHECK(strstr(buf, "然后紧接着右转") != NULL, "相邻路口应合并播报（官方措辞）");
        free(ml.v);
    }
    /* 距离远 ⇒ 不合并 */
    {
        VoiceRouter vr;
        voice_router_reset(&vr);
        Maneuver mv[2] = { mk_man(MAN_LEFT, "建国路", 0), mk_man(MAN_RIGHT, "", 0) };
        ManeuverList ml;
        double gap[2] = { 0.0, 500.0 };   /* 下一条距当前 500 m */
        mk_list(&ml, mv, 2, gap);
        int r = voice_router_maybe_say(&vr, 0, &ml, 200.0, 8.0, buf, sizeof buf, 1000);
        CHECK(r == 1, "应播");
        CHECK(strstr(buf, "然后紧接着") == NULL, "距离远时不合并");
        free(ml.v);
    }
    /* ★ 负向对照：只有一条指令（没有下一条）⇒ 绝不合并。
     *   判据"有下一条就合并"如果写错成"永远合并"，这里会红。 */
    {
        VoiceRouter vr;
        voice_router_reset(&vr);
        Maneuver mv[1] = { mk_man(MAN_LEFT, "建国路", 0) };
        ManeuverList ml;
        double gap[1] = { 0.0 };
        mk_list(&ml, mv, 1, gap);
        voice_router_maybe_say(&vr, 0, &ml, 200.0, 8.0, buf, sizeof buf, 1000);
        CHECK(strstr(buf, "然后紧接着") == NULL, "没有下一条时不应合并");
        free(ml.v);
    }

    /* 3.3 同文本冷却：15 秒内同一句不重复 */
    {
        VoiceRouter vr;
        voice_router_reset(&vr);
        Maneuver mv[3] = { mk_man(MAN_LEFT, "建国路", 0),
                           mk_man(MAN_LEFT, "建国路", 0),
                           mk_man(MAN_LEFT, "建国路", 0) };
        ManeuverList ml;
        double gap[3] = { 0.0, 500.0, 500.0 };
        mk_list(&ml, mv, 3, gap);
        int r1 = voice_router_maybe_say(&vr, 0, &ml, 500.0, 8.0, buf, sizeof buf, 1000);
        CHECK(r1 == 1, "首次应播");
        /* 换一条指令，但话术文本完全相同 ⇒ 冷却期内不播 */
        int r2 = voice_router_maybe_say(&vr, 1, &ml, 500.0, 8.0, buf, sizeof buf, 5000);
        CHECK(r2 == 0, "同文本在冷却期内不重复播");
        /* 冷却过期（>15s）⇒ 又能播 */
        int r3 = voice_router_maybe_say(&vr, 2, &ml, 500.0, 8.0, buf, sizeof buf, 20000);
        CHECK(r3 == 1, "冷却过期后同文本可再播");
        free(ml.v);
    }

    /* 3.4 限速：首次不播、变化 <10 不播、>=10 才播、冷却 60s */
    {
        VoiceRouter vr;
        voice_router_reset(&vr);
        CHECK(voice_router_maybe_say_limit(&vr, 80, 80, buf, sizeof buf, 1000) == 0,
              "限速首次不播");
        CHECK(voice_router_maybe_say_limit(&vr, 85, 85, buf, sizeof buf, 2000) == 0,
              "限速变化 <10 不播");
        CHECK(voice_router_maybe_say_limit(&vr, 60, 60, buf, sizeof buf, 3000) == 1,
              "限速变化 >=10 应播");
        CHECK(strstr(buf, "限速") != NULL, "限速句含\"限速\"");
        /* 冷却期内（60s）不再播 */
        CHECK(voice_router_maybe_say_limit(&vr, 40, 40, buf, sizeof buf, 4000) == 0,
              "限速冷却期内不播");
        CHECK(voice_router_maybe_say_limit(&vr, 40, 40, buf, sizeof buf, 70000) == 1,
              "限速冷却过期后可播");
    }

    /* 3.5 告警：两级（远/近），同一告警实例不重复
     * ★ 官方用 IdentityHashMap<Alarm,Integer>：按**告警实例**记已播级别，
     *   所以一条路上有两个隧道时第二个仍会播（这是本模块修掉的一个真 bug：
     *   早期版本按**类型**记 ⇒ 第二个隧道永远不播）。 */
    {
        VoiceRouter vr;
        voice_router_reset(&vr);
        VoiceAlarm a;
        memset(&a, 0, sizeof a);
        a.type = VOICE_ALARM_TUNNEL;
        a.inst = 0;
        a.dist_m = 500.0;
        a.len_m = 1000.0;
        CHECK(voice_router_maybe_say_alarm(&vr, &a, buf, sizeof buf, 1000) == 1,
              "告警远级应播");
        CHECK(strstr(buf, "进入隧道") != NULL, "隧道告警文案");
        a.dist_m = 400.0;
        CHECK(voice_router_maybe_say_alarm(&vr, &a, buf, sizeof buf, 5000) == 0,
              "同一告警实例同级不重复播");
        /* 冷却过后进入近级（<60m）应再播一次 */
        a.dist_m = 50.0;
        CHECK(voice_router_maybe_say_alarm(&vr, &a, buf, sizeof buf, 10000) == 1,
              "告警近级应播");
        a.dist_m = 20.0;
        CHECK(voice_router_maybe_say_alarm(&vr, &a, buf, sizeof buf, 20000) == 0,
              "近级播过不再重复");
        /* ★ 另一个实例（inst 不同）⇒ 应当能再播（官方 IdentityHashMap 语义） */
        a.inst = 1;
        a.dist_m = 500.0;
        CHECK(voice_router_maybe_say_alarm(&vr, &a, buf, sizeof buf, 30000) == 1,
              "另一个告警实例应独立播报");
    }

    /* 3.5b 告警冷却：官方 lastAlarmSay 的 3000 ms（与转向播报错开、避免抢话）。
     * ★ 这条必须能失败：去掉冷却后，转向刚播完就会紧接着播告警。
     * ★ 两条告警的**文案要不同**（隧道长度不同 ⇒ "全长 800 米" vs
     *   "全长 1 公里"），否则会被**同文本冷却**（官方 say() 的 15 秒规则）
     *   拦掉 —— 那就分不清是哪个冷却在起作用了。
     *   官方 say() 是所有播报的**唯一出口** ⇒ 同文本冷却对告警同样生效。 */
    {
        VoiceRouter vr;
        voice_router_reset(&vr);
        VoiceAlarm a;
        memset(&a, 0, sizeof a);
        a.type = VOICE_ALARM_TUNNEL;
        a.inst = 0;
        a.dist_m = 500.0;
        a.len_m = 1000.0;
        CHECK(voice_router_maybe_say_alarm(&vr, &a, buf, sizeof buf, 1000) == 1,
              "首次告警应播");
        /* 1000ms 后另一个实例（文案不同）⇒ 冷却期内不播 */
        a.inst = 1;
        a.len_m = 800.0;
        CHECK(voice_router_maybe_say_alarm(&vr, &a, buf, sizeof buf, 2000) == 0,
              "告警冷却期内不播（官方 3000ms 错开）");
        /* 冷却过期（>=3000ms）⇒ 能播 */
        CHECK(voice_router_maybe_say_alarm(&vr, &a, buf, sizeof buf, 4000) == 1,
              "告警冷却过期后可播");
    }

    /* 3.5c 同文本冷却对**告警**同样生效（官方 say() 是唯一出口）。
     * ★ 官方 say()："t.equals(lastText) && now-lastTextAt < 15000 ⇒ 丢弃"。
     *   这条以前只做在转向那条路上，告警/限速/到达都不过它 ⇒ 抄漏了。
     *
     * ★ 第三次调用要**换一个实例**（inst 不同）：官方每个告警实例最多两级
     *   （远级 + 近级），同一个实例的远级播过就不会再播 ⇒ 拿它测"冷却过期"
     *   会被实例级别挡住，测不到冷却本身。 */
    {
        VoiceRouter vr;
        voice_router_reset(&vr);
        VoiceAlarm a;
        memset(&a, 0, sizeof a);
        a.type = VOICE_ALARM_TUNNEL;
        a.dist_m = 500.0;
        a.len_m = 1000.0;
        /* 第一次：inst=0 播 */
        a.inst = 0;
        CHECK(voice_router_maybe_say_alarm(&vr, &a, buf, sizeof buf, 1000) == 1,
              "3.5c 首次告警应播");
        /* 另一个实例，但**文案逐字相同** ⇒ 15 秒内应被去抖丢掉 */
        a.inst = 1;
        CHECK(voice_router_maybe_say_alarm(&vr, &a, buf, sizeof buf, 5000) == 0,
              "3.5c 告警也受同文本冷却（官方 say() 唯一出口）");
        /* 冷却过期（>15s）后，**第三个**实例 ⇒ 能播 */
        a.inst = 2;
        CHECK(voice_router_maybe_say_alarm(&vr, &a, buf, sizeof buf, 20000) == 1,
              "3.5c 同文本冷却过期后告警可播");
    }

    /* 3.6 到达：只播一次；距离太远不播 */
    {
        VoiceRouter vr;
        voice_router_reset(&vr);
        CHECK(voice_router_maybe_say_arrive(&vr, 50.0, buf, sizeof buf, 1000) == 1,
              "到达应播");
        CHECK(strcmp(buf, "前方即将到达目的地") == 0,
              "到达句 = 官方 arrival() 文案");
        CHECK(voice_router_maybe_say_arrive(&vr, 20.0, buf, sizeof buf, 2000) == 0,
              "到达只播一次");
        /* 距离太远不播 */
        VoiceRouter vr2;
        voice_router_reset(&vr2);
        CHECK(voice_router_maybe_say_arrive(&vr2, 500.0, buf, sizeof buf, 1000) == 0,
              "距离远不播到达");
    }

    /* ================= 4) 沿路告警扫描 =================
     *
     * ★ 抄官方 Alarms.fromSegments + Navigator.findAlarm + Alarms.priority：
     *   · 连续 tunnel 段合并成一条区间告警（官方同款）
     *   · 隧道太短（<60m，官方 MIN_TUNNEL_M）不提示
     *   · 按官方 priority() 打分取最优的一个
     *
     * 路线：5 段 / 1600 m
     *   seg0  0..200   无
     *   seg1 200..500  隧道
     *   seg2 500..900  隧道（与 seg1 合并 ⇒ 隧道区间 200..900，长 700m）
     *   seg3 900..1100 无
     *   seg4 1100..1600 收费站 */
    {
        RouteResult rr;
        memset(&rr, 0, sizeof rr);
        rr.ok = 1;
        rr.n = 6;                       /* 5 段 */
        uint32_t segm[5]  = { 200, 300, 400, 200, 500 };
        uint8_t  segf[5]  = { 0, EF_TUNNEL, EF_TUNNEL, 0, EF_TOLL };
        rr.seg_m      = segm;
        rr.edge_flags = segf;
        rr.seg_ds     = NULL;

        VoiceAlarm al;
        /* 官方 priority() 的第一档（timeSec<6 || dist<75）对**所有类型**生效，
         * 所以 200m 处的隧道在 12.5 m/s 下（timeSec=16）拿不到第一档，
         * 要靠"最后一档"（timeSec<7 || dist<100）—— 200m 也不满足
         * ⇒ 官方口径下这个距离**不该提示**。这是官方原样行为。 */
        CHECK(voice_scan_alarm(&rr, 0.0, 12.5, &al) == 0,
              "200m 外的隧道在官方 priority 下不该提示（timeSec 与 dist 都不够）");

        /* 逼近到 60m 内 ⇒ 第一档生效，应扫到隧道 */
        CHECK(voice_scan_alarm(&rr, 145.0, 12.5, &al) == 1,
              "逼近隧道 55m 应扫到告警");
        CHECK(al.type == VOICE_ALARM_TUNNEL, "55m 处应扫到隧道");
        CHECK(al.dist_m > 50.0 && al.dist_m < 60.0, "隧道距离在 50~60 m");
        CHECK(al.len_m > 650.0 && al.len_m < 750.0,
              "隧道区间长度 = 两段合并（官方 fromSegments 同款）≈700m");

        /* ★ 隧道**里面**不重复提示（官方 findAlarm 的区间型分支） */
        CHECK(voice_scan_alarm(&rr, 400.0, 12.5, &al) == 0,
              "已在隧道内不提示（官方区间型规则）");

        /* ★ 官方 priority：收费站 priority=6 < 隧道 priority=11 ⇒ 收费站更优先。
         *   把收费站放到 100m、隧道放到 800m，看选谁。 */
        {
            RouteResult rr2;
            memset(&rr2, 0, sizeof rr2);
            rr2.ok = 1;
            rr2.n = 4;                   /* 3 段 */
            uint32_t m2[3] = { 100, 800, 700 };
            uint8_t  f2[3] = { EF_TOLL, EF_TUNNEL, 0 };
            rr2.seg_m = m2;
            rr2.edge_flags = f2;
            CHECK(voice_scan_alarm(&rr2, 0.0, 12.5, &al) == 1, "应扫到告警");
            CHECK(al.type == VOICE_ALARM_TOLL,
                  "官方 priority：收费站(6) 优先于 隧道(11)");
            CHECK(al.dist_m < 1.0, "收费站距当前位置 ≈0m（第一档 dist<75）");
        }

        /* ★ 负向对照：太短的隧道（<60m，官方 MIN_TUNNEL_M）不提示。
         *   ★ 样本要点：隧道必须在**前方**（否则车已在区间内，官方规则直接跳过，
         *     过滤阈值根本没被检验到 —— 写样本时踩过这个坑），
         *     且距离要 <75m（官方 priority 第一档），否则压根不进候选。 */
        {
            RouteResult rr3;
            memset(&rr3, 0, sizeof rr3);
            rr3.ok = 1;
            rr3.n = 4;                   /* 3 段 */
            uint32_t m3[3] = { 50, 30, 600 };
            uint8_t  f3[3] = { 0, EF_TUNNEL, 0 };
            rr3.seg_m = m3;
            rr3.edge_flags = f3;
            /* 隧道在 50..80（长 30m < 官方 MIN_TUNNEL_M=60），距当前位置 50m */
            CHECK(voice_scan_alarm(&rr3, 0.0, 12.5, &al) == 0,
                  "30m 的短隧道不提示（官方 MIN_TUNNEL_M=60）");
        }
        /* ★ 同一段路、隧道长度改成 100m（>60）⇒ 应当提示。
         *   这条与上一条构成**成对**判据：只有过滤阈值真正起作用时才会一红一绿。 */
        {
            RouteResult rr4;
            memset(&rr4, 0, sizeof rr4);
            rr4.ok = 1;
            rr4.n = 4;
            uint32_t m4[3] = { 50, 100, 600 };
            uint8_t  f4[3] = { 0, EF_TUNNEL, 0 };
            rr4.seg_m = m4;
            rr4.edge_flags = f4;
            CHECK(voice_scan_alarm(&rr4, 0.0, 12.5, &al) == 1,
                  "100m 的隧道要提示（>官方 MIN_TUNNEL_M=60）");
            CHECK(al.type == VOICE_ALARM_TUNNEL, "是隧道");
        }

        /* ★★★ 回归判据：**实例号 inst 必须与扫描位置无关，且不同告警不撞号**。
         *   官方用 IdentityHashMap<Alarm,Integer>，key 是 Alarm **对象身份**
         *   —— 那批 Alarm 在构建路线时一次性生成，之后每次 findAlarm 遍历的
         *   都是同一批对象 ⇒ 身份恒定、互不相同。
         *   早期版本用"本次扫描见到的第几个候选"当 inst ⇒ 被"已过去 20 m"
         *   过滤掉的候选不占号 ⇒ 同一段告警在两次扫描里拿到不同的 inst
         *   ⇒ 已播级别记到了别的告警头上 ⇒ 该播的被静默吞掉。
         *   ★ 这个 bug 是**端到端实测**（tools/voice_e2e.c 的 B2 用例）抓到的：
         *     单函数自测构造 inst 是手工给的，永远抓不到。
         *
         * 样本：两个收费站 —— A 在 0 m、B 在 1000 m（段 0 与段 2 的起点）。
         *   从 0 m 扫 ⇒ 选中 A；从 950 m 扫 ⇒ A 已过去（跳过）、选中 B。
         *   ★ 旧实现里 A 拿 inst=0、B 也拿 inst=0（因为 A 被跳过后不占号）
         *     ⇒ 两个**不同**的告警撞了同一个号 ⇒ 已播级别互相污染。 */
        {
            RouteResult rr5;
            memset(&rr5, 0, sizeof rr5);
            rr5.ok = 1;
            rr5.n = 4;                    /* 3 段 */
            uint32_t m5[3] = { 100, 900, 500 };   /* 段起点 0 / 100 / 1000 */
            uint8_t  f5[3] = { EF_TOLL, 0, EF_TOLL };
            rr5.seg_m = m5;
            rr5.edge_flags = f5;

            VoiceAlarm aA, aB, aB2, aA2;
            CHECK(voice_scan_alarm(&rr5, 0.0, 12.5, &aA) == 1,
                  "从起点应扫到收费站 A");
            CHECK(voice_scan_alarm(&rr5, 950.0, 12.5, &aB) == 1,
                  "逼近 1000 m 处应扫到收费站 B");
            CHECK(aA.type == VOICE_ALARM_TOLL && aB.type == VOICE_ALARM_TOLL,
                  "两个都应是收费站");
            /* ★ 不同告警 ⇒ 不同 inst（官方对象身份互不相同） */
            CHECK(aA.inst != aB.inst,
                  "不同的告警实例必须有不同的 inst（撞号会互相污染已播级别）");
            /* ★ 同一告警 ⇒ 无论从哪儿扫，inst 都一样 */
            CHECK(voice_scan_alarm(&rr5, 960.0, 12.5, &aB2) == 1,
                  "换个位置仍应扫到收费站 B");
            CHECK(aB2.inst == aB.inst,
                  "同一告警的实例号必须与扫描位置无关（官方对象身份语义）");
            CHECK(voice_scan_alarm(&rr5, 10.0, 12.5, &aA2) == 1,
                  "从 10 m 处仍应扫到收费站 A");
            CHECK(aA2.inst == aA.inst,
                  "同一告警的实例号必须与扫描位置无关（官方对象身份语义）");
        }
    }

    if (g_fail == 0) { printf("VOICE-OK\n"); return 0; }
    fprintf(stderr, "VOICE-SELFTEST FAILED: %d 项\n", g_fail);
    return 1;
}
