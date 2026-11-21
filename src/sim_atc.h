/**
 * sim_atc.h —— PC 模拟器"模拟塔台对话"（header-only，零新依赖）
 *
 * 合规边界（评审红线）：只用【虚构呼号 + 标准陆空通话用语模板】生成对话，
 * 不含任何真实管制通话的录音/转写内容。数字按民航读法（1=幺 0=洞 7=拐 9=勾）。
 *
 * 语音链路：Windows 自带 SAPI TTS（PowerShell 调用，离线、无 API key）
 *   → 合成 22050Hz WAV → SDL 回调加"电台质感"（带限+底噪+PTT咔哒）播放。
 * 中文文本经 atc_text.txt（UTF-8）传给 PowerShell，避开命令行编码坑。
 *
 * 环境：SIM_ATC=0 关闭；无 PowerShell / 无中文语音 / 合成失败时自动禁用。
 * 触发：sim_atc_announce() 由主循环按周期调，按高度/升降率挑模板。
 */
#ifndef SIM_ATC_H
#define SIM_ATC_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include "data_flight.h"

#define ATC_QN   8
#define ATC_TXT  256

static volatile int s_atc_enabled = 0;   /* 初始化成功且未失败 */
static volatile int s_atc_running = 0;
static pthread_t    s_atc_th;
static pthread_mutex_t s_atc_qm = PTHREAD_MUTEX_INITIALIZER;
static char s_atc_q[ATC_QN][ATC_TXT];
static int  s_atc_qh = 0, s_atc_qt = 0;
static int  s_atc_hi_toggle = 0;         /* 高空目标模板轮换 */

/* PowerShell + System.Speech 命令（全 ASCII；文本走 UTF-8 文件）。
 * 无中文语音时 exit 2，C 端据此禁用。 */
static const char *ATC_PS_CMD =
  "powershell -NoProfile -Command "
  "\"Add-Type -AssemblyName System.Speech;"
  "$s=New-Object System.Speech.Synthesis.SpeechSynthesizer;"
  "$z=($s.GetInstalledVoices()|Where-Object{$_.VoiceInfo.Culture.Name.StartsWith('zh')})|Select-Object -First 1;"
  "if(-not $z){exit 2};"
  "$s.SelectVoice($z.VoiceInfo.Name);$s.Rate=1;"
  "$s.SetOutputToWaveFile('atc_line.wav',"
  "(New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(22050,"
  "[System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen,"
  "[System.Speech.AudioFormat.AudioChannel]::Mono)));"
  "$t=Get-Content -Raw -Encoding UTF8 'atc_text.txt';"
  "$s.Speak($t);$s.Dispose()\"";

static void sim_atc_push(const char *text)
{
    if (!s_atc_enabled) return;
    pthread_mutex_lock(&s_atc_qm);
    int next = (s_atc_qt + 1) % ATC_QN;
    if (next != s_atc_qh) {
        snprintf(s_atc_q[s_atc_qt], ATC_TXT, "%s", text);
        s_atc_qt = next;
    }
    pthread_mutex_unlock(&s_atc_qm);
}

/* 数字 → 民航读法（0=洞 1=幺 2=两 7=拐 9=勾），字母原样保留 */
static void atc_cn_digits(const char *src, char *dst, int dstn)
{
    static const char *rep[10] = {"洞","幺","两","三","四","五","六","拐","八","勾"};
    int w = 0;
    for (; *src && w < dstn - 8; src++) {
        if (*src >= '0' && *src <= '9')
            w += snprintf(dst + w, (size_t)(dstn - w), "%s", rep[*src - '0']);
        else
            w += snprintf(dst + w, (size_t)(dstn - w), "%c", *src);
    }
    dst[w] = '\0';
}

/* 整数 → 中文读法（供高度米数，0..9999 足够） */
static void atc_cn_num(int n, char *dst, int dstn)
{
    static const char *d[10] = {"零","一","二","三","四","五","六","七","八","九"};
    char tmp[64]; tmp[0] = '\0'; int w = 0;
    int q = n / 1000 % 10, b = n / 100 % 10, s = n / 10 % 10, g = n % 10;
    if (n <= 0) { snprintf(dst, (size_t)dstn, "地面"); return; }
    if (q) w += snprintf(tmp + w, sizeof(tmp) - (size_t)w, "%s千", d[q]);
    if (b) w += snprintf(tmp + w, sizeof(tmp) - (size_t)w, "%s百", d[b]);
    else if (q && (s || g)) w += snprintf(tmp + w, sizeof(tmp) - (size_t)w, "零");
    if (s) {
        if (s == 1 && !(q || b))
            w += snprintf(tmp + w, sizeof(tmp) - (size_t)w, "十");
        else
            w += snprintf(tmp + w, sizeof(tmp) - (size_t)w, "%s十", d[s]);
    }
    if (g) w += snprintf(tmp + w, sizeof(tmp) - (size_t)w, "%s", d[g]);
    snprintf(dst, (size_t)dstn, "%s", tmp);
}

