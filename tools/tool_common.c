#include "tool_common.h"

#include <string.h>

/* 见 tool_common.h 的说明：这里只放"逐字重复过多遍"的小助手，
 * 不含任何业务逻辑，也不统一各工具语义有差异的部分（如 parse_ll）。 */

const char *tc_arg_val(const char *a, const char *prefix)
{
    size_t n = strlen(prefix);
    return strncmp(a, prefix, n) == 0 ? a + n : NULL;
}

void tc_ab_init(AutoBuildOpt *ab)
{
    memset(ab, 0, sizeof(*ab));
    ab->enable = 1;
}

int tc_load_graph(MapSet *ms, const char *graph_dir, const AutoBuildOpt *ab)
{
    AutoBuildOpt def;
    if (!ab) {
        tc_ab_init(&def);
        ab = &def;
    }
    return mapset_load_repair(ms, graph_dir, ab, NULL);
}
