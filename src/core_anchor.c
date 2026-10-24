/* navimap_core 聚合层的占位目标（ARCH_ASSESSMENT.md 阶段 2）。
 *
 * 阶段 2 把业务拆成 navmap_map / route / search / render / ui 五个独立静态库之后，
 * navimap_core 不再含任何业务 .c，只作为"链接聚合层"把五个模块库 + 五个叶子库 +
 * 第三方库（zlib/sqlite3/protobuf-c/libm 或 psapi）聚合成一个对外链接面，使 24 个
 * exe / navmap_sdl / navmap_qt 等无需改动即可继续链接 navimap_core。
 *
 * 为避免空静态库告警、并保留 build/libnavimap_core.a 供架构门禁 core_no_sdl 扫描，
 * 这里放一个无副作用的锚点符号。该符号不参与任何业务逻辑，也不引用 SDL。
 */
int navimap_core_anchor(void)
{
    return 0;
}
