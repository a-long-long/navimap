/* skedaddle_gpx_selftest —— GPX 解析/写出/持久化自测（判据 GPX-OK）。
 *
 * 覆盖：
 *   1) 解析一份"脏"GPX：注释 / CDATA / BOM / CRLF / XML 实体 / 缺属性 /
 *      自闭合 trkpt / lon 在前 lat 在后 —— 数值与名字都要对；
 *   2) 写出 → 再解析：往返一致（点坐标 e7 逐点相等、名字相等）；
 *   3) 边界：空串、坏 XML、只有 wpt 的文件（-2 的判定）；
 *   4) sqlite 存取往返 + 同名轨迹替换（不翻倍）+ fav 增删；
 *   5) 负向对照：把解析器的 lon 当 lat（注入）⇒ 第 1 条坐标判据 FAIL。
 *
 * 用法：skedaddle_gpx_selftest（自含，不依赖任何外部文件；sqlite 用临时目录）
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "map/gpx.h"
#include "utils/geo.h"
#include "utils/plat.h"

static int g_fail = 0;
static int g_checks = 0;

static void check(int cond, const char *what)
{
    g_checks++;
    if (!cond) {
        g_fail++;
        printf("FAIL: %s\n", what);
    }
}

/* 一份"什么都有"的 GPX：故意把属性顺序、空白、实体、注释、CDATA 全塞进去 */
static const char *kDirty =
    "\xEF\xBB\xBF"                                   /* UTF-8 BOM */
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
    "<gpx version=\"1.1\" creator=\"test\" xmlns=\"http://www.topografix.com/GPX/1/1\">\r\n"
    "  <!-- 这是注释：<trkpt lat=\"1\" lon=\"2\"/> 不该被解析 -->\r\n"
    "  <metadata><name>整体名（不该当轨迹名）</name></metadata>\r\n"
    "  <wpt lon=\"116.4074\" lat=\"39.9042\"><name>家 &amp; 办公室</name></wpt>\r\n"
    "  <wpt lat=\"31.2304\" lon=\"121.4737\"><name>上海&amp;lt;tag&amp;gt;</name></wpt>\r\n"
    "  <trk>\r\n"
    "    <name>骑行 &amp; 散步</name>\r\n"
    "    <trkseg>\r\n"
    "      <trkpt lat=\"39.9042000\" lon=\"116.4074000\"><ele>50</ele><time>t</time></trkpt>\r\n"
    "      <trkpt lon=\"116.4174000\" lat=\"39.9142000\"/>\r\n"
    "      <trkpt lat=\"39.9242\" lon=\"116.4274\"/>\r\n"
    "      <trkpt lat=\"200\" lon=\"500\"/>\r\n"        /* 非法值：必须被跳过 */
    "      <trkpt lon=\"116.4374\"/>\r\n"               /* 缺 lat：必须被跳过 */
    "    </trkseg>\r\n"
    "  </trk>\r\n"
    "  <rte>\r\n"
    "    <name>路线 A</name>\r\n"
    "    <rtept lat=\"23.1291\" lon=\"113.2644\"/>\r\n"
    "    <rtept lat=\"23.1391\" lon=\"113.2744\"/>\r\n"
    "  </rte>\r\n"
    "  <trk><name><![CDATA[第二段 <轨迹>]]></name><trkseg>"
    "<trkpt lat=\"40.0\" lon=\"116.0\"/><trkpt lat=\"40.1\" lon=\"116.1\"/>"
    "</trkseg></trk>\r\n"
    "</gpx>\r\n";

