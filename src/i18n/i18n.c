#include "i18n/i18n.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static I18nLang g_lang = I18N_ZH;   /* 默认中文 */

static void i18n_load(const char *code);  /* 前向声明，定义见文件末尾 */

void i18n_set_lang(I18nLang l)
{
    g_lang = (l == I18N_EN) ? I18N_EN : I18N_ZH;

    /* 释放上一次加载的映射（切换语言时） */
    for (size_t i = 0; i < g_n; i++) {
        free((void *)g_pairs[i].zh);
        free((void *)g_pairs[i].en);
    }
    free(g_pairs);
    g_pairs = NULL;
    g_n = 0;

    if (g_lang == I18N_EN) i18n_load("en");
}

I18nLang i18n_lang(void)
{
    return g_lang;
}

typedef struct { const char *zh; const char *en; } TrPair;

/* 以中文为原文。en 缺失的项不在此表，i18n_tr 会原样返回中文（回退）。 */
static const TrPair TR[] = {
    /* 窗口标题 / 设置键 */
    { "跑路了", "Skedaddle" },
    { "语言",   "Language" },
    { "中文",   "Chinese" },
    { "English", "English" },

    /* 设置面板（SETTINGS_SCHEMA 的 label） */
    { "昼夜模式",   "Day/Night" },
    { "导航面板",   "Nav panel" },
    { "POI 图标",   "POI icons" },
    { "地名标注",   "Place labels" },
    { "等高线",     "Contours" },
    { "网格细分刻度", "Grid subdivision" },
    { "语音播报",   "Voice guidance" },
    { "默认页签",   "Default tab" },
    { "字号(pt)",   "Font size (pt)" },
    { "道路细节",   "Road detail" },
    { "坐标系",     "Datum" },
    { "网格类型",   "Grid type" },

    /* 设置枚举选项（opts 串，逐个翻译） */
    { "白天",       "Day" },
    { "夜间",       "Night" },
    { "自动(日出日落)", "Auto (sun)" },
    { "浏览",       "Browse" },
    { "导航",       "Navigate" },
    { "WGS84",      "WGS84" },
    { "CGCS2000",   "CGCS2000" },
    { "北京1954",   "Beijing 1954" },
    { "西安1980",   "Xian 1980" },
    { "经纬网",     "Lat/Lon grid" },
    { "方里格",     "Gauss-Krüger" },

    /* 坐标系短名 / 标签（datum.c：读数前缀用 short，菜单与提示用 label） */
    { "北京54",     "Beijing 1954" },
    { "西安80",     "Xian 1980" },
    { "WGS84（默认）",       "WGS84 (default)" },
    { "CGCS2000（2000国家大地坐标系）", "CGCS2000 (2000 national datum)" },
    { "北京 1954",  "Beijing 1954" },
    { "西安 1980",  "Xian 1980" },

    /* C 核心自绘设置面板（chrome.c） */
    { "设置",       "Settings" },
    { "关",         "Off" },
    { "开",         "On" },
    { "浮层透明度…", "Overlay opacity…" },
    { "逐个设置",   "Per layer" },
    { "重置全部",   "Reset all" },
    { "关闭",       "Close" },

    /* ☰ 主菜单（buildMenus） */
    { "菜单(&M)",   "Menu(&M)" },
    { "搜索地点",   "Search place" },
    { "清除路线",   "Clear route" },
    { "缩放到路线", "Zoom to route" },
    { "跟随车头",   "Follow heading" },
    { "下载地图…", "Download map…" },
    { "退出",       "Quit" },
    { "模拟导航(&S)", "Simulate(&S)" },
    { "开始 / 停止", "Start / Stop" },
    { "注入偏航（触发重算）", "Inject deviation" },
    { "加速",       "Speed up" },
    { "减速",       "Slow down" },
    { "视图(&V)",   "View(&V)" },
    { "道路细节 LOD %d", "Road detail LOD %d" },
    { "字号增大",   "Larger font" },
    { "字号减小",   "Smaller font" },
    { "坐标系(&C)", "Datum(&C)" },
    { "轨迹(&K)",   "Tracks(&K)" },
    { "导入 GPX…",  "Import GPX…" },
    { "导出全部为 GPX…", "Export all as GPX…" },
    { "收藏视口中心", "Favorite center" },
    { "轨迹 / 收藏面板", "Tracks / Favorites panel" },
    { "设置(&P)",   "Settings(&P)" },
    { "经纬网（度）", "Lat/Lon grid (deg)" },
    { "方里格（公里）", "Gauss-Krüger (km)" },
    { "夜间模式",   "Night mode" },
    { "自动（日出日落）", "Auto (sunrise/sunset)" },

    /* 右栏按钮悬停提示 */
    { "放大",       "Zoom in" },
    { "缩小",       "Zoom out" },
    { "定位到关注点", "Locate to favorites" },
    { "昼 / 夜 切换", "Day / Night toggle" },
    { "坐标网",     "Coordinate grid" },
    { "显示坐标网", "Show grid" },
    { "轨迹 / 收藏（导入 GPX）", "Tracks / Favorites (import GPX)" },
    { "途经点（面板开着时点地图添加）", "Via points (click map when panel open)" },
    { "主菜单",     "Main menu" },
    { "搜索地点或道路（Ctrl+F）", "Search place or road (Ctrl+F)" },

    /* 搜索浮层 */
    { "搜索",       "Search" },
    { "搜索地点、道路…", "Search place, road…" },

    /* 坐标网按钮悬停提示（updateDatumTip） */
    { "坐标网：%1\n", "Grid: %1\n" },
    { "坐标系：%1    网格：%2\n", "Datum: %1    Grid: %2\n" },
    { "左键循环切换 · 右键菜单", "Left-click cycles · right-click menu" },

    /* 浮层透明度对话框（showOverlayAlphaDialog） */
    { "浮层透明度", "Overlay opacity" },
    { "（C 核心）", " (C core)" },
    { "默认",       "Default" },
    { "全部恢复默认（75%）", "Reset all to default (75%)" },

    /* 转向指令面板 */
    { "转向指令",   "Turn-by-turn" },

    /* 轨迹 / 收藏面板 */
    { "轨迹 / 收藏", "Tracks / Favorites" },
    { "导出全部…",  "Export all…" },
    { "收藏中心点",  "Favorite center" },
    { "贴合",       "Snap" },
    { "把这条轨迹吸附到道路网（贴合路线）", "Snap this track to the road network" },
    { "删除这一条",  "Delete this item" },
    { "删除这个途经点", "Delete this via point" },
    { "(未命名轨迹)", "(unnamed track)" },
    { "(未命名)",    "(unnamed)" },
    { "双击定位到这条轨迹的起点", "Double-click to locate this track's start" },
    { "双击定位到这个收藏点", "Double-click to locate this favorite" },
    { "（还没有轨迹：点「导入 GPX…」选一个 .gpx 文件，或用 Ctrl+D 收藏视口中心）",
      "(No tracks yet: click \"Import GPX…\" to pick a .gpx, or Ctrl+D to favorite the viewport center)" },

    /* 途经点面板 */
    { "途经点",     "Via points" },
    { "面板开着时，点地图即可按顺序添加途经点；路线会依次经过它们。",
      "With the panel open, click the map to add via points in order; the route visits them sequentially." },
    { "清空",       "Clear" },
    { "途经点 %1：%2, %3", "Via point %1: %2, %3" },
    { "（还没有途经点：面板开着时点地图即可添加；也可先设好起点和终点）",
      "(No via points yet: click the map with the panel open to add; or set start and end first)" },
    { "全程约 %1 km", "Total ~ %1 km" },

    /* 下载地图对话框（download_dialog.cpp） */
    { "下载地图（全球可下载区域 · Geofabrik）", "Download map (Geofabrik extracts)" },
    { "可下载区域（✓已下载 ·未下载）", "Downloadable regions (✓ downloaded · pending)" },
    { "选择左侧一个地区，查看下载链接并下载。", "Pick a region on the left to see its link and download." },
    { "下载并转换", "Download & convert" },
    { "复制下载链接", "Copy link" },
    { "同时生成 SRTM 等高线", "Also generate SRTM contours" },
    { "请选择最末级地区（带下载链接的）查看详情。", "Select a leaf region (the one with a download link) for details." },
    { "将下载：%1\n保存到：%2/%3-latest.osm.pbf\n（若本地已存在，会先比对远端；有更新则询问是否更新）",
      "Will download: %1\nSaved to: %2/%3-latest.osm.pbf\n(If it already exists locally, the remote is compared first; if newer, you'll be asked to update)" },
    { "发现更新", "Update available" },
    { "本地已有 %1，远端存在更新版本。是否下载更新？\n（选择\"否\"将保留本地文件。）",
      "A newer version of %1 exists on the server. Download the update?\n(Choosing \"No\" keeps the local file.)" },

    /* main_window.cpp 后续补充 */
    { "%1  (%2 点)", "%1  (%2 pts)" },
    { "导入 GPX 文件", "Import GPX file" },
    { "GPX 轨迹", "GPX tracks" },
    { "所有文件", "All files" },
    { "导出全部为 GPX", "Export all as GPX" },

    /* 下载地图对话框：状态 / 进度提示 */
    { "  ✓已下载", "  ✓ downloaded" },
    { "  ·未下载", "  · pending" },
    { "  (%1/%2)", "  (%1/%2 downloaded)" },
    { "本地已存在 %1。正在比对远端是否有更新…",
      "Already exists locally: %1. Comparing with remote for updates…" },
    { "开始下载：%1\n  → %2", "Start download: %1\n  → %2" },
    { "无法创建目标文件：%1", "Cannot create target file: %1" },
    { "无法比对远端（%1）。为稳妥起见保留本地文件，跳过下载。\n如需强制刷新，请先删除本地文件后重试。",
      "Cannot compare with remote (%1). Keeping local file to be safe, skipping download.\nTo force refresh, delete the local file and retry." },
    { "远端无更新（本地已是最新），跳过下载。", "Remote has no update (local is latest), skipping download." },
    { "用户选择更新，开始下载…", "User chose to update, starting download…" },
    { "用户选择保留本地文件，跳过下载。", "User chose to keep local file, skipping download." },
    { "下载失败：%1", "Download failed: %1" },
    { "下载完成（%1 字节），开始转换…", "Download complete (%1 bytes), starting conversion…" },
    { "运行 sync_maps.sh（转换 + 建地名库 + 地标表）…",
      "Running sync_maps.sh (convert + build place DB + landmark table)…" },
    { "转换完成 ✓ 地图已就绪。", "Conversion complete ✓ map ready." },
    { "转换失败（exit %1）。可在终端手动运行 scripts/sync_maps.sh 查看详情。",
      "Conversion failed (exit %1). Run scripts/sync_maps.sh in terminal for details." },

    /* 下载树：中国省级（主战场）+ 大洲（其它国家名缺译时回退中文） */
    { "中国（全国）", "China (whole country)" },
    { "中国·省级", "China · provinces" },
    { "北京", "Beijing" }, { "天津", "Tianjin" }, { "河北", "Hebei" },
    { "山西", "Shanxi" }, { "内蒙古", "Inner Mongolia" }, { "辽宁", "Liaoning" },
    { "吉林", "Jilin" }, { "黑龙江", "Heilongjiang" }, { "上海", "Shanghai" },
    { "江苏", "Jiangsu" }, { "浙江", "Zhejiang" }, { "安徽", "Anhui" },
    { "福建", "Fujian" }, { "江西", "Jiangxi" }, { "山东", "Shandong" },
    { "河南", "Henan" }, { "湖北", "Hubei" }, { "湖南", "Hunan" },
    { "广东", "Guangdong" }, { "广西", "Guangxi" }, { "海南", "Hainan" },
    { "重庆", "Chongqing" }, { "四川", "Sichuan" }, { "贵州", "Guizhou" },
    { "云南", "Yunnan" }, { "西藏", "Tibet" }, { "陕西", "Shaanxi" },
    { "甘肃", "Gansu" }, { "青海", "Qinghai" }, { "宁夏", "Ningxia" },
    { "新疆", "Xinjiang" }, { "香港", "Hong Kong" }, { "澳门", "Macau" },
    { "台湾", "Taiwan" },
    { "亚洲", "Asia" }, { "欧洲", "Europe" }, { "非洲", "Africa" },
    { "北美洲", "North America" }, { "南美洲", "South America" }, { "大洋洲", "Oceania" },
    /* 世界各国 / 地区名：英文从 Geofabrik path 反推 */
    { "不丹", "Bhutan" },
    { "东帝汶", "Timor-Leste" },
    { "中非", "Central African Republic" },
    { "丹麦", "Denmark" },
    { "乌克兰", "Ukraine" },
    { "乌兹别克斯坦", "Uzbekistan" },
    { "乌干达", "Uganda" },
    { "乌拉圭", "Uruguay" },
    { "乍得", "Chad" },
    { "也门", "Yemen" },
    { "亚美尼亚", "Armenia" },
    { "以色列", "Israel" },
    { "伊拉克", "Iraq" },
    { "伊朗", "Iran" },
    { "俄罗斯（欧洲部分）", "Russia (European part)" },
    { "保加利亚", "Bulgaria" },
    { "克罗地亚", "Croatia" },
    { "冈比亚", "Gambia" },
    { "冰岛", "Iceland" },
    { "几内亚", "Guinea" },
    { "列支敦士登", "Liechtenstein" },
    { "刚果（布）", "Congo" },
    { "刚果（金）", "Democratic Republic of the Congo" },
    { "利比亚", "Libya" },
    { "利比里亚", "Liberia" },
    { "加拿大", "Canada" },
    { "加纳", "Ghana" },
    { "加蓬", "Gabon" },
    { "匈牙利", "Hungary" },
    { "北马其顿", "North Macedonia" },
    { "南苏丹", "South Sudan" },
    { "南非", "South Africa" },
    { "博茨瓦纳", "Botswana" },
    { "卡塔尔", "Qatar" },
    { "卢旺达", "Rwanda" },
    { "卢森堡", "Luxembourg" },
    { "印度", "India" },
    { "印度尼西亚", "Indonesia" },
    { "厄瓜多尔", "Ecuador" },
    { "厄立特里亚", "Eritrea" },
    { "叙利亚", "Syria" },
    { "吉尔吉斯斯坦", "Kyrgyzstan" },
    { "吉布提", "Djibouti" },
    { "哈萨克斯坦", "Kazakhstan" },
    { "哥伦比亚", "Colombia" },
    { "喀麦隆", "Cameroon" },
    { "土库曼斯坦", "Turkmenistan" },
    { "土耳其", "Turkey" },
    { "圣马力诺", "San Marino" },
    { "圭亚那", "Guyana" },
    { "坦桑尼亚", "Tanzania" },
    { "埃及", "Egypt" },
    { "埃塞俄比亚", "Ethiopia" },
    { "塔吉克斯坦", "Tajikistan" },
    { "塞内加尔", "Senegal" },
    { "塞尔维亚", "Serbia" },
    { "塞拉利昂", "Sierra Leone" },
    { "塞浦路斯", "Cyprus" },
    { "墨西哥", "Mexico" },
    { "多哥", "Togo" },
    { "奥地利", "Austria" },
    { "委内瑞拉", "Venezuela" },
    { "孟加拉国", "Bangladesh" },
    { "安哥拉", "Angola" },
    { "安道尔", "Andorra" },
    { "尼日利亚", "Nigeria" },
    { "尼日尔", "Niger" },
    { "尼泊尔", "Nepal" },
    { "巴勒斯坦", "Palestine" },
    { "巴基斯坦", "Pakistan" },
    { "巴布亚新几内亚", "Papua New Guinea" },
    { "巴拉圭", "Paraguay" },
    { "巴林", "Bahrain" },
    { "巴西", "Brazil" },
    { "布隆迪", "Burundi" },
    { "希腊", "Greece" },
    { "德国", "Germany" },
    { "意大利", "Italy" },
    { "拉脱维亚", "Latvia" },
    { "挪威", "Norway" },
    { "捷克", "Czech Republic" },
    { "摩尔多瓦", "Moldova" },
    { "摩洛哥", "Morocco" },
    { "摩纳哥", "Monaco" },
    { "文莱", "Brunei" },
    { "斐济", "Fiji" },
    { "斯洛伐克", "Slovakia" },
    { "斯洛文尼亚", "Slovenia" },
    { "斯里兰卡", "Sri Lanka" },
    { "新加坡", "Singapore" },
    { "新西兰", "New Zealand" },
    { "日本", "Japan" },
    { "智利", "Chile" },
    { "朝鲜", "North Korea" },
    { "柬埔寨", "Cambodia" },
    { "格陵兰", "Greenland" },
    { "格鲁吉亚", "Georgia" },
    { "比利时", "Belgium" },
    { "毛里塔尼亚", "Mauritania" },
    { "沙特阿拉伯", "Saudi Arabia" },
    { "法国", "France" },
    { "波兰", "Poland" },
    { "波黑", "Bosnia and Herzegovina" },
    { "泰国", "Thailand" },
    { "津巴布韦", "Zimbabwe" },
    { "澳大利亚", "Australia" },
    { "爱尔兰", "Ireland" },
    { "爱沙尼亚", "Estonia" },
    { "玻利维亚", "Bolivia" },
    { "瑞典", "Sweden" },
    { "瑞士", "Switzerland" },
    { "白俄罗斯", "Belarus" },
    { "百慕大", "Bermuda" },
    { "科威特", "Kuwait" },
    { "科特迪瓦", "Ivory Coast" },
    { "科索沃", "Kosovo" },
    { "秘鲁", "Peru" },
    { "突尼斯", "Tunisia" },
    { "立陶宛", "Lithuania" },
    { "索马里", "Somalia" },
    { "约旦", "Jordan" },
    { "纳米比亚", "Namibia" },
    { "缅甸", "Myanmar" },
    { "罗马尼亚", "Romania" },
    { "美国（本土）", "United States (mainland)" },
    { "老挝", "Laos" },
    { "肯尼亚", "Kenya" },
    { "芬兰", "Finland" },
    { "苏丹", "Sudan" },
    { "苏里南", "Suriname" },
    { "英国", "United Kingdom" },
    { "荷兰", "Netherlands" },
    { "莫桑比克", "Mozambique" },
    { "菲律宾", "Philippines" },
    { "葡萄牙", "Portugal" },
    { "蒙古", "Mongolia" },
    { "西班牙", "Spain" },
    { "贝宁", "Benin" },
    { "赞比亚", "Zambia" },
    { "越南", "Vietnam" },
    { "阿塞拜疆", "Azerbaijan" },
    { "阿富汗", "Afghanistan" },
    { "阿尔及利亚", "Algeria" },
    { "阿尔巴尼亚", "Albania" },
    { "阿曼", "Oman" },
    { "阿根廷", "Argentina" },
    { "阿联酋", "United Arab Emirates" },
    { "韩国", "South Korea" },
    { "马拉维", "Malawi" },
    { "马来西亚", "Malaysia" },
    { "马耳他", "Malta" },
    { "马达加斯加", "Madagascar" },
    { "马里", "Mali" },
    { "黎巴嫩", "Lebanon" },
    { "黑山", "Montenegro" },
    /* 下载对话框补充文案：短版 key 与等高线日志 */
    { "  [等高线] %1", "  [Contours] %1" },
    { "  [等高线] PBF 文件不存在，跳过生成。", "  [Contours] PBF file not found, skipping generation." },
    { "  [等高线] skedaddle_gen_contours 工具未编译，跳过生成。", "  [Contours] skedaddle_gen_contours tool not built, skipping generation." },
    { "  [等高线] skedaddle_pbf_info 工具未编译，跳过生成。", "  [Contours] skedaddle_pbf_info tool not built, skipping generation." },
    { "  [等高线] skedaddle_pbf_info 运行超时，跳过生成。", "  [Contours] skedaddle_pbf_info timed out, skipping generation." },
    { "  [等高线] 区域内 %1 张 SRTM 瓦片已全部存在，仅生成等高线。", "  [Contours] All %1 SRTM tiles in region already exist; generating contours only." },
    { "  [等高线] 区域内 %1/%2 张 SRTM 瓦片已存在，自动下载缺失的瓦片。", "  [Contours] %1/%2 SRTM tiles in region exist; auto-downloading missing ones." },
    { "  [等高线] 区域范围：%1,%2 – %3,%4", "  [Contours] Region bounds: %1,%2 – %3,%4" },
    { "  [等高线] 无法从 PBF 提取 bbox，跳过生成。", "  [Contours] Cannot extract bbox from PBF, skipping generation." },
    { "  [等高线] 无法创建 SRTM 缓存目录，跳过生成。", "  [Contours] Cannot create SRTM cache directory, skipping generation." },
    { "  [等高线] 检测到已存在 %1，跳过生成。", "  [Contours] Detected existing %1, skipping generation." },
    { "  [等高线] 生成失败（exit %1）。可在命令行手动运行：%2 %3", "  [Contours] Generation failed (exit %1). Run manually on CLI: %2 %3" },
    { "  [等高线] 生成完成 ✓ 地图目录已包含等高线数据。", "  [Contours] Generation complete ✓ map directory now contains contour data." },
    { "  [等高线] 生成超时（2 分钟），已强制停止。", "  [Contours] Generation timed out (2 min), force-stopped." },
    { "  [等高线]⚠ %1", "  [Contours]⚠ %1" },
    { "将下载：%1\n保存到：%2/%3-latest.osm.pbf", "Will download: %1\nSaved to: %2/%3-latest.osm.pbf" },
    { "搜索地点或道路（Ctrl+F）\n最近选择：", "Search place or road (Ctrl+F)\nRecent:" },
    { "无法比对远端（%1）。为稳妥起见保留本地文件，跳过下载。", "Cannot compare with remote (%1). Keeping local file to be safe, skipping download." },
    { "（还没有途经点：面板开着时点地图添加；也可先设好起点和终点）", "(No via points yet: click the map with the panel open to add; or set start and end first)" },

};

