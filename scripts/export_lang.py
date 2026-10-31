#!/usr/bin/env python3
"""从 src/i18n/i18n.c 的 TR[] 导出每种语言的独立 XML 文件。

用法：
    python3 scripts/export_lang.py

读取 i18n.c 里 `static const TrPair TR[] = { ... };` 的全部 { "zh", "en" } 对，
写出 lang/en.xml（简化 TS 风格）：

    <?xml version="1.0" encoding="UTF-8"?>
    <translations lang="en">
      <item><zh>跑路了</zh><en>Skedaddle</en></item>
      <item>
        <zh>将下载：%1
    保存到：%2/%3-latest.osm.pbf</zh>
        <en>Will download: %1
    Saved to: %2/%3-latest.osm.pbf</en>
      </item>
    </translations>

约定：
    - 中文是源语言（msgid），不生成文件，内联在代码里。
    - 每种目标语言一个文件：lang/<code>.xml（当前只有 en）。
    - 该文件是「主数据源」；i18n.c 里的 TR[] 保留为兜底（文件缺失/损坏时仍可用）。
    - 含换行的串：i18n.c 源码里是字面 \\n，导出时还原为 XML 里的真实换行
      （解析时原样保留以匹配调用方 key）。
    - 文本里的 & < > 转义为 &amp; &lt; &gt;（解析器会还原）。
    - 重新生成会覆盖 lang/en.xml，因此**只应在翻译源(i18n.c)变动后**运行；
      若你手改了 lang/en.xml 想保留，请勿运行本脚本（或先备份）。
"""
import re
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src", "i18n", "i18n.c")
OUT_DIR = os.path.join(ROOT, "lang")
OUT = os.path.join(OUT_DIR, "en.xml")


def extract_pairs(path):
    txt = open(path, encoding="utf-8").read()
    start = txt.index("static const TrPair TR[] = {")
    end = txt.index("};", start)
    block = txt[start:end]
    pairs = re.findall(r'\{\s*"((?:\\.|[^"\\])*)"\s*,\s*"((?:\\.|[^"\\])*)"\s*\}',
                       block, re.S)
    return pairs


def xml_esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def main():
    pairs = extract_pairs(SRC)
    seen = {}
    for zh, en in pairs:
        seen[zh] = en
    os.makedirs(OUT_DIR, exist_ok=True)
    with open(OUT, "w", encoding="utf-8") as f:
        f.write('<?xml version="1.0" encoding="UTF-8"?>\n')
        f.write('<!-- Skedaddle / 跑路了 英文语言包 (lang/en.xml) -->\n')
        f.write('<!-- 由 scripts/export_lang.py 从 src/i18n/i18n.c 的 TR[] 自动导出。 -->\n')
        f.write('<!-- 手改本文件会在运行时覆盖内置翻译；勿运行导出脚本以免覆盖你的修改。 -->\n')
        f.write('<translations lang="en">\n')
        for zh, en in seen.items():
            # i18n.c 源码里是字面 \n，这里还原为真实换行（XML 可读多行）
            zh2 = zh.replace("\\n", "\n").replace("\\t", "\t")
            en2 = en.replace("\\n", "\n").replace("\\t", "\t")
            if "\n" in zh2 or "\n" in en2:
                f.write("  <item>\n")
                f.write(f"    <zh>{xml_esc(zh2)}</zh>\n")
                f.write(f"    <en>{xml_esc(en2)}</en>\n")
                f.write("  </item>\n")
            else:
                f.write(f'  <item><zh>{xml_esc(zh2)}</zh><en>{xml_esc(en2)}</en></item>\n')
        f.write("</translations>\n")
    print(f"written {len(seen)} entries -> {OUT}")


if __name__ == "__main__":
    main()
