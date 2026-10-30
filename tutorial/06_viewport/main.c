/* tutorial/06_viewport/main.c — 第 6 课 · 视口、投影与瓦片键
 *
 * 累加内容：打通"地理坐标 ↔ 屏幕像素"。
 *   - MapView：中心经纬度 + zoom + 画布尺寸；投影用【缓存 + 惰性失效】——只有参数真变了才重算。
 *   - mv_project / mv_unproj：经纬度 ↔ 像素（互为逆）。
 *   - nm_tile_of：标准 slippy 瓦片键 (z,x,y)，用于"按需加载哪几块瓦片"。
 *
 * 编译： gcc -std=c11 -O2 -o tut main.c && ./tut
 */
#include "../common.h"
#include <math.h>

#define W 360
#define H 300
static uint32_t buf[W * H];

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
    double lat, lon; int zoom, w, h;
    double scale;            /* 像素/度（赤道近似） */
    double c_lat, c_lon; int c_zoom;   /* 缓存失效键 */
} MapView;

/* 惰性失效：仅当中心/zoom 真变了才重算 scale */
static void mv_begin(MapView *mv) {
    if (mv->c_lat == mv->lat && mv->c_lon == mv->lon && mv->c_zoom == mv->zoom) return;
    mv->scale = 256.0 * (1 << mv->zoom) / 360.0;   /* 整幅 360° 刚好 256*2^z 像素宽 */
    mv->c_lat = mv->lat; mv->c_lon = mv->lon; mv->c_zoom = mv->zoom;
}
static int  mv_x(MapView *mv, double lon) { return (int)(mv->w / 2 + (lon - mv->lon) * mv->scale); }
static int  mv_y(MapView *mv, double lat) { return (int)(mv->h / 2 - (lat - mv->lat) * mv->scale); }
static double mv_lon(MapView *mv, int x)  { return mv->lon + (x - mv->w / 2) / mv->scale; }
static double mv_lat(MapView *mv, int y)  { return mv->lat - (y - mv->h / 2) / mv->scale; }

/* 标准 slippy 瓦片键 (z,x,y) */
static void nm_tile_of(double lat, double lon, int z, int *tx, int *ty) {
    int n = 1 << z;
    *tx = (int)((lon + 180) / 360.0 * n);
    double r = lat * M_PI / 180;
    *ty = (int)((1 - log(tan(r) + 1 / cos(r)) / M_PI) / 2 * n);
}

int main(void) {
    tut_cls(buf, W, H, 0xFFF4F1EA);
    MapView mv = {39.9042, 116.4074, 12, W, H, 0, 0, 0, -1};
    mv_begin(&mv);

    /* 几条"路"（经纬度折线），投影后画出来 */
    double road[][2] = {
        {39.9000,116.4000},{39.9010,116.4010},{39.9020,116.4020},{39.9030,116.4035},
    };
    for (int i = 0; i < 3; i++)
        tut_line(buf, W, H, mv_x(&mv, road[i][1]), mv_y(&mv, road[i][0]),
                          mv_x(&mv, road[i+1][1]), mv_y(&mv, road[i+1][0]), 0xFFB03A1F);

    /* 瓦片网格：算出可见瓦片范围，逐块画边界 */
    int tx0, ty0, tx1, ty1;
    nm_tile_of(mv_lat(&mv, 0), mv_lon(&mv, 0), mv.zoom, &tx0, &ty0);
    nm_tile_of(mv_lat(&mv, H), mv_lon(&mv, W), mv.zoom, &tx1, &ty1);
    for (int ty = ty0; ty <= ty1; ty++)
        for (int tx = tx0; tx <= tx1; tx++) {
            /* 瓦片 (tx,ty) 四个角：经纬度边界 → 像素 */
            double lonA = tx * 360.0 / (1 << mv.zoom) - 180;
            double lonB = (tx + 1) * 360.0 / (1 << mv.zoom) - 180;
            double latA = atan(sinh(M_PI * (1 - 2.0 * ty / (1 << mv.zoom)))) * 180 / M_PI;
            double latB = atan(sinh(M_PI * (1 - 2.0 * (ty + 1) / (1 << mv.zoom)))) * 180 / M_PI;
            int x0 = mv_x(&mv, lonA), x1p = mv_x(&mv, lonB);
            int y0 = mv_y(&mv, latA), y1p = mv_y(&mv, latB);
            tut_line(buf, W, H, x0, y0, x1p, y0, 0xFF9AA0A6);
            tut_line(buf, W, H, x0, y0, x0, y1p, 0xFF9AA0A6);
        }

    int cx, cy;
    nm_tile_of(mv.lat, mv.lon, mv.zoom, &cx, &cy);
    printf("中心瓦片 (z=%d): x=%d y=%d\n", mv.zoom, cx, cy);
    printf("投影往返: 屏幕(0,0)→经纬度(%.4f,%.4f)→屏幕(%d,%d)\n",
           mv_lat(&mv, 0), mv_lon(&mv, 0), mv_x(&mv, mv_lon(&mv, 0)), mv_y(&mv, mv_lat(&mv, 0)));

    if (tut_write_bmp("out.bmp", buf, W, H) != 0) { fprintf(stderr, "write failed\n"); return 1; }
    printf("OK: wrote out.bmp — 红线=路，灰格=瓦片边界\n");
    return 0;
}
