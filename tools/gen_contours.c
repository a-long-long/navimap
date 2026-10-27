/* gen_contours —— 从 SRTM .hgt 瓦片生成等高线（自包含，零外部依赖）。
 *
 * 为什么自写：本机没有 GDAL（gdal_contour / gdalwarp 全无），且环境里也没有
 * osmium / python osgeo。等高线不进 OSM 导入管线，直接产出 navimap 专属的三件套
 * 二进制（见 binfmt.h 的 NMBIN_KIND_CONTOURS / CONTOUR_PTS / CONTOUR_POLYREFS），
 * 由 mapset 直接 mmap、paint_contours 直接绘制。
 *
 * 算法：
 *   1. 解析目标 bbox（度），收集覆盖它的 1° SRTM 瓦片（skadi/N<lat>/N<lat>E<lon>.hgt，
 *      已解压为原始 .hgt 交给本工具）。
 *   2. 把每张瓦片（1201×1201 / 3601×3601，16-bit 有符号大端，void=-32768）按 bbox
 *      重采样合并进一张统一 DEM 网格（双线性，void 填均值）。
 *   3. 对 interval 的每个高程倍数跑 marching squares（含鞍部解析），得到线段集合。
 *   4. 按端点把线段缝合成折线（端点哈希 + 双向游走），写入三件套：
 *         contour_pts.bin      顶点池（lat/lon e7，8 字节）
 *         contour_polyrefs.bin 顶点下标序列（u32，CSR 式）
 *         contours.bin         每条等高线记录（高程 / ref_off / ref_cnt / class）
 *
 * 用法：
 *   gen_contours --out-dir=products/graph/beijing --hgt-dir=/tmp/srtm \
 *       --bbox=39.4,115.4,41.1,117.5 [--interval=20] [--index=100] \
 *       [--download[=MIRROR]]
 *
 * 加 --download 时，hgt-dir 里缺失的 1° 瓦片会从 SRTM 镜像（默认 AWS skadi）
 * 自动下载并解压（依赖 python3 + 网络，交给 tools/download_srtm.py 处理）；
 * 缺 python3 / 无网络时降级为"跳过缺失瓦片"，不致命。不带 --download 时行为
 * 与从前完全一致（仅读本地 .hgt）。本工具本身仍为零链接期外部依赖。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <inttypes.h>

#include "map/binfmt.h"

/* ---- 资源上限（防 OOM，超限即降级 / 告警，绝不静默画错）---- */
#define DEM_MAX_AXIS 3600      /* DEM 网格单边最多点数（3600²≈5200 万浮点≈21MB） */
#define MAX_SEG      3000000   /* 单层等高线最多线段数；超出则该层降级为不缝合 */
#define MAX_PTS      300000000 /* contour_pts 顶点数硬上限（≈2.4GB），超出停止 */

/* ============================ 工具函数 ============================ */

static int64_t llround64(double x) { return (int64_t)(x + (x>=0?0.5:-0.5)); }

static uint64_t h64(uint64_t x)
{
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33; return x;
}

/* ============================ SRTM 瓦片 ============================ */

typedef struct {
    int    tlat, tlon;     /* 西南角整数度 */
    int    N;              /* 每边点数（1201 / 3601） */
    float *g;              /* N×N，void 用 NAN 表示 */
} Tile;

