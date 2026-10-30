# 第 9 课 · 地名搜索（bigram 倒排索引）

**累加内容**：用 bigram 倒排索引替代 `LIKE '%x%'` 全表扫描。本课把中文名按 **codepoint**
（不是字节）切成字 bigram，每个 bigram → 倒排 posting(place_id)；查询时取各 bigram posting 的
交集，再用【子串复核】过滤掉"bigram 全中但不连续"的误命中。

**运行**：
```bash
gcc -std=c11 -O2 -o tut main.c && ./tut
```

**你会看到**：查询 `中关村` 命中 `中关村`/`中关村南`/`中关村大街`（红点），未命中 `人民大学` 等（灰点）；
stdout 打印每个候选的 bigram 命中数与子串复核结果。

**关键概念（项目里一致）**：
1. **bigram 切分按"字符"不是"字节"**：中文是 UTF-8 多字节，必须按 codepoint 切（本课 `utf8_next`）。
   切成 `中关`/`关村` → 查 `gram` 表取交集 `place_id` → 逐条取详情。
2. **precision/recall 权衡**：子串复核这步必须的——否则会召回"中"和"关村"恰好相邻但非连续串的误命中。
3. **数据来源**：真实搜索直接吃 `places_auto.db`（SQLite），用 bigram 倒排索引。

**生产最终形态**：`src/search/search.c`（`SearchHit`/`search_open`/`search_query`/`search_bigrams`）。

**练习**：把查询改成 `大学`，看 `人民大学`/`北京大学` 如何被 bigram + 子串复核共同召回。
