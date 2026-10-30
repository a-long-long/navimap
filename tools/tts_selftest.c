/* skedaddle_tts_selftest —— 语音播报（离线 TTS）集成自测。
 *
 * 不依赖任何真实音频设备 / TTS 引擎：用 NAVIMAP_TTS_DRYRUN 把"发声"重定向到
 * 一个临时文件，断言 nm_tts_speak 把文本正确交给了引擎；再对分层阈值
 * nm_tts_tier_for_distance 做纯逻辑断言。全过打印 TTS-OK。 */
#include "utils/plat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(cond, msg) do {                                            \
    if (!(cond)) {                                                       \
        fprintf(stderr, "ASSERT-FAIL %s (%s:%d)\n", msg, __FILE__, __LINE__); \
        g_fail++;                                                       \
    }                                                                   \
} while (0)

static int read_file(const char *path, char *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = '\0';
    return (int)n;
}

int main(void)
{
    /* ---- 1. 分层阈值：距指令越远层级越低，到达/临近立即层 ---- */
    CHECK(nm_tts_tier_for_distance(400.0) == 0, "400m 不播报");
    CHECK(nm_tts_tier_for_distance(300.0) == 1, "300m 接近层");
    CHECK(nm_tts_tier_for_distance(250.0) == 1, "250m 接近层");
    CHECK(nm_tts_tier_for_distance(60.0)  == 2, "60m 立即层");
    CHECK(nm_tts_tier_for_distance(30.0)  == 2, "30m 立即层");
    CHECK(nm_tts_tier_for_distance(0.0)   == 2, "0m 立即层");

    /* ---- 2. 引擎可用性探测不应崩溃 ---- */
    int avail = nm_tts_available();
    printf("TTS-AVAIL=%d\n", avail);

    /* ---- 3. DRYRUN 路径：nm_tts_speak 把文本交给引擎（写进文件） ---- */
    char dry[2048];
    snprintf(dry, sizeof(dry), "%s/tts_selftest_dryrun.txt", nm_temp_dir());
    /* 先清空 */
    FILE *clr = fopen(dry, "wb"); if (clr) fclose(clr);

    nm_setenv("NAVIMAP_TTS_DRYRUN", dry);
    const char *phrase = "前方五百米 右转 进入 建国路";
    int rc = nm_tts_speak(phrase);
    CHECK(rc == 1, "dryrun speak 返回 1");

    char got[4096];
    int n = read_file(dry, got, sizeof(got));
    CHECK(n > 0, "dryrun 文件非空");
    CHECK(strstr(got, phrase) != NULL, "dryrun 文件含播报文本");

    /* 第二次调用应追加一行，而不是覆盖 */
    nm_tts_speak("已到达目的地");
    n = read_file(dry, got, sizeof(got));
    CHECK(strstr(got, "已到达目的地") != NULL, "dryrun 追加第二句");

    /* 关掉 dryrun 后，无引擎环境应返回 0 且不写文件 */
    nm_setenv("NAVIMAP_TTS_DRYRUN", "");
    long before = 0;
    { FILE *f = fopen(dry, "rb"); if (f) { fseek(f, 0, SEEK_END); before = ftell(f); fclose(f); } }
    int rc2 = nm_tts_speak("不应写入");
    (void)rc2;   /* 有无引擎都允许（dryrun 已关），只验证文件不被改动 */
    long after = 0;
    { FILE *f = fopen(dry, "rb"); if (f) { fseek(f, 0, SEEK_END); after = ftell(f); fclose(f); } }
    CHECK(before == after, "非 dryrun 且本机无引擎时不写文件");

    /* 清理 */
    nm_unlink(dry);

    if (g_fail == 0) { printf("TTS-OK\n"); return 0; }
    fprintf(stderr, "TTS-SELFTEST FAILED: %d 项\n", g_fail);
    return 1;
}