/* ---------------- 运行时语言文件（XML） ----------------
 * 每种目标语言一个独立文件（lang/<code>.xml），由 Qt 侧通过
 * i18n_set_lang_dir() 传入目录。文件优先；文件缺失/损坏时回退下面的
 * 内置 TR[]（与文件同源，由 scripts/export_lang.py 从 TR[] 生成）。
 * 中文是源语言（msgid），不生成文件。 */

static TrPair *g_pairs = NULL;   /* 从 XML 加载的该语言映射 */
static size_t  g_n = 0;
static char   *g_lang_dir = NULL;

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

void i18n_set_lang_dir(const char *dir)
{
    free(g_lang_dir);
    g_lang_dir = dir ? xstrdup(dir) : NULL;
}

/* 极简 XML 文本解码（只处理我们生成的格式）：
 *   - 实体 &amp; &lt; &gt; &quot; &#NNN; &#xHH; 还原
 *   - 字面 \n \t \\ \" 还原（兼容手改 xml 时使用字面转义）
 * 写入 dst（容量 dstcap），返回解码长度。 */
static size_t xml_decode(const char *src, size_t srclen, char *dst, size_t dstcap)
{
    size_t o = 0;
    for (size_t i = 0; i < srclen; i++) {
        char c = src[i];
        if (c == '&') {
            size_t j = i + 1;
            while (j < srclen && src[j] != ';') j++;
            if (j < srclen) {
                const char *e = src + i + 1;
                size_t elen = j - i - 1;
                char outc = 0;
                if      (elen == 3 && strncmp(e, "amp", 3) == 0)  outc = '&';
                else if (elen == 2 && strncmp(e, "lt", 2) == 0)   outc = '<';
                else if (elen == 2 && strncmp(e, "gt", 2) == 0)   outc = '>';
                else if (elen == 4 && strncmp(e, "quot", 4) == 0) outc = '"';
                else if (e[0] == '#') {
                    int code = 0;
                    if (e[1] == 'x' || e[1] == 'X')
                        for (size_t k = 2; k < elen; k++)
                            code = code * 16 + (e[k] >= '0' && e[k] <= '9' ? e[k]-'0'
                                  : e[k] >= 'a' && e[k] <= 'f' ? e[k]-'a'+10
                                  : e[k] >= 'A' && e[k] <= 'F' ? e[k]-'A'+10 : 0);
                    else
                        for (size_t k = 1; k < elen; k++)
                            code = code * 10 + (e[k] >= '0' && e[k] <= '9' ? e[k]-'0' : 0);
                    outc = (char)(code & 0xFF);
                }
                if (outc) { if (o + 1 < dstcap) dst[o++] = outc; i = j; continue; }
                i = j; continue;   /* 未知实体：跳过 */
            }
            if (o + 1 < dstcap) dst[o++] = '&';
            continue;
        }
        if (c == '\\' && i + 1 < srclen) {
            char n = src[i + 1];
            char outc = 0;
            if      (n == 'n') outc = '\n';
            else if (n == 't') outc = '\t';
            else if (n == '\\') outc = '\\';
            else if (n == '"') outc = '"';
            if (outc) { if (o + 1 < dstcap) dst[o++] = outc; i++; continue; }
        }
        if (o + 1 < dstcap) dst[o++] = c;
    }
    if (o < dstcap) dst[o] = 0;
    return o;
}

