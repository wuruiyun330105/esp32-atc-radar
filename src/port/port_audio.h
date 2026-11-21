/**
 * port_audio.h —— 设备端音频（ES8311 编解码器 + I2S 功放）
 *   提示音：运行时合成正弦波（切页/命中/数据到达）
 *   播报：WAV（PCM16）整段播放，带航空电台音效（降速 0.9x + 带通 + 软削波）
 */
#ifndef PORT_AUDIO_H
#define PORT_AUDIO_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

bool audio_init(void);
bool audio_ready(void);
bool audio_busy(void);

/* 播放一段正弦音（阻塞，单位 ms；freq<=0 表示静音等待） */
void audio_tone(int freq, int ms);

/* 播放内存里的 WAV（PCM16，8k~48k，单/双声道）。缓冲须在 PSRAM 或可写内存
 * （内部会就地做电台音效）。返回是否完整播完。 */
bool audio_play_wav(const uint8_t *wav, size_t len);

/* 语义化提示音 */
void audio_chime(void);
void audio_blip(void);
void audio_ping(void);
void audio_data(void);

void audio_set_volume(int vol);

/* 录音：采 ms 毫秒（16kHz 立体声 16bit），写成 WAV 到 out（含 44 字节头）。
 * 返回 WAV 总字节数，失败返回 0。用于语音指令。 */
int audio_record_wav(uint8_t *out, int cap, int ms);

#ifdef __cplusplus
}
#endif
#endif
