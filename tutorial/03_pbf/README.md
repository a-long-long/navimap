# 第 3 课 · PBF 流式解析（精髓复刻）

> 📖 配套读源码讲义：[`docs/teaching/03_pbf.md`](../../docs/teaching/03_pbf.md)（读源码式讲解，讲清楚“为什么这么写”；与本科小程序同号对应）


**累加内容**：PBF 最经典的三个坑——① varint(LEB128) 变长整数；② DenseNodes 的 delta 编码；
③ stringtable 不以 `\0` 结尾。本课不引入 protobuf-c，用几十行把三点本质复刻出来，亲手 decode
一段 DenseNodes 并画出来。

**运行**：
```bash
gcc -std=c11 -O2 -o tut main.c && ./tut
```

**你会看到**：stdout 打出 4 个节点的解码结果（id/lat/lon 由 delta 还原），以及从 stringtable
按长度安全取键 `"highway"`；`out.bmp` 是这 4 个节点连成的 way。

**三个关键教训（项目里也是这么踩的）**：
1. **varint 变长**：小整数只占 1 字节，省空间也省 IO——这是 PBF 体量能压到 `.osm.pbf` 的原因。
2. **delta 编码**：`DenseNodes` 的 id/lat/lon 只存相邻差（大多是小整数），必须**累加还原**，
   不能当成绝对值。
3. **⚠️ stringtable 不是 NUL 结尾**：protobuf-c 的 `ProtobufCBinaryData` 不保证 `\0`。直接当 C 串，
   `strcmp` 会把 `"highway"` 读作 `"highway\x7f"`（吃掉下一个字段），导致 `highway=*` 整类落空。
   项目 `pbf_unpack_block` 因此把所有 stringtable 拷成 NUL 结尾副本——这就是"序列化数据 ≠ C 字符串"。

**生产最终形态**：`src/pbf/pbf_stream.c`（块结构 4 字节大端长度 + Blob + 解压）、
`src/pbf/pbf_block.c`（`pbf_str` 返回 NUL 结尾 C 串、`pbf_unpack_block` 安全解包）。

**练习**：把 `nodes[4]` 再加一个节点，重跑，确认 delta 解码仍能还原；再试把 `blob2` 直接当 `%s`
打印（注释打开），观察越界读到什么。