/* 话术模板（虚构话术，风格参考标准陆空通话；不含真实通话内容）
 * variant: 0=落地 1=起飞后移交 2=进近雷达识别 3=放行/区域 */
static void sim_atc_make_line(const flight_t *f, int variant, char *out, int n)
{
    char cs[FL_CALL_MAX * 2];
    atc_cn_digits(f->callsign, cs, (int)sizeof(cs));
    /* 高度：输入为百英尺 → 米（取整到 50m） */
    int m    = (int)(f->alt * 100.0f * 0.3048f);
    int altm = (m / 50) * 50;
    char alm[32];
    atc_cn_num(altm, alm, (int)sizeof(alm));
    switch (variant) {
    case 0:
        snprintf(out, (size_t)n, "虹桥塔台，%s，跑道三六左，可以落地，地面风二四零，四米秒。", cs);
        break;
    case 1:
        snprintf(out, (size_t)n, "%s，联系进近幺幺九点零，再见。", cs);
        break;
    case 2:
        snprintf(out, (size_t)n, "进近，%s，雷达识别，下降到%s米保持。", cs, alm);
        break;
    default:
        if (s_atc_hi_toggle)
            snprintf(out, (size_t)n, "%s，虹桥放行，可以开车，跑道三六左外等。", cs);
        else
            snprintf(out, (size_t)n, "%s，雷达识别，保持当前高度，预计到达时间不变。", cs);
        s_atc_hi_toggle ^= 1;
        break;
    }
}

/* 合成线程：取文本 → PowerShell 生成 WAV → 交给 SDL 播放 */
static void *sim_atc_worker(void *arg)
{
    (void)arg;
    char line[ATC_TXT];
    while (s_atc_running) {
        int have = 0;
        pthread_mutex_lock(&s_atc_qm);
        if (s_atc_qh != s_atc_qt) {
            snprintf(line, ATC_TXT, "%s", s_atc_q[s_atc_qh]);
            s_atc_qh = (s_atc_qh + 1) % ATC_QN;
            have = 1;
        }
        pthread_mutex_unlock(&s_atc_qm);
        if (!have) { usleep(300 * 1000); continue; }

        FILE *tf = fopen("atc_text.txt", "wb");
        if (!tf) break;
        fwrite(line, 1, strlen(line), tf);
        fclose(tf);

        remove("atc_line.wav");
        int rc = system(ATC_PS_CMD);
        FILE *wf = fopen("atc_line.wav", "rb");
        if (!wf) {
            printf("[atc] 语音合成失败(rc=%d)，模拟对话禁用（需 PowerShell + 中文语音包）\n", rc);
            s_atc_enabled = 0;
            break;
        }
        fclose(wf);
        sim_audio_play_wav("atc_line.wav");
        printf("[atc] %s\n", line);
        /* 两句之间至少隔 4 秒，避免连续轰炸 */
        usleep(4 * 1000 * 1000);
    }
    return NULL;
}

static void sim_atc_init(void)
{
    s_atc_running = 1;
    s_atc_enabled = 1;
    if (pthread_create(&s_atc_th, NULL, sim_atc_worker, NULL) == 0) {
        pthread_detach(s_atc_th);
        printf("[atc] 模拟塔台对话已启用（SAPI TTS 离线合成 + 电台质感）\n");
        sim_atc_push("虹桥塔台，模拟对话已启用。");
    } else {
        printf("[atc] 对话线程创建失败，禁用\n");
        s_atc_enabled = 0;
    }
}

static void sim_atc_quit(void)
{
    s_atc_running = 0;
    /* 线程可能阻塞在 system() 里，不 join，进程退出统一回收 */
}

/* 从航班列表挑一个目标播报：优先低空（进离场阶段话术最自然） */
static void sim_atc_announce(const flight_list_t *fl)
{
    if (!s_atc_enabled || !fl || fl->count <= 0) return;
    const flight_t *best = NULL;
    int best_score = 1 << 20;
    for (int i = 0; i < fl->count; i++) {
        const flight_t *f = &fl->items[i];
        if (!f->active || !f->callsign[0]) continue;
        int score = f->alt + (f->dist_nm > 30.0f ? 20 : 0);
        if (score < best_score) { best_score = score; best = f; }
    }
    if (!best) return;
    int v = (best->alt < 20) ? (best->vs < 0 ? 0 : 1)
          : (best->alt < 60 ? 2 : 3);
    char line[ATC_TXT];
    sim_atc_make_line(best, v, line, ATC_TXT);
    sim_atc_push(line);
}

#endif /* SIM_ATC_H */
