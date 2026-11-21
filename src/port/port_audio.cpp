/**
 * port_audio.cpp —— 设备端音频（ES8311 + IDF I2S 驱动）
 *
 * 为什么不用 Arduino 的 ESP_I2S：
 *   它把 DMA 固定成 6 desc × 240 帧（48k 立体声只有约 30ms 缓冲），
 *   一旦 WiFi/TLS 抢占 CPU 就欠载 → 播报卡顿。这里直接用 esp_driver_i2s，
 *   DMA 提到 16 × 512 帧（约 170ms），并用大块写入。
 *
 * 音效：TTS 的 48kHz WAV 会重采样到 44.1kHz 播放——既是标准采样率（ES8311 认），
 * 又天然把语速压到 0.92 倍、音调略沉，听感更接近真人塔台通话。
 */
#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <string.h>
#include "driver/i2s_std.h"
#include "pin_config.h"
#include "es8311.h"
#include "port_audio.h"

#define AUDIO_SR       16000
#define AUDIO_MAX_FR   480

/* I2S 引脚（微雪官方 pin_config） */
#define I2S_MCK   42
#define I2S_BCLK   9
#define I2S_WS    45
#define I2S_DOUT   8
#define I2S_DIN   10

static es8311_handle_t s_es = nullptr;
static bool s_ok = false;
static int  s_vol = 78;

static i2s_chan_handle_t s_tx = nullptr;
static i2s_chan_handle_t s_rx = nullptr;
static int  s_rate = 0;
static bool s_stereo = true;

static int16_t s_frame[AUDIO_MAX_FR * 2];
static int16_t *s_pcm = nullptr;        /* 重采样后的输出缓冲（PSRAM） */
static const int PCM_FR = 4096;         /* 每次输出帧数 */
static volatile bool s_busy = false;

/* ---------- I2S 通道管理 ---------- */
static void i2s_teardown(void)
{
    if (s_tx) { i2s_channel_disable(s_tx); i2s_del_channel(s_tx); s_tx = nullptr; }
    if (s_rx) { i2s_channel_disable(s_rx); i2s_del_channel(s_rx); s_rx = nullptr; }
    s_rate = 0;
}

static bool i2s_setup(int rate, bool stereo)
{
    if (s_tx && rate == s_rate && stereo == s_stereo) return true;
    i2s_teardown();

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 16;          /* 默认 6 → 16，抗抢占 */
    chan_cfg.dma_frame_num = 512;        /* 默认 240 → 512 */
    chan_cfg.auto_clear = true;
    if (i2s_new_channel(&chan_cfg, &s_tx, &s_rx) != ESP_OK) {
        Serial.println("[audio] i2s_new_channel 失败");
        return false;
    }

    i2s_std_config_t std_cfg;
    memset(&std_cfg, 0, sizeof(std_cfg));
    std_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate);
    std_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_16BIT, stereo ? I2S_SLOT_MODE_STEREO : I2S_SLOT_MODE_MONO);
    std_cfg.gpio_cfg.mclk = (gpio_num_t)I2S_MCK;
    std_cfg.gpio_cfg.bclk = (gpio_num_t)I2S_BCLK;
    std_cfg.gpio_cfg.ws   = (gpio_num_t)I2S_WS;
    std_cfg.gpio_cfg.dout = (gpio_num_t)I2S_DOUT;
    std_cfg.gpio_cfg.din  = (gpio_num_t)I2S_DIN;

    if (i2s_channel_init_std_mode(s_tx, &std_cfg) != ESP_OK ||
        i2s_channel_init_std_mode(s_rx, &std_cfg) != ESP_OK) {
        Serial.println("[audio] i2s init std 失败");
        i2s_teardown();
        return false;
    }
    i2s_channel_enable(s_tx);
    i2s_channel_enable(s_rx);
    s_rate = rate;
    s_stereo = stereo;

    if (s_es) es8311_sample_frequency_config(s_es, rate * 256, rate);
    return true;
}

