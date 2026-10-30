# 第 7 课：视口、投影与瓦片键

> 模块：`src/map/map_view.{c,h}`、`src/map/tileidx.{c,h}`
> 前置：第 2 课 `nm_mercator`/`nm_mercator_y_fast`、墨卡托世界坐标概念。

## 一、教学目的

打通"地理坐标 ↔ 屏幕像素"的转换，理解视口驱动按需加载与瓦片空间索引。学完后学生应能：写出"经纬度 → 屏幕像素"的调用链；解释投影缓存为什么能省时间（用惰性失效键而非手动 `invalidate`）；说明 `mv_proj` 与 `mv_project` 逐位等价却更快的原因；说明为什么瓦片键判定必须用"逐点比对 5228 万真实节点"而非抽样。

## 二、功能构成

| 文件 | 职责 |
|---|---|
| `map_view.c/h` | `MapView`（视口状态 + 投影缓存）；`mv_project`/`mv_unproj`/`mv_fit_bbox`/`mv_proj_begin`/`mv_proj` |
| `tileidx.c/h` | 瓦片键三版本：`nm_tile_of` / `nm_tile_of_raw` / `nm_tile_of_exact` |

**`MapView`**（`map_view.h:14-30`）：`lat/lon/zoom/w/h/rot` 视口状态 + 投影缓存 `cx,cy,scale` 与惰性失效键 `c_valid/c_lat/c_lon/c_zoom/c_rot/c_w/c_h`。

**`MvProj` 快照**（`map_view.h:51-55`）：批量投影时把"每帧常量"提到循环外：`cx,cy,scale,hw,hh,rot`。

**helper 函数签名一览**：

| 签名 | 行号 | 一行作用 |
|---|---|---|
| `void mv_init(MapView *v, double lat, double lon, double zoom, int w, int h)` | `map_view.c:5-8` | 初始化视口 |
| `double mv_scale(const MapView *v)` | `map_view.c:10-13` | 像素/世界单位 = 256·2^zoom |
| `static void mv_prepare(const MapView *v)` | `map_view.c:22-36` | 惰性失效地填投影缓存 |
| `int mv_project(const MapView *v, double lat, double lon, int *x, int *y)` | `map_view.c:38-61` | 单点经纬度→屏幕像素（+越界判定） |
| `void mv_proj_begin(const MapView *v, MvProj *p)` | `map_view.c:63-70` | 取本帧投影快照 |
| `void mv_unproject(const MapView *v, int x, int y, double *lat, double *lon)` | `map_view.c:72-88` | 屏幕像素→经纬度 |
| `void mv_visible_bbox(const MapView *v, double bbox[4])` | `map_view.c:90-96` | 当前可见经纬度范围 |
| `void mv_pan_pixels(MapView *v, int dx, int dy)` | `map_view.c:98-109` | 拖动：屏幕位移→中心移动 |
| `void mv_zoom_at(MapView *v, double dzoom, int px, int py)` | `map_view.c:111-125` | 以屏幕点为锚缩放 |
| `void mv_clamp_zoom(MapView *v, double zmin, double zmax)` | `map_view.c:127-131` | 缩放 clamp |
| `void mv_fit_bbox(MapView *v, double lat0, double lon0, double lat1, double lon1, double margin_px)` | `map_view.c:133-152` | 让矩形刚好装进视口 |
| `static inline void mv_proj(const MvProj *p, double lat, double lon, int *x, int *y)` | `map_view.h:59-75` | 批量投影（内联，无缓存键比对） |
| `static inline void mv_proj_e7(const MvProj *p, int32_t lat_e7, int32_t lon_e7, int *x, int *y)` | `map_view.h:81-85` | e7 整数版投影（逐位等价） |
| `void nm_tile_of(double lat, double lon, int *tx, int *ty)` | `tileidx.c:47-67` | 生产用：查表+边界回退 |
| `void nm_tile_of_raw(double lat, double lon, int *tx, int *ty)` | `tileidx.c:34-45` | 仅自检：查表无回退 |
| `void nm_tile_of_exact(double lat, double lon, int *tx, int *ty)` | `tileidx.c:20-31` | 参考：sin+log 精确版 |

