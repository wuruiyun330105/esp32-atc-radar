/**
 * data_config.c —— 配置读写实现（编译期区分 PC / 设备）
 */
#include "data_config.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef ESP_PLATFORM
/* ======================= 设备端：NVS ======================= */
#include <Preferences.h>
static Preferences s_prefs;

void cfg_defaults(app_config_t *o)
{
    memset(o, 0, sizeof(*o));
    o->latitude  = 31.2304f;   /* 上海（人民广场），配网页可改 */
    o->longitude = 121.4737f;
    o->range_nm  = 120;
    o->theme     = 0;
    o->flight_src = FL_SRC_ADSB;
    /* 默认值留空；NVS 为空时由配网页填写 WiFi */
    /* WiFi 请在配网页或 app_config.json 中填写 */
    o->wifi_ssid[0] = 0;
    o->wifi_pass[0] = 0;
    /* 播报：面壁 VoxCPM2 云端合成（主用）；局域网 TTS 为后备 */
    o->mb_key[0] = 0;   /* 面壁 Key：配网页填写 */
    o->tts_host[0] = 0; /* 局域网 TTS：可选 */
    /* 对话模型（预留，助手页已移除） */
    o->ai_url[0] = 0;
    o->ai_key[0] = 0;
    o->ai_model[0] = 0;
    /* Key 全部留空，运行时通过配网页写入 NVS */
    o->qweather_key[0] = 0;
    o->qweather_host[0] = 0;
    o->vf_key[0] = 0;
}

/* 用户是否通过配网页手动保存过坐标（没保存过 → 允许开机 IP 自动定位） */
static bool s_posSaved = false;
bool cfg_pos_saved(void) { return s_posSaved; }

bool cfg_load(app_config_t *o)
{
    cfg_defaults(o);
    if (!s_prefs.begin("radar", true)) return false;
    s_prefs.getString("ssid", o->wifi_ssid, sizeof(o->wifi_ssid));
    s_prefs.getString("pass", o->wifi_pass, sizeof(o->wifi_pass));
    s_prefs.getString("qkey", o->qweather_key, sizeof(o->qweather_key));
    s_prefs.getString("qhost", o->qweather_host, sizeof(o->qweather_host));
    s_posSaved = s_prefs.isKey("lat");
    o->latitude  = s_prefs.getFloat("lat", o->latitude);
    o->longitude = s_prefs.getFloat("lon", o->longitude);
    o->range_nm  = s_prefs.getInt("range", o->range_nm);
    o->theme     = s_prefs.getInt("theme", o->theme);
    o->flight_src = s_prefs.getInt("fsrc", o->flight_src);
    o->ann_auto   = s_prefs.getInt("annauto", 1);
    s_prefs.getString("ahost", o->adsb_host, sizeof(o->adsb_host));
    s_prefs.getString("vfkey", o->vf_key, sizeof(o->vf_key));
    s_prefs.getString("tts", o->tts_host, sizeof(o->tts_host));
    s_prefs.getString("mbkey", o->mb_key, sizeof(o->mb_key));
    s_prefs.getString("aiurl", o->ai_url, sizeof(o->ai_url));
    s_prefs.getString("aikey", o->ai_key, sizeof(o->ai_key));
    s_prefs.getString("aimodel", o->ai_model, sizeof(o->ai_model));
    bool has = strlen(o->wifi_ssid) > 0;
    s_prefs.end();
    return has;
}

bool cfg_save(const app_config_t *o)
{
    if (!s_prefs.begin("radar", false)) return false;
    s_prefs.putString("ssid", o->wifi_ssid);
    s_prefs.putString("pass", o->wifi_pass);
    s_prefs.putString("qkey", o->qweather_key);
    s_prefs.putString("qhost", o->qweather_host);
    s_prefs.putFloat("lat", o->latitude);
    s_prefs.putFloat("lon", o->longitude);
    s_prefs.putInt("range", o->range_nm);
    s_prefs.putInt("theme", o->theme);
    s_prefs.putInt("fsrc", o->flight_src);
    s_prefs.putInt("annauto", o->ann_auto);
    s_prefs.putString("ahost", o->adsb_host);
    s_prefs.putString("vfkey", o->vf_key);
    s_prefs.putString("tts", o->tts_host);
    s_prefs.putString("mbkey", o->mb_key);
    s_prefs.putString("aiurl", o->ai_url);
    s_prefs.putString("aikey", o->ai_key);
    s_prefs.putString("aimodel", o->ai_model);
    s_prefs.end();
    return true;
}

