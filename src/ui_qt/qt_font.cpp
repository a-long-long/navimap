/* qt_font.cpp —— Qt 前端的中文字体接入。
 *
 * 问题：Qt 前端的控件文字（顶栏/菜单/右栏按钮/搜索框占位符）全部由 Qt 自己
 * 绘制，走的是 Qt 的字体系统。**地图上的文字不是**——那是 C 渲染层用
 * stb_truetype 画的（见 render/font.c），两套字体互不相干。
 *
 * 现象：在没有中文字体的环境（Linux 容器、Qt offscreen 平台插件、CI 机器）
 * 里，Qt 控件的中文全是方框（tofu），而地图上的中文正常 —— 因为 C 侧自带
 * DroidSansFallback.ttf，Qt 侧什么都没有。
 *
 * 解法：**把 C 侧自带的字体注册进 Qt 的字体数据库**，让两套用同一批字体。
 *   ① 遍历 nm_font_candidates()（含内置 Noto 系列 + DroidSansFallback + 系统字体），
 *      用 QFontDatabase::addApplicationFont() 注册；
 *   ② 挑出**确实含中文字形**的族作为应用默认字体；
 *   ③ 用 QFont::setFamilies() 同时列出中英族，让 Qt 自己做 per-glyph 回退
 *      （Qt6 的 setFamilies 就是为这个设计的：拉丁字走 Noto，中文走 Fallback）。
 *
 * ★★ 判"这个字体能不能画中文"为什么不用 Qt 的字体度量（踩过的坑）：
 *   最直觉的做法是量"中"字的宽度，跟一个没有中文的参照字体比宽度，不同就
 *   说明有字形。**但这在 Qt offscreen 平台插件下不可靠** —— 那个插件不走
 *   真实字体引擎，所有族的宽度都一样，于是**每一个族都判成"没有中文"**
 *   （实测：Noto Sans / Droid Sans Fallback / 微软雅黑全部判否）。
 *   另一个坑是 QFontInfo::family() 返回的是 Qt **实际替换后**的族名，
 *   与请求的名字未必一致，不能拿它判"族是否存在"。
 *
 *   ⇒ 可靠做法：**直接查字体文件本身**。C 侧已有 stb_truetype 的字形查询
 *   （font.h 的 font_probe_glyph），直接问"这个文件的 cmap 里有没有'中'"，
 *   答案与 Qt 无关、跨平台一致。
 */
#include "qt_font.h"

#include <QApplication>
#include <QFont>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFontMetrics>
#include <QString>
#include <QStringList>

#include <cstdio>

extern "C" {
#include "render/font.h"     /* font_open / font_probe_glyph / font_close */
#include "utils/plat.h"      /* nm_font_candidates */
}

/* 这个字体**文件**里有没有"中"字形？直接查 cmap，不依赖 Qt 的度量。 */
static bool file_has_cjk(const char *path)
{
    Font *f = font_open(path, 14);
    if (!f) return false;
    int face = -1, gi = -1, w = 0, h = 0, nz = 0;
    bool ok = false;
    if (font_probe_glyph(f, 0x4E2D /* 中 */, &face, &gi, &w, &h, &nz)) {
        /* gi != 0 ⇒ cmap 里有这个码点；nz > 0 ⇒ 光栅化出来确实有像素 */
        ok = (gi != 0) && (nz > 0);
    }
    font_close(f);
    return ok;
}

QString qt_setup_fonts(int pointSize)
{
    size_t n = 0;
    const char *const *cands = nm_font_candidates(&n);

    /* ① 注册所有候选字体进 Qt 字体库，并记录"文件 → 族名"的对应关系。
     *    ★ 判定"能否画中文"要按**文件**来，不能只按族名：同一个文件可能
     *    注册出多个族名，而系统里同名的族可能来自别的文件。 */
    QStringList cjkFamilies;     /* 能画中文的族（按注册顺序） */
    QStringList allFamilies;     /* 所有注册成功的族 */
    for (size_t i = 0; i < n; i++) {
        if (!cands[i]) continue;
        int id = QFontDatabase::addApplicationFont(QString::fromUtf8(cands[i]));
        if (id < 0) continue;
        QStringList fams = QFontDatabase::applicationFontFamilies(id);
        bool cjk = file_has_cjk(cands[i]);
        for (const QString &fam : fams) {
            if (!allFamilies.contains(fam)) allFamilies.append(fam);
            if (cjk && !cjkFamilies.contains(fam)) cjkFamilies.append(fam);
            fprintf(stderr, "[qt-font]   文件 %-30s → 族 %-24s 含中文: %s\n",
                    cands[i], fam.toUtf8().constData(), cjk ? "是" : "否");
        }
    }

    /* ② 系统里常见的中文族也算上（若自带字体没注册成功）。
     *    这些是**系统字体**，Qt 本来就认识；但仍要验证它们确实能画中文
     *    （有些系统的"宋体"是精简版，只有部分汉字）。 */
    const char *kSysCjk[] = {
        "Microsoft YaHei", "SimSun", "SimHei", "KaiTi", "FangSong",
        "Noto Sans CJK SC", "Noto Sans CJK TC", "Source Han Sans SC",
        "WenQuanYi Micro Hei", "Droid Sans Fallback", "PingFang SC",
    };
    /* Qt6 起 families() 是静态成员；Qt5 需经 QFontDatabase 实例调用。
     * 用临时实例统一两种写法（静态成员经实例调用在 C++ 合法）。 */
    QStringList sysFams = QFontDatabase().families();
    for (const char *name : kSysCjk) {
        QString fam = QString::fromUtf8(name);
        if (!sysFams.contains(fam)) continue;
        if (!allFamilies.contains(fam)) allFamilies.append(fam);
        if (!cjkFamilies.contains(fam)) cjkFamilies.append(fam);
    }

    if (cjkFamilies.isEmpty()) {
        /* 用 fprintf 而不是 qWarning：release 构建（QT_NO_DEBUG_OUTPUT）下
         * qWarning/qInfo 被编译掉，诊断信息就彻底看不见了 —— 而"没找到中文字体"
         * 恰恰是最需要被看到的情况。 */
        fprintf(stderr,
                "[qt-font] 警告：没有可用中文字体，Qt 控件的中文会显示为方框"
                "（地图上的文字不受影响：那是 C 侧 stb_truetype 画的）\n");
        return QString();
    }

    /* ③ 应用字体：setFamilies 给出回退链，Qt 按字形自动挑。
     *    顺序：先中文字体（覆盖 CJK），再拉丁字体（覆盖 ASCII/符号）。 */
    QStringList chain = cjkFamilies;
    for (const QString &fam : allFamilies) {
        if (!chain.contains(fam)) chain.append(fam);
    }
    QFont app_font = QApplication::font();
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    app_font.setFamilies(chain);
#else
    /* Qt5 无 setFamilies：把首个中文字族设为主族，Qt 自己按字形回退到其他
     * 已注册字体（拉丁字走其余族）。回退链由 Qt 字体引擎自动完成。 */
    app_font.setFamily(chain.first());
#endif
    app_font.setPointSize(pointSize > 0 ? pointSize : app_font.pointSize());
    QApplication::setFont(app_font);

    fprintf(stderr, "[qt-font] 中文字体: %s（候选 %d 个文件，可用中文字族 %d）\n",
            cjkFamilies.first().toUtf8().constData(), (int)n, (int)cjkFamilies.size());
    return cjkFamilies.first();
}