static int16_t be16(const uint8_t *p)
{
    return (int16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

/* 读一张原始 .hgt（大端 int16）。成功返回 0，*outN / *outGrid 有效；失败返回 -1。 */
static int read_hgt(const char *path, int *outN, float **outGrid)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz < 2 || (sz % 2) != 0) { fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
    uint8_t *raw = (uint8_t *)malloc((size_t)sz);
    if (!raw) { fclose(f); return -1; }
    if (fread(raw, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(raw); return -1; }
    fclose(f);

    long cells = sz / 2;
    int N = (int)(0.5 + sqrt((double)cells));
    if ((long)N * N != cells) { free(raw); return -1; }

    float *g = (float *)malloc((size_t)N * N * sizeof(float));
    if (!g) { free(raw); return -1; }
    for (long i = 0; i < (long)N * N; i++) {
        int16_t v = be16(raw + 2 * i);
        g[i] = (v == -32768) ? nanf("") : (float)v;
    }
    free(raw);
    *outN = N; *outGrid = g;
    return 0;
}

/* 瓦片内双线性采样（行列浮点坐标，row0=北）。void 角点用邻角均值回填后再插值。 */
static float sample_tile(const float *g, int N, double rowf, double colf)
{
    int r0 = (int)floor(rowf), c0 = (int)floor(colf);
    if (r0 < 0) r0 = 0;
    if (c0 < 0) c0 = 0;
    int r1 = r0 + 1, c1 = c0 + 1;
    if (r1 > N - 1) r1 = N - 1;
    if (c1 > N - 1) c1 = N - 1;
    float v[4];
    v[0] = g[(size_t)r0 * N + c0];
    v[1] = g[(size_t)r0 * N + c1];
    v[2] = g[(size_t)r1 * N + c0];
    v[3] = g[(size_t)r1 * N + c1];
    int cnt = 0; float sum = 0.0f;
    for (int k = 0; k < 4; k++) if (!isnan(v[k])) { cnt++; sum += v[k]; }
    if (cnt == 0) return nanf("");
    float mean = sum / cnt;
    for (int k = 0; k < 4; k++) if (isnan(v[k])) v[k] = mean;
    double tr = rowf - r0, tc = colf - c0;
    double top = v[0] * (1.0 - tc) + v[1] * tc;
    double bot = v[2] * (1.0 - tc) + v[3] * tc;
    return (float)(top * (1.0 - tr) + bot * tr);
}

/* ============================ 自动下载 DEM ============================ */
/* 仅当 --download 时启用：借用 tools/download_srtm.py（纯标准库，零第三方依赖）
 * 预取 bbox 内缺失的 SRTM .hgt 瓦片，再走既有加载逻辑。无 python3 / 无网络时
 * 降级为现有的"跳过缺失瓦片"行为，不会致命退出。 */

static const char *g_argv0       = "";     /* 存 argv[0]，用于定位 tools/ 下的脚本 */
static int        g_do_download  = 0;      /* --download 开关 */
static const char *g_dl_mirror   = NULL;   /* --download=URL 的可选镜像 */
static const char *g_dl_override = NULL;   /* --downloader=PATH 显式脚本路径 */

static int file_exists(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

/* 解析 argv0 的目录（兼容 / 与 \\），写入 buf 并返回。 */
static const char *argv0_dir(const char *argv0, char *buf, size_t n)
{
    const char *last = NULL, *p = argv0;
    for (; *p; p++) if (*p == '/' || *p == '\\') last = p;
    if (!last) { buf[0] = '.'; buf[1] = '\0'; return buf; }
    size_t len = (size_t)(last - argv0);
    if (len >= n) len = n - 1;
    memcpy(buf, argv0, len);
    buf[len] = '\0';
    return buf;
}

/* 解析 download_srtm.py 的路径（优先级：--downloader > 环境变量 > 候选位置）。 */
static const char *find_downloader(const char *override)
{
    static char buf[4096];
    if (override && *override) return override;
    const char *e = getenv("GEN_CONTOURS_PY");
    if (e && *e && file_exists(e)) return e;

    char d[4096];
    argv0_dir(g_argv0, d, sizeof(d));
    snprintf(buf, sizeof(buf), "%s/../tools/download_srtm.py", d);
    if (file_exists(buf)) return buf;
    if (file_exists("tools/download_srtm.py")) return "tools/download_srtm.py";
    const char *root = getenv("NAVIMAP_ROOT");
    if (root && *root) {
        snprintf(buf, sizeof(buf), "%s/tools/download_srtm.py", root);
        if (file_exists(buf)) return buf;
    }
    return NULL;
}

/* 调起 download_srtm.py 预取缺失瓦片。成功/降级返回 0，找不到脚本返回 -1。 */
static int run_download(const char *bbox, const char *hgt_dir,
                        const char *mirror, const char *dl_override)
{
    const char *script = find_downloader(dl_override);
    if (!script) {
        fprintf(stderr, "  [警告] 未找到 download_srtm.py，跳过自动下载。"
                        "请用 --downloader=路径 指定，或确认 tools/download_srtm.py 存在。\n");
        return -1;
    }
    char cmd[8192];
    int off = snprintf(cmd, sizeof(cmd),
                       "python3 \"%s\" --bbox=\"%s\" --out=\"%s\"",
                       script, bbox, hgt_dir);
    if (mirror && *mirror)
        snprintf(cmd + off, sizeof(cmd) - (size_t)off, " --mirror=\"%s\"", mirror);
    fprintf(stderr, "  [自动下载 DEM] %s\n", cmd);
    int rc = system(cmd);
    if (rc != 0) {
        fprintf(stderr, "  [警告] 自动下载 DEM 返回非零（rc=%d）：需 python3 与网络。"
                        "将仅使用本地已有的 .hgt 瓦片。\n", rc);
        return -1;
    }
    return 0;
}

/* ============================ 主流程 ============================ */

/* 解析 "lat0,lon0,lat1,lon1" */
static int parse_bbox(const char *s, double *lat0, double *lon0,
                      double *lat1, double *lon1)
{
    char *p = strdup(s);
    if (!p) return -1;
    int n = sscanf(p, "%lf,%lf,%lf,%lf", lat0, lon0, lat1, lon1);
    free(p);
    if (n != 4) return -1;
    if (*lat0 > *lat1) { double t = *lat0; *lat0 = *lat1; *lat1 = t; }
    if (*lon0 > *lon1) { double t = *lon0; *lon0 = *lon1; *lon1 = t; }
    return 0;
}

/* 哈希表：端点(量化) → 线段端点节点（链式）。用于把线段缝合成折线。 */
typedef struct { int32_t qx, qy; int seg; int which; int next; } HNode;

int main(int argc, char **argv)
{
    g_argv0 = argv[0];
    const char *out_dir = NULL, *hgt_dir = NULL, *bbox = NULL;
    const char *prefix = "contour";  // 默认前缀
    double lat0 = 0, lon0 = 0, lat1 = 0, lon1 = 0;
    double interval = 20.0, index_int = 100.0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strncmp(a, "--out-dir=", 10))        out_dir = a + 10;
        else if (!strncmp(a, "--hgt-dir=", 10))    hgt_dir = a + 10;
        else if (!strncmp(a, "--bbox=", 7))        bbox = a + 7;
        else if (!strncmp(a, "--interval=", 11))   interval = atof(a + 11);
        else if (!strncmp(a, "--index=", 8))       index_int = atof(a + 8);
        else if (!strncmp(a, "--prefix=", 9))      prefix = a + 9;  // 新增前缀参数
        else if (!strcmp(a, "--download"))         g_do_download = 1;
        else if (!strncmp(a, "--download=", 11))   { g_do_download = 1; g_dl_mirror = a + 11; }
        else if (!strncmp(a, "--downloader=", 13)) g_dl_override = a + 13;
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            fprintf(stderr,
                "用法: gen_contours --out-dir=DIR --hgt-dir=DIR "
                "--bbox=lat0,lon0,lat1,lon1 [--interval=M] [--index=M]\n"
                "      --prefix=NAME      输出文件前缀（默认 \"contour\" → NAME_contours.bin）\n"
                "      --download[=MIRROR]  缺失的 SRTM 瓦片自动从 MIRROR 下载"
                "（默认 AWS skadi），需 python3 与网络\n"
                "      --downloader=PATH    指定 download_srtm.py 路径（默认自动探测）\n");
            return 2;
        } else {
            fprintf(stderr, "未知参数: %s\n", a); return 2;
        }
    }
    if (!out_dir || !hgt_dir || !bbox) {
        fprintf(stderr, "缺少必需参数 --out-dir / --hgt-dir / --bbox\n");
        return 2;
    }
    if (parse_bbox(bbox, &lat0, &lon0, &lat1, &lon1) != 0) {
        fprintf(stderr, "bbox 格式错（应为 lat0,lon0,lat1,lon1）\n"); return 2;
    }
    if (interval <= 0) interval = 20.0;
    if (index_int <= 0) index_int = 100.0;

    /* ---------- 1. 收集并加载覆盖 bbox 的 1° 瓦片 ---------- */
    if (g_do_download)
        run_download(bbox, hgt_dir, g_dl_mirror, g_dl_override);

    int tlat_lo = (int)floor(lat0), tlat_hi = (int)floor(lat1);
    int tlon_lo = (int)floor(lon0), tlon_hi = (int)floor(lon1);
    int ntiles = 0;
    Tile *tiles = NULL;
    float *H = NULL;
    int rc = 1;
    for (int tlat = tlat_lo; tlat <= tlat_hi; tlat++) {
        for (int tlon = tlon_lo; tlon <= tlon_hi; tlon++) {
            char name[64];
            const char *ns = (tlat >= 0) ? "N" : "S";
            const char *ew = (tlon >= 0) ? "E" : "W";
            snprintf(name, sizeof(name), "%s%02d%s%03d.hgt",
                     ns, abs(tlat), ew, abs(tlon));
            char path[4096];
            snprintf(path, sizeof(path), "%s/%s", hgt_dir, name);
            int N; float *g;
            if (read_hgt(path, &N, &g) != 0) {
                fprintf(stderr, "  跳过瓦片 %s（读不到，可能落在海洋/无数据区）\n", name);
                continue;
            }
            Tile *t = realloc(tiles, (size_t)(ntiles + 1) * sizeof(Tile));
            if (!t) { free(g); fprintf(stderr, "OOM\n"); goto cleanup; }
            tiles = t;
            tiles[ntiles].tlat = tlat; tiles[ntiles].tlon = tlon;
            tiles[ntiles].N = N; tiles[ntiles].g = g;
            ntiles++;
        }
    }
    if (ntiles == 0) {
        fprintf(stderr, "没有任何可用 SRTM 瓦片（hgt-dir=%s），无法生成等高线\n", hgt_dir);
        return 1;
    }
    fprintf(stderr, "已加载 %d 张 SRTM 瓦片\n", ntiles);

    /* 瓦片快速索引：tile_index[(tlat-minLat)*spanLon + (tlon-minLon)] */
    int minLat = tlat_lo, minLon = tlon_lo;
    int spanLat = tlat_hi - tlat_lo + 1, spanLon = tlon_hi - tlon_lo + 1;
    int *tidx = (int *)malloc((size_t)spanLat * spanLon * sizeof(int));
    if (!tidx) { fprintf(stderr, "OOM\n"); goto cleanup; }
    for (int k = 0; k < spanLat * spanLon; k++) tidx[k] = -1;
    for (int k = 0; k < ntiles; k++)
        tidx[(tiles[k].tlat - minLat) * spanLon + (tiles[k].tlon - minLon)] = k;

    /* ---------- 2. 重采样合并成统一 DEM 网格 ---------- */
    double span_lat = lat1 - lat0, span_lon = lon1 - lon0;
    int nx = (int)ceil(span_lon / (1.0 / 1200.0)) + 1;
    int ny = (int)ceil(span_lat / (1.0 / 1200.0)) + 1;
    if (nx < 2) nx = 2;
    if (ny < 2) ny = 2;
    if (nx > DEM_MAX_AXIS) nx = DEM_MAX_AXIS;
    if (ny > DEM_MAX_AXIS) ny = DEM_MAX_AXIS;
    double dx = span_lon / (nx - 1), dy = span_lat / (ny - 1);

    H = (float *)malloc((size_t)nx * ny * sizeof(float));
    if (!H) { fprintf(stderr, "OOM\n"); goto cleanup; }
    for (int j = 0; j < ny; j++) {
        double lat = lat0 + j * dy;
        int tlat = (int)floor(lat);
        for (int i = 0; i < nx; i++) {
            double lon = lon0 + i * dx;
            int tlon = (int)floor(lon);
            float val = nanf("");
            if (tlat >= minLat && tlat < minLat + spanLat &&
                tlon >= minLon && tlon < minLon + spanLon) {
                int ti = tidx[(tlat - minLat) * spanLon + (tlon - minLon)];
                if (ti >= 0) {
                    Tile *T = &tiles[ti];
                    double fy = lat - tlat;          /* 0=南 1=北 */
                    double fx = lon - tlon;          /* 0=西 1=东 */
                    double rowf = (1.0 - fy) * (T->N - 1);
                    double colf = fx * (T->N - 1);
                    val = sample_tile(T->g, T->N, rowf, colf);
                }
            }
            H[(size_t)j * nx + i] = val;
        }
    }

    /* 高程范围（仅有限值） */
    float hmin = nanf(""), hmax = nanf("");
    for (int k = 0; k < nx * ny; k++) {
        float v = H[k];
        if (isnan(v)) continue;
        if (isnan(hmin) || v < hmin) hmin = v;
        if (isnan(hmax) || v > hmax) hmax = v;
    }
    if (isnan(hmin) || isnan(hmax)) {
        fprintf(stderr, "DEM 全为 void，无法生成等高线\n"); goto cleanup;
    }
    fprintf(stderr, "DEM 网格 %dx%d，高程范围 %.0f..%.0f m，间隔 %.0f m（计曲线 %.0f m）\n",
            nx, ny, hmin, hmax, interval, index_int);

    /* ---------- 3. 准备输出三件套（使用前缀命名） ---------- */
    char p_cont[4096], p_poly[4096], p_pts[4096];
    snprintf(p_cont, sizeof(p_cont), "%s/%s_contours.bin", out_dir, prefix);
    snprintf(p_poly, sizeof(p_poly), "%s/%s_contour_polyrefs.bin", out_dir, prefix);
    snprintf(p_pts,  sizeof(p_pts),  "%s/%s_contour_pts.bin", out_dir, prefix);

    NMBinWriter *wc  = nmbin_create(p_cont, NMBIN_KIND_CONTOURS, NM_CONTOUR_BYTES, "gen_contours");
    NMBinWriter *wp  = nmbin_create(p_poly, NMBIN_KIND_CONTOUR_POLYREFS, 4, "gen_contours");
    NMBinWriter *wpt = nmbin_create(p_pts,  NMBIN_KIND_CONTOUR_PTS, NM_CONTOUR_PT_BYTES, "gen_contours");
    if (!wc || !wp || !wpt) {
        fprintf(stderr, "无法创建输出文件（目录存在？）\n");
        if (wc) nmbin_abort(wc);
        if (wp) nmbin_abort(wp);
        if (wpt) nmbin_abort(wpt);
        goto cleanup;
    }
    nmbin_add_field(wc, "elev",    CF_ELEV,     NMT_U64, 8);
    nmbin_add_field(wc, "ref_off", CF_REF_OFF,  NMT_U32, 4);
    nmbin_add_field(wc, "ref_cnt", CF_REF_CNT,  NMT_U32, 4);
    nmbin_add_field(wc, "class",   CF_CLASS,    NMT_U8,  1);
    nmbin_add_field(wc, "flags",   CF_FLAGS,    NMT_U8,  1);
    nmbin_add_field(wc, "name_id", CF_NAME_ID,  NMT_U32, 4);
    nmbin_add_field(wpt, "lat", CP_LAT, NMT_I32, 4);
    nmbin_add_field(wpt, "lon", CP_LON, NMT_I32, 4);
    nmbin_add_field(wp, "idx", 0, NMT_U32, 4);

    uint64_t total_contours = 0, total_pts = 0;
    int overflow = 0;

    /* 逐层 marching squares + 缝合 */
    long L0 = (long)(ceil(hmin / interval) * interval);
    long L1 = (long)(floor(hmax / interval) * interval);
    for (long L = L0; L <= L1; L += (long)interval) {
        int is_index = (index_int > 0 && fmod((double)L, index_int) < 1e-6) ? 1 : 0;

        /* 线段缓冲（本层一次性） */
        int cap = 1024;
        int nseg = 0;
        double *sx0 = malloc((size_t)cap * sizeof(double));
        double *sy0 = malloc((size_t)cap * sizeof(double));
        double *sx1 = malloc((size_t)cap * sizeof(double));
        double *sy1 = malloc((size_t)cap * sizeof(double));
        int32_t *sqa = malloc((size_t)cap * sizeof(int32_t));
        int32_t *sqb = malloc((size_t)cap * sizeof(int32_t));
        if (!sx0 || !sy0 || !sx1 || !sy1 || !sqa || !sqb) { overflow = 1; break; }

        /* 边中点插值助手（在格子坐标上） */
        #define ADDSEG(ax,ay,bx,by) do {                                            \
            if (nseg >= cap) { int ncap = cap * 2;                                  \
                sx0=realloc(sx0,ncap*sizeof(double)); sy0=realloc(sy0,ncap*sizeof(double)); \
                sx1=realloc(sx1,ncap*sizeof(double)); sy1=realloc(sy1,ncap*sizeof(double)); \
                sqa=realloc(sqa,ncap*sizeof(int32_t)); sqb=realloc(sqb,ncap*sizeof(int32_t)); \
                if(!sx0||!sy0||!sx1||!sy1||!sqa||!sqb){overflow=1;break;} cap=ncap; } \
            sx0[nseg]=ax; sy0[nseg]=ay; sx1[nseg]=bx; sy1[nseg]=by;                 \
            sqa[nseg]=(int32_t)llround64(ax/1e-9); sqb[nseg]=(int32_t)llround64(by/1e-9); \
            nseg++;                                                                \
        } while(0)

        for (int j = 0; j + 1 < ny; j++) {
            for (int i = 0; i + 1 < nx; i++) {
                float vTL = H[(size_t)(j+1)*nx + i];
                float vTR = H[(size_t)(j+1)*nx + i+1];
                float vBR = H[(size_t)j*nx + i+1];
                float vBL = H[(size_t)j*nx + i];
                if (isnan(vTL) && isnan(vTR) && isnan(vBR) && isnan(vBL)) continue;

                int aTL = (!isnan(vTL) && vTL >= (float)L) ? 1 : 0;
                int aTR = (!isnan(vTR) && vTR >= (float)L) ? 1 : 0;
                int aBR = (!isnan(vBR) && vBR >= (float)L) ? 1 : 0;
                int aBL = (!isnan(vBL) && vBL >= (float)L) ? 1 : 0;
                int cnt = aTL + aTR + aBR + aBL;
                if (cnt == 0 || cnt == 4) continue;

                double xL = lon0 + i * dx, xR = lon0 + (i+1) * dx;
                double yB = lat0 + j * dy, yT = lat0 + (j+1) * dy;
                double px[4], py[4]; int np = 0;

                /* 顶边 TL-TR */
                if (aTL != aTR) {
                    double t = ((double)L - vTL) / (vTR - vTL);
                    px[np] = xL + t * (xR - xL); py[np] = yT; np++;
                }
                /* 右边 TR-BR */
                if (aTR != aBR) {
                    double t = ((double)L - vTR) / (vBR - vTR);
                    px[np] = xR; py[np] = yT + t * (yB - yT); np++;
                }
                /* 底边 BR-BL */
                if (aBR != aBL) {
                    double t = ((double)L - vBR) / (vBL - vBR);
                    px[np] = xR + t * (xL - xR); py[np] = yB; np++;
                }
                /* 左边 BL-TL */
                if (aBL != aTL) {
                    double t = ((double)L - vBL) / (vTL - vBL);
                    px[np] = xL; py[np] = yB + t * (yT - yB); np++;
                }

                if (np == 2) {
                    ADDSEG(px[0], py[0], px[1], py[1]);
                } else if (np == 4) {
                    /* 鞍部：用四角均值判定中心"高于/低于"本层，决定配对 */
                    float m = (vTL + vTR + vBR + vBL) / 4.0f;
                    int above_TL = aTL;   /* checkerboard：above 集 = {TL,BR} 或 {TR,BL} */
                    if (above_TL) { /* above={TL,BR} */
                        if (m >= (float)L) { ADDSEG(px[0],py[0],px[3],py[3]); ADDSEG(px[1],py[1],px[2],py[2]); }
                        else               { ADDSEG(px[0],py[0],px[1],py[1]); ADDSEG(px[2],py[2],px[3],py[3]); }
                    } else {       /* above={TR,BL} */
                        if (m >= (float)L) { ADDSEG(px[0],py[0],px[1],py[1]); ADDSEG(px[2],py[2],px[3],py[3]); }
                        else               { ADDSEG(px[0],py[0],px[3],py[3]); ADDSEG(px[1],py[1],px[2],py[2]); }
                    }
                }
                if (overflow) break;
            }
            if (overflow) break;
        }

        if (overflow) { free(sx0);free(sy0);free(sx1);free(sy1);free(sqa);free(sqb); break; }

        /* ---------- 4. 缝合线段成折线 ---------- */
        if (nseg == 0) { free(sx0);free(sy0);free(sx1);free(sy1);free(sqa);free(sqb); continue; }

        if (nseg > MAX_SEG) {
            fprintf(stderr, "  层 %ld 线段 %d 超上限，降级：不缝合直接输出（仅本层）\n", L, nseg);
        }

        /* 端点哈希（用于缝合；nseg 不超上限时才建） */
        int nbucket = 1;
        while ((long)nbucket * 2 < (long)nseg * 2 + 16) nbucket *= 2;
        int *bucket = NULL; HNode *nodes = NULL; int nn = 0;
        int do_stitch = (nseg <= MAX_SEG);
        if (do_stitch) {
            bucket = (int *)malloc((size_t)nbucket * sizeof(int));
            nodes  = (HNode *)malloc((size_t)nseg * 2 * sizeof(HNode));
            if (!bucket || !nodes) do_stitch = 0;
            else { for (int b = 0; b < nbucket; b++) bucket[b] = -1; }
        }

        if (do_stitch) {
            for (int s = 0; s < nseg; s++) {
                for (int e = 0; e < 2; e++) {
                    int32_t qx = (e == 0) ? sqa[s] : sqb[s];
                    int32_t qy = (e == 0) ? sqb[s] : sqa[s];
                    uint64_t key = ((uint64_t)(uint32_t)qx << 32) | (uint32_t)qy;
                    int b = (int)(h64(key) % (uint64_t)nbucket);
                    int idx = nn++;
                    nodes[idx].qx = qx; nodes[idx].qy = qy;
                    nodes[idx].seg = s; nodes[idx].which = e;
                    nodes[idx].next = bucket[b]; bucket[b] = idx;
                }
            }
        }

        char *visited = NULL;
        if (do_stitch) { visited = (char *)calloc((size_t)nseg, 1); if (!visited) do_stitch = 0; }

        /* 折线输出缓冲（本层所有折线共享，逐条写出后释放） */
        /* 先收集本层所有折线到内存，再统一写出，避免每点一次写调用 */
        #define MAX_PLINE_PTS 2000000
        int   *pline_x = malloc(sizeof(int) * MAX_PLINE_PTS);
        int   *pline_y = malloc(sizeof(int) * MAX_PLINE_PTS);
        if (!pline_x || !pline_y) { /* 极端情况：跳过本层缝合，直接退化为不缝合 */
            do_stitch = 0; free(pline_x); free(pline_y); pline_x = pline_y = NULL;
        }

        if (do_stitch) {
            for (int s0 = 0; s0 < nseg; s0++) {
                if (visited[s0]) continue;
                visited[s0] = 1;
                /* 起点链：chain 存 (lat_e7, lon_e7) 交错 */
                int cx[8192], cy[8192]; int cn = 0;
                if (cn < 8192) { cx[cn] = (int)llround64(sy0[s0]*1e7); cy[cn] = (int)llround64(sx0[s0]*1e7); cn++; }
                if (cn < 8192) { cx[cn] = (int)llround64(sy1[s0]*1e7); cy[cn] = (int)llround64(sx1[s0]*1e7); cn++; }

                /* 前向扩展（从 B 端） */
                int32_t cur_qx = sqb[s0], cur_qy = sqb[s0]; /* B 端点 key */
                for (;;) {
                    uint64_t key = ((uint64_t)(uint32_t)cur_qx << 32) | (uint32_t)cur_qy;
                    int b = (int)(h64(key) % (uint64_t)nbucket);
                    int found = -1;
                    for (int it = bucket[b]; it >= 0; it = nodes[it].next) {
                        if (nodes[it].seg == s0) continue;
                        if (visited[nodes[it].seg]) continue;
                        if (nodes[it].qx == cur_qx && nodes[it].qy == cur_qy) { found = it; break; }
                    }
                    if (found < 0) break;
                    int ts = nodes[found].seg; visited[ts] = 1;
                    /* 另一端坐标 */
                    double ox, oy;
                    if (nodes[found].which == 0) { ox = sx1[ts]; oy = sy1[ts]; cur_qx = sqb[ts]; cur_qy = sqb[ts]; }
                    else                         { ox = sx0[ts]; oy = sy0[ts]; cur_qx = sqa[ts]; cur_qy = sqa[ts]; }
                    if (cn < 8192) { cx[cn] = (int)llround64(oy*1e7); cy[cn] = (int)llround64(ox*1e7); cn++; }
                    else break;
                }
                /* 后向扩展（从 A 端） */
                cur_qx = sqa[s0]; cur_qy = sqa[s0];
                for (;;) {
                    uint64_t key = ((uint64_t)(uint32_t)cur_qx << 32) | (uint32_t)cur_qy;
                    int b = (int)(h64(key) % (uint64_t)nbucket);
                    int found = -1;
                    for (int it = bucket[b]; it >= 0; it = nodes[it].next) {
                        if (nodes[it].seg == s0) continue;
                        if (visited[nodes[it].seg]) continue;
                        if (nodes[it].qx == cur_qx && nodes[it].qy == cur_qy) { found = it; break; }
                    }
                    if (found < 0) break;
                    int ts = nodes[found].seg; visited[ts] = 1;
                    double ox, oy;
                    if (nodes[found].which == 0) { ox = sx1[ts]; oy = sy1[ts]; cur_qx = sqb[ts]; cur_qy = sqb[ts]; }
                    else                         { ox = sx0[ts]; oy = sy0[ts]; cur_qx = sqa[ts]; cur_qy = sqa[ts]; }
                    /* 前插到链首 */
                    if (cn < 8192) {
                        for (int k = cn; k > 0; k--) { cx[k] = cx[k-1]; cy[k] = cy[k-1]; }
                        cx[0] = (int)llround64(oy*1e7); cy[0] = (int)llround64(ox*1e7); cn++;
                    } else break;
                }

                if (cn < 2) { /* 退化：理论上不会发生 */ continue; }
                /* 写折线：pts + polyrefs + 记录 */
                if (total_pts + (uint64_t)cn > MAX_PTS) { overflow = 1; break; }
                /* pts */
                uint8_t rec8[8];
                for (int k = 0; k < cn; k++) {
                    nm_put_i32(rec8 + CP_LAT, cx[k]);
                    nm_put_i32(rec8 + CP_LON, cy[k]);
                    nmbin_write_records(wpt, rec8, 1);
                }
                /* polyrefs */
                uint32_t base = (uint32_t)total_pts;
                uint8_t rec4[4];
                for (int k = 0; k < cn; k++) {
                    nm_put_u32(rec4, base + (uint32_t)k);
                    nmbin_write_records(wp, rec4, 1);
                }
                /* contour 记录 */
                uint8_t rec[NM_CONTOUR_BYTES];
                memset(rec, 0, sizeof(rec));
                nm_put_u64(rec + CF_ELEV, (uint64_t)L);
                nm_put_u32(rec + CF_REF_OFF, base);
                nm_put_u32(rec + CF_REF_CNT, (uint32_t)cn);
                rec[CF_CLASS] = (uint8_t)is_index;
                nmbin_write_records(wc, rec, 1);

                total_contours++;
                total_pts += (uint64_t)cn;
            }
        } else {
            /* 不缝合：每条线段当一条 2 点等高线 */
            for (int s = 0; s < nseg; s++) {
                if (total_pts + 2 > MAX_PTS) { overflow = 1; break; }
                int32_t ax = (int32_t)llround64(sy0[s]*1e7), ay = (int32_t)llround64(sx0[s]*1e7);
                int32_t bx = (int32_t)llround64(sy1[s]*1e7), by = (int32_t)llround64(sx1[s]*1e7);
                uint8_t rec8[8];
                nm_put_i32(rec8 + CP_LAT, ax); nm_put_i32(rec8 + CP_LON, ay);
                nmbin_write_records(wpt, rec8, 1);
                nm_put_i32(rec8 + CP_LAT, bx); nm_put_i32(rec8 + CP_LON, by);
                nmbin_write_records(wpt, rec8, 1);
                uint8_t rec4[4];
                uint32_t base = (uint32_t)total_pts;
                nm_put_u32(rec4, base);   nmbin_write_records(wp, rec4, 1);
                nm_put_u32(rec4, base+1); nmbin_write_records(wp, rec4, 1);
                uint8_t rec[NM_CONTOUR_BYTES];
                memset(rec, 0, sizeof(rec));
                nm_put_u64(rec + CF_ELEV, (uint64_t)L);
                nm_put_u32(rec + CF_REF_OFF, base);
                nm_put_u32(rec + CF_REF_CNT, 2);
                rec[CF_CLASS] = (uint8_t)is_index;
                nmbin_write_records(wc, rec, 1);
                total_contours++; total_pts += 2;
            }
        }

        free(sx0); free(sy0); free(sx1); free(sy1); free(sqa); free(sqb);
        free(bucket); free(nodes); free(visited);
        free(pline_x); free(pline_y);
        if (overflow) break;
        #undef ADDSEG
        #undef MAX_PLINE_PTS
    }

    rc = (nmbin_finalize(wc) == 0 && nmbin_finalize(wp) == 0 &&
              nmbin_finalize(wpt) == 0) ? 0 : 1;
    if (overflow)
        fprintf(stderr, "警告：顶点数达到上限，等高线被截断（调大 MAX_PTS 或加粗 --interval）\n");
    fprintf(stderr, "等高线生成完成：%llu 条，%llu 个顶点 → %s (前缀: %s)\n",
            (unsigned long long)total_contours, (unsigned long long)total_pts, out_dir, prefix);

cleanup:
    if (tiles) { for (int k = 0; k < ntiles; k++) free(tiles[k].g); free(tiles); }
    free(H);
    return rc;
}