#else
/* ======================= PC：JSON 文件 ======================= */
#define CFG_FILE "app_config.json"

void cfg_defaults(app_config_t *o)
{
    memset(o, 0, sizeof(*o));
    o->latitude  = 31.2304f;   /* 上海，app_config.json 可覆盖 */
    o->longitude = 121.4737f;
    o->range_nm  = 120;
    o->theme     = 0;
    o->flight_src = FL_SRC_ADSB;
    strncpy(o->tts_host, "127.0.0.1:8765", sizeof(o->tts_host) - 1);
    /* Key 留空，由 app_config.json 提供 */
    o->qweather_key[0] = 0;
    o->qweather_host[0] = 0;
    o->vf_key[0] = 0;
}

/* 极简提取字符串值： "key":"value" ；找不到返回 false */
static bool jget_str(const char *buf, const char *key, char *out, int cap)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(buf, pat);
    if (!p) return false;
    p = strchr(p, ':');
    if (!p) return false;
    p = strchr(p, '"');
    if (!p) return false;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < cap - 1) out[i++] = *p++;
    out[i] = '\0';
    return true;
}

static bool jget_num(const char *buf, const char *key, double *out)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(buf, pat);
    if (!p) return false;
    p = strchr(p, ':');
    if (!p) return false;
    *out = strtod(p + 1, NULL);
    return true;
}

/* 用户是否通过配网页手动保存过坐标（没保存过 → 允许开机 IP 自动定位） */
static bool s_posSaved = false;
bool cfg_pos_saved(void) { return s_posSaved; }

bool cfg_load(app_config_t *o)
{
    cfg_defaults(o);
    FILE *f = fopen(CFG_FILE, "rb");
    if (!f) return false;
    char buf[1024]; size_t n = fread(buf, 1, sizeof(buf) - 1, f); buf[n] = '\0'; fclose(f);
    jget_str(buf, "ssid",  o->wifi_ssid,  sizeof(o->wifi_ssid));
    jget_str(buf, "pass",  o->wifi_pass,  sizeof(o->wifi_pass));
    jget_str(buf, "qkey",  o->qweather_key, sizeof(o->qweather_key));
    jget_str(buf, "qhost", o->qweather_host, sizeof(o->qweather_host));
    jget_str(buf, "ahost", o->adsb_host, sizeof(o->adsb_host));
    jget_str(buf, "tts",   o->tts_host,   sizeof(o->tts_host));
    jget_str(buf, "mbkey", o->mb_key,     sizeof(o->mb_key));
    jget_str(buf, "vfkey", o->vf_key, sizeof(o->vf_key));
    double d;
    if (jget_num(buf, "lat", &d)) o->latitude = (float)d;
    if (jget_num(buf, "lon", &d)) o->longitude = (float)d;
    if (jget_num(buf, "range", &d)) o->range_nm = (int)d;
    if (jget_num(buf, "theme", &d)) o->theme = (int)d;
    if (jget_num(buf, "fsrc", &d)) o->flight_src = (int)d;
    return strlen(o->wifi_ssid) > 0;
}

bool cfg_save(const app_config_t *o)
{
    FILE *f = fopen(CFG_FILE, "wb");
    if (!f) return false;
    fprintf(f,
        "{\"ssid\":\"%s\",\"pass\":\"%s\",\"qkey\":\"%s\",\"qhost\":\"%s\","
        "\"lat\":%.5f,\"lon\":%.5f,\"range\":%d,\"theme\":%d,\"fsrc\":%d,"
        "\"ahost\":\"%s\",\"vfkey\":\"%s\",\"tts\":\"%s\",\"mbkey\":\"%s\"}\n",
        o->wifi_ssid, o->wifi_pass, o->qweather_key, o->qweather_host,
        o->latitude, o->longitude, o->range_nm, o->theme, o->flight_src,
        o->adsb_host, o->vf_key, o->tts_host, o->mb_key);
    fclose(f);
    return true;
}
#endif
