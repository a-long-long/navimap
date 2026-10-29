#include "utils/geo.h"

#include <stdlib.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

double nm_haversine_m(double lat1, double lon1, double lat2, double lon2)
{
    double p1 = lat1 * M_PI / 180.0;
    double p2 = lat2 * M_PI / 180.0;
    double dp = p2 - p1;
    double dl = (lon2 - lon1) * M_PI / 180.0;
    double a = sin(dp / 2) * sin(dp / 2) +
               cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);
    double c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
    return NM_EARTH_R_M * c;
}

double nm_fast_dist_m(int32_t lat1_e7, int32_t lon1_e7, int32_t lat2_e7, int32_t lon2_e7)
{
    double dlat = (double)(lat2_e7 - lat1_e7) / 1e7;
    double dlon = (double)(lon2_e7 - lon1_e7) / 1e7;
    double latm = (double)((lat1_e7 + lat2_e7) / 2) / 1e7;
    double kx = cos(latm * M_PI / 180.0);
    double dy = dlat * (M_PI / 180.0) * NM_EARTH_R_M;
    double dx = dlon * (M_PI / 180.0) * NM_EARTH_R_M * kx;
    return sqrt(dx * dx + dy * dy);
}

void nm_mercator(double lat, double lon, double *wx, double *wy)
{
    double x = (lon + 180.0) / 360.0;
    double s = sin(lat * M_PI / 180.0);
    double y = 0.5 - log((1.0 + s) / (1.0 - s)) / (4.0 * M_PI);
    *wx = x;
    *wy = y;
}

void nm_mercator_inv(double wx, double wy, double *lat, double *lon)
{
    *lon = wx * 360.0 - 180.0;
    double n = M_PI * (1.0 - 2.0 * wy);
    *lat = 180.0 / M_PI * atan(sinh(n));
}

/* ------------------------------------------------------------------ */
/* 纬度 → 墨卡托 y 的查表版                                            */
/*                                                                     */
/* 为什么值得：一帧几十万到上百万次投影，每次都要跑一遍 sin + log + 两次
 * 除法；实测 z9 北京一帧 273770 条边里，投影独占 30 ms（路网总耗 101 ms
 * 的 30%），比"换更聪明的遍历顺序"更值得动。
 *
 * 做法：y 只依赖纬度，而且是光滑曲线，所以离线（首次用时）把它采样成表，
 * 运行时线性插值。
 *
 * 误差账：y''(度) = (π/180)²·sinφ/(2π·cos²φ)，45° 处约 6.9e-5 世界单位/度²；
 * 线性插值的世界误差 = h²/8·|y''|。取 N=2^15 个格、覆盖 ±85.0511°，
 * h=0.0052° → 世界误差 2.3e-10。换算成像素要乘 256·2^zoom：z18 下 0.016 px，
 * z20 下 0.06 px，远小于取整误差 0.5 px。（换 dolphin/value：z<18 <0.02 px。）
 *
 * ⚠ 上面这个 2.3e-10 **只在"中纬度"成立**：y'' 按 1/cos²φ 发散，误差随纬度
 *   急剧变大 —— 60° 约 5.7e-10（z18 0.04 px）、80° 约 5.3e-9（z18 0.36 px）、
 *   85° 约 2.2e-8（z18 1.5 px）。本项目数据落在 18°~54°N，所以渲染不受影响；
 *   但**拿它算离散下标**（Z14 瓦片）时，极区误差会大到跨格（85° 处 3.6e-4
 *   瓦片），所以 map/tileidx.c 在瓦片边界带里必须回退到精确公式。
 *
 * ★ 只给 mv_project 用。nm_mercator 保留精确版给航路MEA/逆算等对
 *   精度敏感又不热的路径，两边不会被悄悄对齐。
 * ------------------------------------------------------------------ */

#define MY_BITS   15
#define MY_N      (1u << MY_BITS)          /* 32768 格 → 256 KB */
#define MY_RANGE  85.0511287798066         /* 标准瓦片方案的纬度上限 */

static double *g_my;                       /* MY_N + 1 个采样点 */

