/**
 * sim_audio.h —— PC 模拟器程序化音效 + WAV 播放（header-only, 零依赖）
 *
 * 用 SDL2 音频回调实时合成简单音色，不需要任何音频文件、不需要联网。
 * sim_atc.h 用 play_wav 接口播放 TTS 合成的语音（带电台质感处理）。
 * 只在 PC 模拟器编译（SIMULATOR=1），真板固件不 include 本文件。
 *
 * 用法：
 *   sim_audio_init();                 // main 开始
 *   sim_audio_blip(880, 60);          // 切页/按键：短促高音
 *   sim_audio_ping(1200, 140);        // 雷达命中：清脆"嘀"
 *   sim_audio_chime();                // 助手唤起：三音阶提示
 *   sim_audio_play_wav(path);         // 播放 WAV（任意线程可调）
 *   sim_audio_quit();                 // 退出前
 */
#ifndef SIM_AUDIO_H
#define SIM_AUDIO_H

#include <SDL2/SDL.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SIM_AUDIO_RATE 22050

typedef struct {
    double freq;      /* 当前音频率 Hz，0=静音 */
    double freq_end;  /* >0 时做线性滑音 */
    long   total;     /* 总采样数 */
    long   pos;       /* 已播放采样数 */
    double amp;       /* 音量 0..1 */
    int    shape;     /* 0=正弦 1=方波(更"电子") */
} sim_tone_t;

/* WAV 播放状态（sim_atc.h 的 TTS 语音走这里） */
typedef struct {
    int16_t *data;       /* 已加载的 PCM */
    long     n;          /* 采样数 */
    double   pos;        /* 播放位置（支持非 22050 采样率重采样） */
    double   step;       /* 采样步进 = wav_rate / 22050 */
    int      active;
    float    lp1, lp2;   /* 电台质感：两级低通状态 */
    unsigned int nseed;  /* 底噪 LCG，避免 rand() 的线程安全问题 */
} sim_wav_t;

static SDL_AudioDeviceID s_sim_audio_dev = 0;
static sim_tone_t        s_sim_tone;
static sim_wav_t         s_sim_wav;
static SDL_mutex        *s_sim_audio_lock = NULL;

/* 音频回调：SDL 每次要数据时把 合成音 + WAV(电台质感) 混进去 */
static void sim_audio_callback(void *userdata, Uint8 *stream, int len)
{
    (void)userdata;
    SDL_memset(stream, 0, (size_t)len);
    if (!s_sim_audio_lock) return;
    SDL_LockMutex(s_sim_audio_lock);

    int16_t *out = (int16_t *)stream;
    int n = len / (int)sizeof(int16_t);
    for (int i = 0; i < n; i++) {
        int32_t mix = 0;

        /* 1) 合成提示音 */
        if (s_sim_tone.pos < s_sim_tone.total && s_sim_tone.freq > 0) {
            double prog = (s_sim_tone.total > 1)
                        ? (double)s_sim_tone.pos / (double)s_sim_tone.total : 0.0;
            double f = s_sim_tone.freq;
            if (s_sim_tone.freq_end > 0)
                f += (s_sim_tone.freq_end - s_sim_tone.freq) * prog;
            double phase = 2.0 * 3.141592653589793 * f *
                           ((double)s_sim_tone.pos / SIM_AUDIO_RATE);
            /* 短促音两端做 8ms 淡入淡出，避免爆音 */
            double env = 1.0;
            long fade = (long)(SIM_AUDIO_RATE * 0.008);
            if (s_sim_tone.pos < fade)
                env = (double)s_sim_tone.pos / (double)fade;
            else if (s_sim_tone.total - s_sim_tone.pos < fade)
                env = (double)(s_sim_tone.total - s_sim_tone.pos) / (double)fade;
            if (env < 0.0) env = 0.0;

            double wave = (s_sim_tone.shape == 1)
                        ? (sin(phase) >= 0.0 ? 1.0 : -1.0)
                        : sin(phase);
            mix += (int32_t)(wave * s_sim_tone.amp * env * 20000.0);
            s_sim_tone.pos++;
        }

        /* 2) ATC 语音：两级低通带限 + 白噪声 = 无线电"电台质感" */
        if (s_sim_wav.active && s_sim_wav.data) {
            long i0 = (long)s_sim_wav.pos;
            if (i0 >= s_sim_wav.n) {
                s_sim_wav.active = 0;   /* 数据留到下次播放/退出时释放 */
            } else {
                float x = s_sim_wav.data[i0] / 32768.0f;
                s_sim_wav.lp1 += 0.45f * (x - s_sim_wav.lp1);
                s_sim_wav.lp2 += 0.45f * (s_sim_wav.lp1 - s_sim_wav.lp2);
                s_sim_wav.nseed = s_sim_wav.nseed * 1664525u + 1013904223u;
                float noise = ((float)(int)s_sim_wav.nseed / 2147483648.0f) * 0.04f;
                mix += (int32_t)((s_sim_wav.lp2 + noise) * 31000.0f);
                s_sim_wav.pos += s_sim_wav.step;
            }
        }

        if (mix > 32767)  mix = 32767;
        if (mix < -32768) mix = -32768;
        out[i] = (int16_t)mix;
    }
    SDL_UnlockMutex(s_sim_audio_lock);
}