/* 在 [start,end) 内找下一个 <tag ...>...</tag>，返回内容起点；
 * *content_end 设为 </tag> 起点。找不到返回 NULL。 */
static const char *find_tag(const char *start, const char *end,
                            const char *tag, const char **content_end)
{
    size_t tlen = strlen(tag);
    for (const char *p = start; p + 1 + tlen < end; p++) {
        if (p[0] == '<' && strncmp(p + 1, tag, tlen) == 0 &&
            (p[1 + tlen] == ' ' || p[1 + tlen] == '>' || p[1 + tlen] == '/')) {
            const char *q = p + 1 + tlen;
            while (q < end && *q != '>') q++;
            if (q >= end) return NULL;
            const char *cstart = q + 1;
            char closebuf[64];
            int cn = snprintf(closebuf, sizeof closebuf, "</%s>", tag);
            const char *cpos = cstart;
            while (cpos + cn <= end) {           /* bounded strstr */
                if (memcmp(cpos, closebuf, (size_t)cn) == 0) {
                    *content_end = cpos;
                    return cstart;
                }
                cpos++;
            }
            return NULL;
        }
    }
    return NULL;
}

/* 提取 <tag> 文本并解码（首尾空白已 trim，内部真实换行保留）。 */
static char *extract_tag_text(const char *start, const char *end, const char *tag)
{
    const char *ce;
    const char *cs = find_tag(start, end, tag, &ce);
    if (!cs) return NULL;
    while (ce > cs && (ce[-1] == ' ' || ce[-1] == '\t' ||
                       ce[-1] == '\n' || ce[-1] == '\r')) ce--;
    while (cs < ce && (*cs == ' ' || *cs == '\t' ||
                       *cs == '\n' || *cs == '\r')) cs++;
    size_t len = (size_t)(ce - cs);
    char *out = (char *)malloc(len + 1);
    if (!out) return NULL;
    xml_decode(cs, len, out, len + 1);
    return out;
}