/* 收尾：补静音冲 DMA 尾，关通道，关功放 */
static void play_end(void)
{
    if (s_tx && s_rate > 0) {
        static uint8_t zeros[4096] = {0};
        size_t tail = (size_t)s_rate * (s_stereo ? 4 : 2) / 6;   /* ≈165ms */
        size_t written = 0;
        while (tail) {
            size_t c = tail > sizeof(zeros) ? sizeof(zeros) : tail;
            if (i2s_channel_write(s_tx, zeros, c, &written, 200) != ESP_OK) break;
            tail -= c;
        }
    }
    i2s_teardown();
    digitalWrite(PA, LOW);
}

bool audio_init(void)
{
    if (s_ok) return true;

    Wire.begin(IIC_SDA, IIC_SCL);

    pinMode(PA, OUTPUT);
    digitalWrite(PA, LOW);

    s_es = es8311_create(0, ES8311_ADDRRES_0);
    if (!s_es) {
        Serial.println("[audio] ES8311 create 失败");
        return false;
    }
    es8311_clock_config_t clk;
    memset(&clk, 0, sizeof(clk));
    clk.mclk_inverted      = false;
    clk.sclk_inverted      = false;
    clk.mclk_from_mclk_pin = true;
    clk.mclk_frequency     = AUDIO_SR * 256;
    clk.sample_frequency   = AUDIO_SR;
    if (es8311_init(s_es, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) != ESP_OK) {
        Serial.println("[audio] ES8311 init 失败");
        return false;
    }
    es8311_sample_frequency_config(s_es, clk.mclk_frequency, clk.sample_frequency);
    es8311_microphone_config(s_es, false);
    es8311_voice_volume_set(s_es, s_vol, NULL);

    if (!s_pcm) s_pcm = (int16_t *)heap_caps_malloc(PCM_FR * 2 * sizeof(int16_t),
                                                    MALLOC_CAP_SPIRAM);
    s_ok = true;
    Serial.printf("[audio] ES8311 OK（音量 %d，I2S DMA 16x512）\n", s_vol);
    return true;
}

bool audio_ready(void) { return s_ok; }
bool audio_busy(void) { return s_busy; }

void audio_set_volume(int vol)
{
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    s_vol = vol;
    if (s_ok) es8311_voice_volume_set(s_es, s_vol, NULL);
}

/* ---------- 提示音 ---------- */
void audio_tone(int freq, int ms)
{
    if (!s_ok || ms <= 0) return;
    if (freq <= 0) { vTaskDelay(ms); return; }
    if (!i2s_setup(AUDIO_SR, true)) return;

    digitalWrite(PA, HIGH);
    vTaskDelay(pdMS_TO_TICKS(5));

    const int total = AUDIO_SR * ms / 1000;
    const int fade  = AUDIO_SR * 3 / 1000;
    const float amp = 9000.0f;
    int done = 0;
    while (done < total) {
        int n = total - done;
        if (n > AUDIO_MAX_FR) n = AUDIO_MAX_FR;
        for (int i = 0; i < n; i++) {
            int k = done + i;
            float env = 1.0f;
            if (k < fade) env = (float)k / fade;
            else if (k > total - fade) env = (float)(total - k) / fade;
            int16_t v = (int16_t)(sinf(2.0f * (float)M_PI * freq * k / AUDIO_SR) * amp * env);
            s_frame[i * 2] = v;
            s_frame[i * 2 + 1] = v;
        }
        size_t written = 0;
        i2s_channel_write(s_tx, s_frame, n * 4, &written, 500);
        done += n;
    }
    play_end();
}