/* 初始化：打开默认音频设备。失败则静默降级，不影响画面。 */
static void sim_audio_init(void)
{
    /* 本函数早于 lv_init() 调用，此时 SDL 尚未初始化，
     * 必须自己拉起音频子系统，否则 SDL_OpenAudioDevice 报
     * "Audio subsystem is not initialized" 而静音。 */
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
        printf("[audio] SDL 音频子系统初始化失败：%s\n", SDL_GetError());
        return;
    }
    SDL_AudioSpec want;
    SDL_zero(want);
    want.freq     = SIM_AUDIO_RATE;
    want.format   = AUDIO_S16SYS;
    want.channels = 1;
    want.samples  = 512;
    want.callback = sim_audio_callback;
    s_sim_audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
    if (s_sim_audio_dev) {
        s_sim_audio_lock = SDL_CreateMutex();
        memset(&s_sim_tone, 0, sizeof(s_sim_tone));
        memset(&s_sim_wav, 0, sizeof(s_sim_wav));
        SDL_PauseAudioDevice(s_sim_audio_dev, 0);
        printf("[audio] 已就绪（%d Hz, 合成音效）\n", SIM_AUDIO_RATE);
    } else {
        printf("[audio] 无可用音频设备，静音运行：%s\n", SDL_GetError());
    }
}

/* 触发一个音。若上一个还在响则重头开始。 */
static void sim_audio_tone(double f0, double f1, int ms, double amp, int shape)
{
    if (!s_sim_audio_dev || !s_sim_audio_lock) return;
    SDL_LockMutex(s_sim_audio_lock);
    s_sim_tone.freq     = f0;
    s_sim_tone.freq_end = f1;
    s_sim_tone.total    = (long)(SIM_AUDIO_RATE * ms / 1000.0);
    if (s_sim_tone.total < 1) s_sim_tone.total = 1;
    s_sim_tone.pos      = 0;
    s_sim_tone.amp      = amp;
    s_sim_tone.shape    = shape;
    SDL_UnlockMutex(s_sim_audio_lock);
}

/* 播放 WAV 文件（22050Hz 单声道 16bit；其他采样率自动线性重采样）。
 * 任意线程可调；成功后先来一声 PTT"咔哒"再进语音。 */
static void sim_audio_play_wav(const char *path)
{
    if (!s_sim_audio_dev || !s_sim_audio_lock) return;
    SDL_AudioSpec spec;
    Uint8 *buf = NULL;
    Uint32 len = 0;
    if (!SDL_LoadWAV(path, &spec, &buf, &len)) return;
    if (spec.format != AUDIO_S16SYS || spec.channels != 1 || len < 2) {
        SDL_FreeWAV(buf);
        return;
    }
    SDL_LockMutex(s_sim_audio_lock);
    free(s_sim_wav.data);
    s_sim_wav.data = (int16_t *)malloc(len);
    if (s_sim_wav.data) {
        memcpy(s_sim_wav.data, buf, len);
        s_sim_wav.n    = (long)(len / sizeof(int16_t));
        s_sim_wav.step = (double)spec.freq / SIM_AUDIO_RATE;
        if (s_sim_wav.step < 0.01) s_sim_wav.step = 1.0;
        s_sim_wav.pos    = 0.0;
        s_sim_wav.lp1    = 0.0f;
        s_sim_wav.lp2    = 0.0f;
        s_sim_wav.nseed  = 12345u;
        s_sim_wav.active = 1;
    }
    SDL_UnlockMutex(s_sim_audio_lock);
    SDL_FreeWAV(buf);
    if (s_sim_wav.active) sim_audio_tone(520, 400, 40, 0.12, 1);  /* PTT 咔哒 */
}

/* 切页 / 按键：短促"嗒" */
static void sim_audio_blip(void)
{
    sim_audio_tone(900, 1300, 70, 0.35, 1);
}

/* 雷达扫描命中目标：清脆上扬"嘀" */
static void sim_audio_ping(void)
{
    sim_audio_tone(1100, 1650, 160, 0.38, 0);
}

/* 助手唤起：三音阶"叮咚叮" */
static void sim_audio_chime(void)
{
    sim_audio_tone(660, 990, 220, 0.34, 0);
}

/* 退出 */
static void sim_audio_quit(void)
{
    if (s_sim_audio_dev) {
        SDL_PauseAudioDevice(s_sim_audio_dev, 1);
        SDL_CloseAudioDevice(s_sim_audio_dev);
        s_sim_audio_dev = 0;
    }
    if (s_sim_audio_lock) {
        SDL_LockMutex(s_sim_audio_lock);
        free(s_sim_wav.data);
        s_sim_wav.data = NULL;
        s_sim_wav.active = 0;
        SDL_UnlockMutex(s_sim_audio_lock);
        SDL_DestroyMutex(s_sim_audio_lock);
        s_sim_audio_lock = NULL;
    }
}

#ifdef __cplusplus
}
#endif
#endif /* SIM_AUDIO_H */