/* 加载 lang/<code>.xml 到 g_pairs。失败（文件缺失/损坏）则保持
 * g_pairs 为空，调用方回退到内置 TR[]。 */
static void i18n_load(const char *code)
{
    const char *cands[4];
    char b1[2048]; char b2[2048]; char b3[2048];
    int nc = 0;
    if (g_lang_dir && *g_lang_dir) {
        snprintf(b1, sizeof b1, "%s/%s.xml", g_lang_dir, code);
        cands[nc++] = b1;
    }
    snprintf(b2, sizeof b2, "lang/%s.xml", code); cands[nc++] = b2;
    snprintf(b3, sizeof b3, "%s.xml", code);      cands[nc++] = b3;

    FILE *f = NULL;
    for (int i = 0; i < nc; i++) {
        f = fopen(cands[i], "rb");
        if (f) break;
    }
    if (!f) return;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return; }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    buf[rd] = 0;
    fclose(f);

    const char *p = buf;
    const char *end = buf + rd;
    while (p < end) {
        const char *ce;
        const char *cs = find_tag(p, end, "item", &ce);
        if (!cs) break;
        char *zh = extract_tag_text(cs, ce, "zh");
        char *en = extract_tag_text(cs, ce, "en");
        if (zh && en) {
            TrPair *tmp = (TrPair *)realloc(g_pairs, (g_n + 1) * sizeof(TrPair));
            if (tmp) {
                g_pairs = tmp;
                g_pairs[g_n].zh = zh;
                g_pairs[g_n].en = en;
                g_n++;
            } else {
                free(zh); free(en);
            }
        } else {
            free(zh); free(en);
        }
        p = ce + strlen("</item>");
    }
    free(buf);
}

static const char *tr_lookup(const char *zh)
{
    for (size_t i = 0; i < g_n; i++)
        if (strcmp(g_pairs[i].zh, zh) == 0) return g_pairs[i].en;
    for (size_t i = 0; i < sizeof(TR) / sizeof(TR[0]); i++)
        if (strcmp(TR[i].zh, zh) == 0) return TR[i].en;
    return zh;   /* 缺译：回退中文原文 */
}

const char *i18n_tr(const char *zh)
{
    if (!zh) return zh;
    if (g_lang == I18N_EN) return tr_lookup(zh);
    return zh;   /* 中文模式：原文返回 */
}