static void my_table_build(void)
{
    if (g_my) return;
    double *t = malloc((MY_N + 1) * sizeof(double));
    if (!t) return;                        /* 分配失败就一直走精确版 */
    for (uint32_t i = 0; i <= MY_N; i++) {
        double lat = -MY_RANGE + (2.0 * MY_RANGE) * (double)i / (double)MY_N;
        double s = sin(lat * M_PI / 180.0);
        t[i] = 0.5 - log((1.0 + s) / (1.0 - s)) / (4.0 * M_PI);
    }
    g_my = t;
}

double nm_mercator_y_fast(double lat)
{
    double *t = g_my;
    if (!t) { my_table_build(); t = g_my; }
    if (!t) {                              /* 没表：退回精确公式 */
        double s = sin(lat * M_PI / 180.0);
        return 0.5 - log((1.0 + s) / (1.0 - s)) / (4.0 * M_PI);
    }
    if (lat < -MY_RANGE) lat = -MY_RANGE;
    else if (lat > MY_RANGE) lat = MY_RANGE;

    double u = (lat + MY_RANGE) * ((double)MY_N / (2.0 * MY_RANGE));
    uint32_t i = (uint32_t)u;
    if (i >= MY_N) i = MY_N - 1;
    double f = u - (double)i;
    return t[i] + (t[i + 1] - t[i]) * f;
}

/* ------------------------------------------------------------------ */
/* 日出日落（太阳高度角）                                              */
/*                                                                     */
/* 标准周期项公式（赤纬 + 时差），算本地太阳时下的太阳天顶距；天顶距   */
/* > 90.833°（含大气折射修正）即判为夜间。纬度 lat、经度 lon 单位度，   */
/* 东经/北纬为正。utc 为 time_t 秒。                                   */
/* 极区：cos_zen 被夹到 [-1,1]，天顶距恒 >90 即恒夜、恒 <90 即恒昼，   */
/* 公式自然成立，不会除零或越界。                                     */
/* ------------------------------------------------------------------ */

int nm_sun_is_night(double lat, double lon, time_t utc)
{
    struct tm t;
#if defined(_WIN32)
    gmtime_s(&t, &utc);
#else
    gmtime_r(&utc, &t);
#endif
    int year  = t.tm_year + 1900;
    int month = t.tm_mon + 1;
    int day   = t.tm_mday;
    double utc_hours = t.tm_hour + t.tm_min / 60.0 + t.tm_sec / 3600.0;

    /* 一年中的第几天（1..366），闰年 2 月后 +1 */
    static const int cum[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    int doy = cum[month - 1] + day;
    if (month > 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) doy++;

    double rad = M_PI / 180.0;
    double gamma = 2.0 * M_PI / 365.0 * (doy - 1.0 + (utc_hours - 12.0) / 24.0);

    /* 时差（分钟） */
    double eqtime = 229.18 * (0.000075
                   + 0.001868 * cos(gamma) - 0.032077 * sin(gamma)
                   - 0.014615 * cos(2 * gamma) - 0.040849 * sin(2 * gamma));
    /* 赤纬（弧度） */
    double decl = 0.006918 - 0.399912 * cos(gamma) + 0.070257 * sin(gamma)
                  - 0.006758 * cos(2 * gamma) + 0.000907 * sin(2 * gamma)
                  - 0.002697 * cos(3 * gamma) + 0.001480 * sin(3 * gamma);
    /* 真太阳时（分钟）= UTC 分钟 + 时差 + 经度修正（每度 4 分钟，东经为正） */
    double tst = utc_hours * 60.0 + eqtime + 4.0 * lon;
    /* 时角（度）：正午为 0，每小时 15° */
    double ha = tst / 4.0 - 180.0;

    double cos_zen = sin(lat * rad) * sin(decl)
                   + cos(lat * rad) * cos(decl) * cos(ha * rad);
    if (cos_zen >  1.0) cos_zen =  1.0;
    if (cos_zen < -1.0) cos_zen = -1.0;
    double zen = acos(cos_zen) / rad;
    return zen > 90.833 ? 1 : 0;
}