## 三、完整实现代码

### 1. 视口结构 `MapView` 与 `MvProj`、内联投影（`map_view.h` 完整体节选）

```h
typedef struct {
    double lat, lon;     /* 视口中心 */
    double zoom;         /* 类似瓦片 z：scale = 256 * 2^zoom 像素/世界 */
    int    w, h;         /* 屏幕尺寸（像素） */
    double rot;         /* 地图旋转角（度，顺时针为正；0 = 正北朝上） */

    /* 投影缓存（惰性填充，见 map_view.c 的 mv_prepare）。
     * 视口中心的墨卡托坐标与缩放在一帧内是常量，但原先每次 mv_project
     * 都要重算它们：一次 sin + 一次 log + 一次 pow，正好是每次投影开销的
     * 一半。渲染一帧要投影几十万到几百万次，这些全是重复计算。
     * 这里连"缓存键"一起存，逐字段比对做惰性失效，所以调用方直接改
     * lat/lon/zoom/w/h 也不会画错（不用记着去通知缓存）。 */
    int    c_valid;
    double c_lat, c_lon, c_zoom, c_rot;
    int    c_w, c_h;
    double cx, cy, scale;
} MapView;

typedef struct {
    double cx, cy, scale;   /* 视口中心的墨卡托坐标 + 每世界单位的像素数 */
    double hw, hh;          /* 画布中心偏移：w/2.0、h/2.0 */
    double rot;             /* 旋转角（度，顺时针为正） */
} MvProj;

static inline void mv_proj(const MvProj *p, double lat, double lon, int *x, int *y)
{
    double wx = (lon + 180.0) / 360.0;
    double wy = nm_mercator_y_fast(lat);
    double sx = (wx - p->cx) * p->scale;
    double sy = (wy - p->cy) * p->scale;
    /* rot==0 时这一支恒不进，热路径与改造前逐位等价 */
    if (p->rot != 0.0) {
        double a = -p->rot * (M_PI / 180.0);
        double ca = cos(a), sa = sin(a);
        double rx = sx * ca - sy * sa;
        double ry = sx * sa + sy * ca;
        sx = rx; sy = ry;
    }
    *x = (int)lround(sx + p->hw);
    *y = (int)lround(sy + p->hh);
}

/* e7 整数版：热路径手里的经纬度本来就是 int32 e7（落盘格式），省得每个调用点
 * 都写一遍 /1e7。★ 它就是 mv_proj 的包装，逐位等价，没有额外的性能含义
 * （试过让 e7 版"少算一次除法"或"直算查表下标"，都要动浮点结合顺序，
 *   等价性会破，见 PLAN.md §31）。 */
static inline void mv_proj_e7(const MvProj *p, int32_t lat_e7, int32_t lon_e7,
                              int *x, int *y)
{
    mv_proj(p, (double)lat_e7 / 1e7, (double)lon_e7 / 1e7, x, y);
}
```

### 2. 投影缓存与单点投影 `mv_prepare` / `mv_project`（完整体）

`mv_prepare` 用**逐字段比对缓存键**做惰性失效——`window.c` 到处直接改 `view.lat/zoom`，要求"改完必须 `invalidate`"迟早会漏；漏了就是画面错位。`mv_project` 走 `nm_mercator_y_fast` 查表插值而非精确 `nm_mercator`，因为一帧几十万次调用里 `sin+log` 是真花钱的。

