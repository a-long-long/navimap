#include "i18n/i18n.h"

#include <string.h>

static I18nLang g_lang = I18N_ZH;   /* 默认中文 */

void i18n_set_lang(I18nLang l)
{
    g_lang = (l == I18N_EN) ? I18N_EN : I18N_ZH;
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
};

static const char *tr_lookup(const char *zh)
{
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
