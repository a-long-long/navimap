#include "map/tileidx.h"
#include "map/binfmt.h"
#include "utils/geo.h"

#include <math.h>

/* 边界带宽（单位：瓦片）。
   ★ 它必须**大于查表的最坏误差**，否则回退根本不触发（第一版取 1e-4，
     tools/tile_selftest 的定向构造立刻抓到 6536 处差异）。
     误差账：nm_mercator_y_fast 的插值误差 = h²/8·|y''|，h = 0.0052°，
     y''(度²) = (π/180)²·sinφ/(2π·cos²φ)。
       φ=45°  → 3.8e-6 瓦片        φ=53°（中国最北）→ 6e-6 瓦片
       φ=85°  → 3.6e-4 瓦片（cos²φ→0，误差被放大近 100 倍）
   取 2e-3：对最坏情况有 5.5 倍余量。代价是 ~0.4% 的点要走一次精确公式
   （京→郑 85 万次里约 3400 次 × 55 ns ≈ 0.2 ms，可忽略）。 */
#define TILE_EDGE_EPS 2e-3

/* 参考实现：就是 nm_mercator 的精确公式，跟本项目其他所有瓦片计算
   （mapset.c / coverage.c / graph_build.c）逐位一致。别改它，它是尺子。 */
void nm_tile_of_exact(double lat, double lon, int *tx, int *ty)
{
    double wx, wy;
    nm_mercator(lat, lon, &wx, &wy);
    double Z = (double)(1u << NM_TILE_Z);
    int x = (int)(wx * Z), y = (int)(wy * Z);
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= (int)Z) x = (int)Z - 1;
    if (y >= (int)Z) y = (int)Z - 1;
    *tx = x; *ty = y;
}

/* 查表版（不带边界回退）。生产代码不要用 —— 只给自检当"对照组"。 */
void nm_tile_of_raw(double lat, double lon, int *tx, int *ty)
{
    double Z  = (double)(1u << NM_TILE_Z);
    double wx = (lon + 180.0) / 360.0;
    double wy = nm_mercator_y_fast(lat);
    int x = (int)floor(wx * Z), y = (int)floor(wy * Z);
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= (int)Z) x = (int)Z - 1;
    if (y >= (int)Z) y = (int)Z - 1;
    *tx = x; *ty = y;
}

void nm_tile_of(double lat, double lon, int *tx, int *ty)
{
    double Z  = (double)(1u << NM_TILE_Z);
    double wx = (lon + 180.0) / 360.0;
    double wy = nm_mercator_y_fast(lat);
    double fx = wx * Z, fy = wy * Z;
    double ix = floor(fx), iy = floor(fy);
    if (fx - ix < TILE_EDGE_EPS || fx - ix > 1.0 - TILE_EDGE_EPS ||
        fy - iy < TILE_EDGE_EPS || fy - iy > 1.0 - TILE_EDGE_EPS) {
        /* 贴着瓦片边界：查表的 3.8e-6 瓦片误差可能跨格，用精确公式定夺 */
        nm_mercator(lat, lon, &wx, &wy);
        fx = wx * Z; fy = wy * Z;
        ix = floor(fx); iy = floor(fy);
    }
    int x = (int)ix, y = (int)iy;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= (int)Z) x = (int)Z - 1;
    if (y >= (int)Z) y = (int)Z - 1;
    *tx = x; *ty = y;
}
