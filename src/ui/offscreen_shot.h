#ifndef NM_UI_OFFSCREEN_SHOT_H
#define NM_UI_OFFSCREEN_SHOT_H

#ifdef __cplusplus
extern "C" {
#endif

/* 离屏像素审计通道的**单一可信源**。
 *
 * 把"参数解析 + 算路/搜索/模拟/GPX 设置 + 渲到内存缓冲 + 共享审计
 * (app_offscreen_audit) + 存 24 位 BMP"整条链路收在一处，让
 * skedaddle --shot-offscreen 与 regress.sh 的离屏判据共用同一份逻辑。
 *
 * ★ 不依赖 SDL：纯内存缓冲（0x00RRGGBB）+ 内建 24 位 BMP 写出。
 *   —— skedaddle 不链 SDL，这条路径必须零 SDL 依赖。
 *   —— ovl_px.py 只读 24 位 BMP，所以这里写 24 位（BGR、行 4 字节对齐、
 *      自下而上存储）。
 *
 * 返回 app_offscreen_audit 的退出码：0=全部 ASSERT-OK，1=有 ASSERT-FAIL。 */
int offscreen_shot_main(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* NM_UI_OFFSCREEN_SHOT_H */
