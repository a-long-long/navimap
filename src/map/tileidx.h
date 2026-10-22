/* tileidx.h —— 经纬度 → Z14 瓦片下标
 * -------------------------------------------------------------------------
 * 路由缝合、瓦片查询、覆盖率统计都要把 (lat, lon) 变成瓦片下标。它是搜索
 * 热路径上每次展开都要算一次的函数（京→郑 85 万次），所以单列一个模块：
 * 生产代码用快的那版，自检工具用同一份代码跟精确版逐点比。
 *
 * 三个版本：
 *   nm_tile_of_exact()  参考实现：sin + log，精确但慢（55.5 ns/次）
 *   nm_tile_of_raw()    查表，不做边界回退（11.2 ns/次）；**只给自检用**，
 *                       它存在的意义是证明"边界回退确有必要"
 *   nm_tile_of()        生产用：查表 + 边界带回退
 *
 * 为什么查表还需要回退：nm_mercator_y_fast() 是插值表，有误差。平时被取整
 * 吃掉，但点落在瓦片边界附近时可能取到隔壁瓦片 —— 瓦片取错 ⇒ 缝合成员查错
 * ⇒ 路线错。所以在边界带里回退到精确公式定夺。
 * ★ 边界带宽（2e-3 瓦片）必须大于查表的最坏误差（极区 3.6e-4 瓦片），
 *   否则误差把点推出判定带，回退压根不触发——这个坑踩过一次，见 tileidx.c。
 *
 * ⚠ 判据不是"抽样没差异"（§31 的教训：lround 会掩盖 ULP 差异，抽样跑不出
 *   来）。tools/tile_selftest 对全图 5228 万个真实节点**逐点**比两套实现，
 *   外加定向构造"正好落在瓦片边界上"的点。
 * ------------------------------------------------------------------------- */
#ifndef NAVIMAP_TILEIDX_H
#define NAVIMAP_TILEIDX_H

void nm_tile_of(double lat, double lon, int *tx, int *ty);
void nm_tile_of_raw(double lat, double lon, int *tx, int *ty);
void nm_tile_of_exact(double lat, double lon, int *tx, int *ty);

#endif /* NAVIMAP_TILEIDX_H */