```c
static void mv_prepare(const MapView *v)
{
    MapView *m = (MapView *)v;        /* 只写缓存字段，逻辑上仍是 const */
    if (m->c_valid &&
        m->c_lat == v->lat && m->c_lon == v->lon && m->c_zoom == v->zoom &&
        m->c_rot == v->rot &&
        m->c_w == v->w && m->c_h == v->h)
        return;
    nm_mercator(v->lat, v->lon, &m->cx, &m->cy);
    m->scale = 256.0 * pow(2.0, v->zoom);
    m->c_lat = v->lat; m->c_lon = v->lon; m->c_zoom = v->zoom;
    m->c_rot = v->rot;
    m->c_w = v->w; m->c_h = v->h;
    m->c_valid = 1;
}

int mv_project(const MapView *v, double lat, double lon, int *x, int *y)
{
    mv_prepare(v);
    MapView *m = (MapView *)v;
    /* ★ 这里不用 nm_mercator：那个 sin+log 版本在每帧几十万次调用的场合
       是真花钱的（z9 一帧 30 ms）。墨卡托的 x 本来就只跟经度线性相关，
       y 只跟纬度相关且足够光滑 → 走查表插值版，见 geo.c 的误差账。 */
    double wx = (lon + 180.0) / 360.0;
    double wy = nm_mercator_y_fast(lat);
    double sx = (wx - m->cx) * m->scale;
    double sy = (wy - m->cy) * m->scale;
    if (m->rot != 0.0) {
        double a = -m->rot * (M_PI / 180.0);
        double ca = cos(a), sa = sin(a);
        double rx = sx * ca - sy * sa;
        double ry = sx * sa + sy * ca;
        sx = rx; sy = ry;
    }
    double px = sx + v->w / 2.0;
    double py = sy + v->h / 2.0;
    *x = (int)lround(px);
    *y = (int)lround(py);
    return (*x < 0 || *y < 0 || *x >= v->w || *y >= v->h) ? 1 : 0;
}
```

### 3. 批量投影快照 `mv_proj_begin`（完整体）

一个 pass 开头调一次，把"每帧常量"存进 `MvProj`；循环里用 `mv_proj`。`mv_proj` 与 `mv_project` **逐位等价**（115 万样本 0 差异），只是省掉每次的缓存键比对与字段加载——仅对"本 pass 内视口不变"有效（渲染一帧正满足）。

```c
void mv_proj_begin(const MapView *v, MvProj *p)
{
    mv_prepare(v);                 /* 与 mv_project 同一份缓存，值完全一致 */
    MapView *m = (MapView *)v;
    p->cx = m->cx; p->cy = m->cy; p->scale = m->scale;
    p->hw = v->w / 2.0; p->hh = v->h / 2.0;
    p->rot = v->rot;
}
```

### 4. 反投影 `mv_unproject`（完整体）

屏幕像素 → 经纬度，用于鼠标拖拽与点击选点。先把像素坐标逆旋转（若有 `rot`），再逆墨卡托。

```c
void mv_unproject(const MapView *v, int x, int y, double *lat, double *lon)
{
    mv_prepare(v);
    MapView *m = (MapView *)v;
    double sx = x - v->w / 2.0;
    double sy = y - v->h / 2.0;
    if (m->rot != 0.0) {
        double a = m->rot * (M_PI / 180.0);
        double ca = cos(a), sa = sin(a);
        double rx = sx * ca - sy * sa;
        double ry = sx * sa + sy * ca;
        sx = rx; sy = ry;
    }
    double wx = m->cx + sx / m->scale;
    double wy = m->cy + sy / m->scale;
    nm_mercator_inv(wx, wy, lat, lon);
}
```

### 5. 矩形适配视口 `mv_fit_bbox`（完整体）

用墨卡托矩形中点定中心、`log2(s/256)` 定 zoom，高纬不偏（中心必须取墨卡托中点而非经纬度中点）。

