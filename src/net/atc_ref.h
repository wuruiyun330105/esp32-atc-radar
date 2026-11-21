/**
 * atc_ref.h —— 音色克隆参考音频（可选）
 *
 * 出于隐私考虑，本开源仓库不包含任何个人录音样本。
 * 如需使用 VoxCPM2 音色克隆，请自行生成 atc_ref_local.h：
 *   1. 录制一段 3~10 秒的清晰男声 WAV（16kHz / 16bit / 单声道）
 *   2. 转成 C 数组，定义 ATC_REF_WAV / ATC_REF_WAV_LEN / ATC_REF_TEXT
 *   3. announce.cpp 里把 #include "atc_ref.h" 改成 "atc_ref_local.h"
 *      （atc_ref_local.h 已加入 .gitignore，不会误提交）
 *
 * 下方为占位：0 长度 WAV，音色克隆请求会被服务端拒绝，
 * 此时 TTS 会回退到无克隆的合成语音。不影响其他功能。
 */
#ifndef ATC_REF_H
#define ATC_REF_H
#include <pgmspace.h>
static const char ATC_REF_TEXT[] = "";
static const unsigned char ATC_REF_WAV[] = {0};
#define ATC_REF_WAV_LEN 0
#endif