/* ---------- WAV ---------- */
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* 航空电台音效：带通 300Hz~3kHz + 软削波 + 极轻底噪 */
static void radio_fx(int16_t *s, size_t n, int rate)
{
    const float dt  = 1.0f / rate;
    const float aHP = (1.0f / (2 * (float)M_PI * 300.0f)) / (1.0f / (2 * (float)M_PI * 300.0f) + dt);
    const float aLP = dt / (1.0f / (2 * (float)M_PI * 3800.0f) + dt);
    float prevX = 0, yHP = 0, yLP = 0;
    static uint32_t rng = 0x12345678u;      /* 轻量 LCG：比 random() 省栈省锁 */
    for (size_t i = 0; i < n; i++) {
        float x = s[i] / 32768.0f;
        yHP = aHP * (yHP + x - prevX);
        prevX = x;
        yLP += aLP * (yHP - yLP);
        /* tanh 近似（比 tanhf 快数倍，音色几乎无差） */
        float t = yLP * 1.5f;                       /* 削波更软：少一点"电台"机械感 */
        float y = t / (1.0f + fabsf(t));
        rng = rng * 1664525u + 1013904223u;
        y += (float)((int32_t)(rng >> 8) % 201 - 100) / 32768.0f;
        int v = (int)(y * 31500.0f);
        s[i] = (int16_t)constrain(v, -32768, 32767);
    }
}

bool audio_play_wav(const uint8_t *wav, size_t len)
{
    if (!s_ok || !wav || !s_pcm || len < 44) return false;
    if (memcmp(wav, "RIFF", 4) || memcmp(wav + 8, "WAVE", 4)) return false;

    int rate = 0, channels = 0, bits = 0, fmtTag = 0;
    const uint8_t *pcm = nullptr;
    size_t pcmLen = 0, off = 12;
    while (off + 8 <= len) {
        uint32_t csz = rd32(wav + off + 4);
        if (!memcmp(wav + off, "fmt ", 4) && off + 24 <= len) {
            fmtTag   = rd16(wav + off + 8);
            channels = rd16(wav + off + 10);
            rate     = (int)rd32(wav + off + 12);
            bits     = rd16(wav + off + 22);
        } else if (!memcmp(wav + off, "data", 4)) {
            pcm = wav + off + 8;
            size_t avail = len - (off + 8);
            pcmLen = csz > avail ? avail : csz;
        }
        off += 8 + csz + (csz & 1);
    }
    if (!pcm || !pcmLen || fmtTag != 1 || bits != 16 ||
        (channels != 1 && channels != 2) || rate < 8000 || rate > 48000) {
        Serial.printf("[audio] WAV 不支持: fmt=%d ch=%d bits=%d rate=%d\n",
                      fmtTag, channels, bits, rate);
        return false;
    }

    /* 播放采样率：48k 重采样到 44.1k（标准率 + 语速 0.92x，更拟人）；
     * 其他标准率按 0.9x 降速；降后不是标准率就原速播。 */
    static const int STD[] = { 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000 };
    int outRate = rate;
    bool resample = false;
    if (rate == 48000) { outRate = 44100; resample = true; }
    else {
        int slow = (int)(rate * 0.90f);
        for (int i = 0; i < 9; i++) if (slow == STD[i]) { outRate = slow; resample = true; break; }
    }

    if (!i2s_setup(outRate, channels == 2)) return false;

    s_busy = true;
    digitalWrite(PA, HIGH);
    vTaskDelay(pdMS_TO_TICKS(5));
    Serial.printf("[audio] 播放 %d->%d Hz %s %u 字节%s\n", rate, outRate,
                  channels == 2 ? "立体声" : "单声道", (unsigned)pcmLen,
                  resample ? "（重采样降速）" : "");

    const int ch = channels;
    const int inFrames = (int)(pcmLen / 2 / ch);
    const int16_t *src = (const int16_t *)pcm;
    const double step = (double)rate / (double)outRate;

    double pos = 0.0;
    bool okAll = true;
    while ((int)pos < inFrames - 1) {
        int n = 0;
        while (n < PCM_FR && (int)pos < inFrames - 1) {
            int i0 = (int)pos;
            float frac = (float)(pos - i0);
            for (int c = 0; c < ch; c++) {
                float a = (float)src[i0 * ch + c];
                float b = (float)src[(i0 + 1) * ch + c];
                s_pcm[n * ch + c] = (int16_t)(a + (b - a) * frac);
            }
            n++;
            pos += step;
        }
        radio_fx(s_pcm, (size_t)n * ch, outRate);
        size_t bytes = (size_t)n * ch * 2;
        size_t written = 0;
        if (i2s_channel_write(s_tx, s_pcm, bytes, &written, 1000) != ESP_OK ||
            written != bytes) { okAll = false; break; }
    }
    play_end();
    s_busy = false;
    return okAll;
}

