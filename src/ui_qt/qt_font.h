#ifndef NAVIMAP_QT_FONT_H
#define NAVIMAP_QT_FONT_H

#include <QString>

/* 把 C 侧自带的字体（含 DroidSansFallback = CJK）注册进 Qt 字体库，
 * 并设置应用默认字体为"中英回退链"。
 *
 * 为什么需要：Qt 控件的文字由 Qt 画，走 Qt 的字体系统；地图上的文字由 C 侧
 * stb_truetype 画（见 render/font.c）。两套字体互不相干 ⇒ 没装中文字体的
 * 机器上，Qt 控件的中文是方框、地图上的中文却正常。
 *
 * 返回实际选中的中文字体族名；空串表示没找到（调用方应打提示但不致命）。
 */
QString qt_setup_fonts(int pointSize);

#endif