```c
void mv_fit_bbox(MapView *v, double lat0, double lon0,
                 double lat1, double lon1, double margin_px)
{
    double x0, y0, x1, y1;
    nm_mercator(lat0, lon0, &x0, &y0);
    nm_mercator(lat1, lon1, &x1, &y1);
    double dx = fabs(x1 - x0), dy = fabs(y1 - y0);
    if (dx < 1e-9) dx = 1e-9;
    if (dy < 1e-9) dy = 1e-9;

    double aw = (double)v->w - 2.0 * margin_px;
    double ah = (double)v->h - 2.0 * margin_px;
    if (aw < 32) aw = 32;
    if (ah < 32) ah = 32;
    double s = fmin(aw / dx, ah / dy);      /* 像素/世界单位 = 256 * 2^zoom */
    v->zoom = log2(s / 256.0);

    /* 中心 = 墨卡托矩形的中点（不是经纬度中点，否则高纬会偏） */
    nm_mercator_inv((x0 + x1) / 2.0, (y0 + y1) / 2.0, &v->lat, &v->lon);
}
```

### 6. 瓦片键三版本 `nm_tile_of` / `nm_tile_of_raw` / `nm_tile_of_exact`（完整体）

Z14（`NM_TILE_Z=14`，约 2.4km/格@北京）。`nm_tile_of_exact` 用精确 `nm_mercator`；`nm_tile_of_raw` 用 `nm_mercator_y_fast` 查表但**不做边界回退**——它存在的意义是"证明边界回退确有必要"。生产版在瓦片边界带（`TILE_EDGE_EPS=2e-3` 瓦片，必须大于查表最坏误差 3.6e-4）回退精确公式定夺。

```c
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
```

## 四、与其他功能之间的联系

- **上游 · 第 2 课**：`mv_project`/`mv_unproject`/`mv_fit_bbox` 依赖 `nm_mercator`/`nm_mercator_y_fast`/`nm_mercator_inv`；`nm_tile_of` 三版本同样以 `nm_mercator` 为"尺子"。
- **下游 · 第 8/9 课渲染管线**：`render/paint.c` 在每 pass 开头 `mv_proj_begin`，循环内用 `mv_proj`（`paint.c:265,462,805,927,1038`）；`ui/window.c` 的 `app_update_coverage`/`paint_frame` 用 `mv_proj_begin`/`mv_proj` 统一驱动。
- **下游 · 第 11 课跨省缝合**：L6 用同样 Z14 切 tile，`nm_tile_of` 在 `coverage.c`/`mapset_detail_prov` 定 tile；`route/sew.c` 经 `mapset_query_tile` 取跨省边，`sew_cb` 用 `coord_key_e7(node lat/lon)`（见 L6 `coordkey.h`）把不同省的同一坐标合成一个图节点，实现 `sew` 缝合。
- **自检 · 第 14 课架构门禁**：`tile_selftest` 对全图 5228 万真实节点逐点比 `nm_tile_of` vs 精确版，并以 `--expect-diff` 证明 `nm_tile_of_raw` 必须有差异（否则回退是死代码）。

## 五、演示、练习与验收

- **演示**：`./build_rel/tile_selftest`（查表版 vs 精确版 逐点等价）；`./build_rel/tile_xcheck`（跨省接边验证）。
- **练习**：给定两个经纬度，手算 Z14 瓦片坐标，再用 `nm_tile_of` 验证（`TILE_EDGE_EPS` 边界带内会触发精确回退）；观察 `tile_selftest` 对 5228 万节点的逐点比对输出。
- **投影链路验证**：在 `mv_project`（`map_view.c:38`）与 `mv_proj`（`map_view.h:59`）分别打桩，对同一组经纬度比较输出，确认二者逐位一致；再在 `mv_proj_begin` 后修改 `view.zoom` 再调 `mv_proj` 应看出差异（证明快照只对本帧有效）。
- **验收**：能写出"经纬度 → 屏幕像素"调用链，并说明投影缓存（惰性失效键）与瓦片键边界回退（`TILE_EDGE_EPS` > 最坏插值误差）各自为什么必要。