/* ---------- 录音（语音指令用） ---------- */
int audio_record_wav(uint8_t *out, int cap, int ms)
{
    if (!s_ok || !out || cap < 4096 || ms <= 0) return 0;
    const int rate = 16000, ch = 1, bits = 16;
    if (!i2s_setup(rate, true)) return 0;      /* 通道按立体声配置，取左声道 */
    digitalWrite(PA, LOW);
    vTaskDelay(pdMS_TO_TICKS(80));

    int want = rate * (bits / 8) * ms / 1000;
    if (want > cap - 44) want = cap - 44;
    int got = 0;
    static int16_t tmp[2048];
    uint32_t t0 = millis();
    while (got < want && millis() - t0 < (uint32_t)ms + 1200) {
        size_t rd = 0;
        if (i2s_channel_read(s_rx, tmp, sizeof(tmp), &rd, 200) != ESP_OK) break;
        int frames = (int)(rd / 4);
        for (int i = 0; i < frames && got + 2 <= want; i++) {
            out[44 + got]     = (uint8_t)(tmp[i * 2] & 0xFF);
            out[44 + got + 1] = (uint8_t)((tmp[i * 2] >> 8) & 0xFF);
            got += 2;
        }
    }

    const uint32_t dataSz = (uint32_t)got;
    const uint32_t riffSz = 36 + dataSz;
    memcpy(out, "RIFF", 4);
    out[4] = riffSz & 0xFF; out[5] = (riffSz >> 8) & 0xFF;
    out[6] = (riffSz >> 16) & 0xFF; out[7] = (riffSz >> 24) & 0xFF;
    memcpy(out + 8, "WAVEfmt ", 8);
    out[16] = 16; out[17] = 0; out[18] = 0; out[19] = 0;
    out[20] = 1; out[21] = 0;
    out[22] = ch; out[23] = 0;
    uint32_t sr = rate;
    out[24] = sr & 0xFF; out[25] = (sr >> 8) & 0xFF;
    out[26] = (sr >> 16) & 0xFF; out[27] = (sr >> 24) & 0xFF;
    uint32_t br = (uint32_t)rate * ch * (bits / 8);
    out[28] = br & 0xFF; out[29] = (br >> 8) & 0xFF;
    out[30] = (br >> 16) & 0xFF; out[31] = (br >> 24) & 0xFF;
    out[32] = (uint8_t)(ch * bits / 8); out[33] = 0;
    out[34] = bits; out[35] = 0;
    memcpy(out + 36, "data", 4);
    out[40] = dataSz & 0xFF; out[41] = (dataSz >> 8) & 0xFF;
    out[42] = (dataSz >> 16) & 0xFF; out[43] = (dataSz >> 24) & 0xFF;

    i2s_teardown();
    return got > 0 ? (int)(44 + dataSz) : 0;
}

/* ---------- 语义化提示音 ---------- */
void audio_chime(void) { audio_tone(784, 70); audio_tone(1046, 70); audio_tone(1568, 90); }
void audio_blip(void)  { audio_tone(520, 45); }
void audio_ping(void)  { audio_tone(1245, 55); }
void audio_data(void)  { audio_tone(988, 45); audio_tone(1319, 60); }
