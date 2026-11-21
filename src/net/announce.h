/**
 * announce.h —— 塔台语音播报（航班进 30km 圈自动播报）
 *
 * 链路（照搬原仓库 emwstudio/esp32-radar 的思路，TTS 换成本机服务）：
 *   航班进圈 → 生成塔台文案（模板 + 航空读法 1→幺 2→两 7→拐 0→洞）
 *            → 请求 TTS 服务拿 WAV → ES8311 播放（电台音效）→ 屏幕 TX 同步点亮
 */
#ifndef ANNOUNCE_H
#define ANNOUNCE_H
#include "data/data_models.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 初始化（分配 PSRAM 缓冲 + 启动播报任务） */
void announce_init(void);

/* 设置面壁 ModelBest Key（VoxCPM2 云端语音，主用；空=不用） */
void announce_set_mbkey(const char *key);

/* 备用：设置局域网 TTS 服务地址 host:port（空=不用） */
void announce_set_host(const char *host_port);

/* 航班进圈通知（内部按呼号做 15 分钟冷却，重复调用安全） */
void announce_notify(const flight_t *f, float dist_km);

/* 进圈自动播报开关（1=开 0=只点击播报） */
void announce_set_auto(int on);
int  announce_auto(void);

/* 点击即播：跳过冷却/距离判断，立刻语音播报这架飞机 */
void announce_speak_now(const flight_t *f);

/* 正在播报？给屏幕 TX 指示用 */
int announce_busy(void);

/* 当前播报字幕（空串=无） */
void announce_status(char *buf, int n);

/* 调试：立即播报一次测试内容 */
void announce_test(void);

#ifdef __cplusplus
}
#endif
#endif