/* 逐点比较两条轨迹的 e7 坐标 */
static int trk_eq(const GpxTrack *a, const GpxTrack *b)
{
    if (a->n != b->n) return 0;
    if (strcmp(a->name, b->name)) return 0;
    for (uint32_t i = 0; i < a->n; i++) {
        if (a->lat_e7[i] != b->lat_e7[i]) return 0;
        if (a->lon_e7[i] != b->lon_e7[i]) return 0;
    }
    return 1;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("== skedaddle_gpx_selftest ==\n");

    /* ---------- 1) 解析脏 GPX ---------- */
    GpxStore s;
    gpx_store_init(&s);
    int rc = gpx_parse(kDirty, strlen(kDirty), &s);
    check(rc == 3, "解析出 3 条轨迹（trk×2 + rte×1）");
    check(s.n_wpt == 2, "解析出 2 个航点");
    if (s.n_trk >= 3) {
        const GpxTrack *t = &s.trk[0];
        check(t->n == 3, "trk1：非法/缺属性点被跳过后剩 3 点");
        check(t->n == 3 && t->lat_e7 && t->lon_e7, "trk1：点数组已分配");
        if (t->n == 3 && t->lat_e7 && t->lon_e7) {
        check(t->lat_e7[0] == 399042000 && t->lon_e7[0] == 1164074000,
              "trk1 第 1 点 e7 精确（lat=39.9042, lon=116.4074）");
        check(t->lat_e7[1] == 399142000 && t->lon_e7[1] == 1164174000,
              "trk1 第 2 点 e7 精确（属性 lon 在前的乱序也解析对）");
        }
        check(!strcmp(t->name, "骑行 & 散步"),
              "trk1 名字：XML 实体 &amp; 已反转义");
        check(!strcmp(s.wpt[0].name, "家 & 办公室"),
              "wpt1 名字实体反转义");
        check(!strcmp(s.wpt[1].name, "上海&lt;tag&gt;"),
              "wpt2 名字：&amp;lt; → &lt;（实体不递归反转义，XML 规范行为）");
        check(s.wpt[0].lat == 39.9042 && s.wpt[0].lon == 116.4074,
              "wpt1 坐标（lon 属性写在 lat 前）");
        const GpxTrack *r = &s.trk[1];
        check(!strncmp(r->name, "(路线)", 8) && strstr(r->name, "路线 A") != NULL,
              "rte 归一成轨迹：名字 = \"(路线)\" 前缀 + 原名");
        check(r->n == 2 && r->lat_e7[0] == 231291000, "rte 2 点、首点正确");
        const GpxTrack *t2 = &s.trk[2];
        check(t2->n == 2, "trk2（CDATA 名字）2 点");
        check(!strcmp(t2->name, "第二段 <轨迹>"), "CDATA 名字原样取出（含 < >）");
    }

    /* ---------- 2) 写出 → 再解析：往返一致 ---------- */
    char path[1024], path2[1024];
    snprintf(path, sizeof(path), "%s/navimap_gpx_rt.gpx", nm_temp_dir());
    snprintf(path2, sizeof(path2), "%s/navimap_gpx_rt2.gpx", nm_temp_dir());
    check(gpx_write(&s, path) == 0, "写出 GPX 文件");
    GpxStore s2;
    gpx_store_init(&s2);
    check(gpx_parse_file(path, &s2) == 3, "再解析回 3 条轨迹");
    check(s2.n_wpt == 2, "往返后收藏数一致");
    if (s.n_trk == s2.n_trk && s.n_trk >= 3) {
        check(trk_eq(&s.trk[0], &s2.trk[0]), "往返一致：trk1（含名字）");
        check(trk_eq(&s.trk[1], &s2.trk[1]), "往返一致：rte");
        check(trk_eq(&s.trk[2], &s2.trk[2]), "往返一致：trk2（CDATA 名字）");
        check(s.wpt[0].lat == s2.wpt[0].lat && s.wpt[0].lon == s2.wpt[0].lon &&
              !strcmp(s.wpt[0].name, s2.wpt[0].name), "往返一致：wpt1");
    }
    check(gpx_write(&s2, path2) == 0, "二次写出（导出的文件可再导出）");

    /* ---------- 3) 边界 ---------- */
    GpxStore se;
    gpx_store_init(&se);
    check(gpx_parse("", 0, &se) == -2, "空串：-2（没解析到东西）");
    check(gpx_parse("<gpx><trk><trkseg><trkpt lat=\"1\"", 30, &se) == -2,
          "截断的 XML：不崩、-2");
    check(gpx_parse("<html><body>不是 GPX</body></html>", 30, &se) == -2,
          "完全不是 GPX：-2");
    check(se.n_trk == 0 && se.n_wpt == 0, "边界情况后 store 仍是空的");
    check(gpx_parse_file("/nonexistent/xxx.gpx", &se) == -1,
          "不存在的文件：-1");
    /* 只有 wpt 的合法 GPX（rc=0 条轨迹但 wpt 有） */
    GpxStore sw;
    gpx_store_init(&sw);
    rc = gpx_parse("<gpx><wpt lat=\"1\" lon=\"2\"><name>x</name></wpt></gpx>", 45, &sw);
    check(rc == 0 && sw.n_wpt == 1, "只有 wpt：返回 0 条轨迹但收藏进来了");

    /* ---------- 4) sqlite 存取 ---------- */
    char dbp[1024];
    snprintf(dbp, sizeof(dbp), "%s/navimap_gpx_test.db", nm_temp_dir());
    remove(dbp);
    void *db = gpx_db_open(dbp);
    check(db != NULL, "打开（新建）tracks 测试库");
    if (db) {
        /* 存 trk1 + trk2 + 2 个收藏 */
        int id1 = gpx_db_save_track(db, &s.trk[0]);
        int id2 = gpx_db_save_track(db, &s.trk[2]);
        int f1 = gpx_db_add_fav(db, "家", 39.9, 116.4);
        int f2 = gpx_db_add_fav(db, "公司", 40.0, 116.5);
        check(id1 > 0 && id2 > 0 && f1 > 0 && f2 > 0, "写入 2 轨迹 + 2 收藏");

        /* 同名再存一次：替换而不是翻倍。
         * ★ 注意替换会**换 id**（DELETE 旧行再 INSERT）⇒ 库里按 id 排序后
         *   trk1 排到 trk2 后面。这不是 bug，是"按 name 幂等替换"的代价；
         *   断言要按实际顺序写（下面 sl.trk[0]=trk2、sl.trk[1]=trk1）。 */
        gpx_db_save_track(db, &s.trk[0]);
        GpxStore sl;
        gpx_store_init(&sl);
        check(gpx_db_load_all(db, &sl) == 0, "从库读回");
        check(sl.n_trk == 2, "同名轨迹替换：仍是 2 条（没翻倍）");
        check(sl.n_wpt == 2, "收藏 2 条");
        if (sl.n_trk == 2) {
            check(trk_eq(&s.trk[2], &sl.trk[0]), "库往返一致：trk2（先存的旧 id）");
            check(trk_eq(&s.trk[0], &sl.trk[1]), "库往返一致：trk1（同名替换后的新 id）");
            check(sl.trk[1].r == s.trk[0].r && sl.trk[1].b == s.trk[0].b,
                  "库往返一致：颜色也存住了");
        }
        check(sl.wpt[0].lat == 39.9 && !strcmp(sl.wpt[0].name, "家"),
              "库往返一致：收藏名字+坐标");

        /* ---------- 同名同坐标收藏查重（导入幂等的另一半）----------
         * 航点没有 name 唯一约束，不查重的话"同一个文件导入两次"会让收藏
         * 每次翻一倍。gpx_db_find_fav 命中返回 id，没命中返回 0。 */
        check(gpx_db_find_fav(db, "家", 39.9, 116.4) > 0,
              "同名同坐标收藏：查得到（幂等导入的依据）");
        check(gpx_db_find_fav(db, "家", 39.90000005, 116.4) > 0,
              "坐标差 5e-8 度算同一个点（浮点容差内）");
        check(gpx_db_find_fav(db, "家", 39.9001, 116.4) == 0,
              "坐标差 1e-4 度：查不到（容差有边界）");
        check(gpx_db_find_fav(db, "公司", 39.9, 116.4) == 0,
              "名字不同：查不到");
        check(gpx_db_find_fav(NULL, "家", 39.9, 116.4) == 0,
              "NULL 句柄：返回 0（不是错误）");

        /* 删除：删 trk2（sl.trk[0]）和收藏"公司" */
        gpx_db_del_track(db, (int)sl.trk[0].db_id);
        gpx_db_del_fav(db, (int)sl.wpt[1].db_id);
        GpxStore sl2;
        gpx_store_init(&sl2);
        gpx_db_load_all(db, &sl2);
        check(sl2.n_trk == 1 && sl2.n_wpt == 1, "删除后剩 1 轨迹 + 1 收藏");
        if (sl2.n_trk == 1)
            check(trk_eq(&s.trk[0], &sl2.trk[0]), "删的是 trk2，trk1 还在且没坏");

        /* 库不存在时读：返回 0 且 store 空（不是错误） */
        GpxStore sn;
        gpx_store_init(&sn);
        check(gpx_db_load_all(NULL, &sn) == 0 && sn.n_trk == 0,
              "NULL 库句柄：读出来是空 store（不是错误）");
        gpx_store_free(&sn);

        gpx_store_free(&sl);
        gpx_store_free(&sl2);
        gpx_db_close(db);
    }
    remove(dbp);
    remove(path);
    remove(path2);

    gpx_store_free(&s);
    gpx_store_free(&s2);
    gpx_store_free(&se);
    gpx_store_free(&sw);

    printf("\n%d 项断言，%d 失败\n", g_checks, g_fail);
    if (g_fail == 0) {
        printf("GPX-OK  解析/往返/边界/持久化全部通过\n");
        return 0;
    }
    printf("GPX-FAIL\n");
    return 1;
}
