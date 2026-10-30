/* tutorial/01_coords/main.c — 第 1 课 · 坐标系
 *
 * 本课在"最小框架"上【累加】第一层：坐标系。
 *   - 经纬度量化成 1e-7 度的 int32（项目内部坐标就是这么存的）
 *   - haversine 真实测距（Web Mercator 只能显示、不能测距）
 *   - 把经纬度画到像素缓冲（简单的"视框线性映射"，是视口投影的雏形）
 *
 * 编译： gcc -std=c11 -O2 -o tut main.c && ./tut
 */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include "../common.h"

#define W 480
#define H 360
static uint32_t buf[W * H];

/* —— 内部坐标：1e-7 度 int32（与项目 nm_lat_to_i32 同约定） —— */
static int32_t lat_to_i32(double lat) { return (int32_t)(lat * 1e7); }
static double i32_to_lat(int32_t v)   { return v / 1e7; }

/* —— haversine 真实球面测距（米） —— */
static double hav_m(double la1, double lo1, double la2, double lo2) {
    double R = 6371008.8;
    double p1 = la1 * M_PI / 180, p2 = la2 * M_PI / 180;
    double dp = (la2 - la1) * M_PI / 180;
    double dl = (lo2 - lo1) * M_PI / 180;
    double a = sin(dp / 2) * sin(dp / 2) +
               cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);
    return 2 * R * asin(sqrt(a));
}

/* —— 把经纬度投到画布：视框 [lon0,lon1]×[lat0,lat1] 线性映射到 [0,W]×[0,H] ——
 * 这是 mv_project 的"最小版"：先理解线性映射，第 6 课才引入墨卡托与投影缓存。 */
static double LON0 = 73.0, LON1 = 135.0;   /* 视框经度范围（中国大致范围） */
static double LAT0 = 18.0, LAT1 = 54.0;    /* 视框纬度范围 */
static int proj_x(double lon) { return (int)((lon - LON0) / (LON1 - LON0) * (W - 1)); }
static int proj_y(double lat) { return (int)((LAT1 - lat) / (LAT1 - LAT0) * (H - 1)); }

int main(void) {
    tut_cls(buf, W, H, 0xFFF4F1EA);

    /* 经纬网：每 10° 一条，演示坐标系是怎么"画"出来的 */
    for (int lon = (int)LON0; lon <= (int)LON1; lon += 10) {
        int x = proj_x(lon);
        tut_line(buf, W, H, x, 0, x, H - 1, 0xFFD8D2C8);
    }
    for (int lat = (int)LAT0; lat <= (int)LAT1; lat += 10) {
        int y = proj_y(lat);
        tut_line(buf, W, H, 0, y, W - 1, y, 0xFFD8D2C8);
    }

    /* 三个城市，量化后画点；再连北京—上海一线 */
    struct { const char *name; double lat, lon; } cities[] = {
        {"北京", 39.9042, 116.4074},
        {"上海", 31.2304, 121.4737},
        {"广州", 23.1291, 113.2644},
    };
    for (int i = 0; i < 3; i++) {
        int x = proj_x(cities[i].lon), y = proj_y(cities[i].lat);
        tut_fill_rect(buf, W, H, x - 3, y - 3, 7, 7, 0xFF1F6FB0);
    }
    tut_line(buf, W, H,
            proj_x(cities[0].lon), proj_y(cities[0].lat),
            proj_x(cities[1].lon), proj_y(cities[1].lat), 0xFFB03A1F);

    /* —— 量化往返误差 & 测距，打印出来看 —— */
    int32_t bj_lat = lat_to_i32(cities[0].lat);
    printf("量化往返误差(度): %.10f\n", i32_to_lat(bj_lat) - cities[0].lat);
    printf("北京→上海 haversine = %.1f km\n",
           hav_m(cities[0].lat, cities[0].lon, cities[1].lat, cities[1].lon) / 1000.0);
    printf("北京→广州 haversine = %.1f km\n",
           hav_m(cities[0].lat, cities[0].lon, cities[2].lat, cities[2].lon) / 1000.0);

    if (tut_write_bmp("out.bmp", buf, W, H) != 0) { fprintf(stderr, "write failed\n"); return 1; }
    printf("OK: wrote out.bmp (%dx%d) — 看经纬网 + 三城 + 京沪连线\n", W, H);
    return 0;
}
