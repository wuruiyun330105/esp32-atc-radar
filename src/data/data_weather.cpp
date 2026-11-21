/**
 * data_weather.c —— 和风天气实时查询 + JSON 解析
 *   PC  : 用系统 curl 取一次（便于联调），再走同一套解析
 *   设备: 用 HTTPClient/HTTPS 取
 */
#include "data_weather.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef ESP_PLATFORM
#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#endif

/* ---------- 从 JSON 里提取 "key":"value" / "key":number ---------- */
static bool j_str(const char *buf, const char *key, char *out, int cap)
{
    char pat[48]; snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(buf, pat); if (!p) return false;
    p = strchr(p, ':'); if (!p) return false;
    p = strchr(p, '"'); if (!p) return false; p++;
    int i = 0; while (*p && *p != '"' && i < cap - 1) out[i++] = *p++;
    out[i] = '\0'; return true;
}
static bool j_num(const char *buf, const char *key, int *out)
{
    char pat[48]; snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(buf, pat); if (!p) return false;
    p = strchr(p, ':'); if (!p) return false;
    p++;
    /* 和风把数值也包在引号里（"temp":"15"），跳过引号再转 */
    while (*p == ' ' || *p == '\t' || *p == '"') p++;
    *out = atoi(p); return true;
}

/* icon 代码 -> 图标类别：0晴 1多云 2阴 3雨 4雪 5雾/沙 */
static int icon_to_cat(int icon)
{
    if (icon == 100 || icon == 150) return 0;              /* 晴 */
    if (icon >= 101 && icon <= 103) return 1;              /* 多云 */
    if (icon == 104) return 2;                             /* 阴 */
    if (icon >= 300 && icon <= 399) return 3;              /* 雨 */
    if (icon >= 400 && icon <= 499) return 4;              /* 雪 */
    if (icon >= 500 && icon <= 515) return 5;              /* 雾/霾/沙 */
    return 1;
}

bool weather_parse_now(const char *json, weather_data_t *o)
{
    if (!json || !o) return false;
    /* 校验 code=200 */
    char code[8];
    if (!j_str(json, "code", code, sizeof(code)) || strcmp(code, "200") != 0)
        return false;

    /* now 对象内的字段：直接用整段搜（字段名唯一，够用） */
    int v;
    if (j_num(json, "temp", &v))      o->temp = v;
    if (j_num(json, "feelsLike", &v)) o->feels_like = v;
    if (j_num(json, "humidity", &v))  o->humidity = v;
    if (j_num(json, "windScale", &v)) o->wind_scale = v;
    if (j_num(json, "pressure", &v))  o->pressure = v;
    if (j_num(json, "precip", &v))    o->precip = v;
    if (j_num(json, "icon", &v))      o->icon = v;

    char s[32];
    if (j_str(json, "text", s, sizeof(s)))      strncpy(o->condition, s, sizeof(o->condition) - 1);
    if (j_str(json, "windDir", s, sizeof(s)))   strncpy(o->wind_dir, s, sizeof(o->wind_dir) - 1);

    /* obsTime: "2026-11-17T08:05+08:00" -> 取 08:05 */
    if (j_str(json, "obsTime", s, sizeof(s))) {
        const char *t = strchr(s, 'T');
        if (t) { o->update_hour = atoi(t + 1); o->update_min = atoi(t + 4); }
    }
    o->valid = 1;
    return true;
}

#ifdef ESP_PLATFORM
/* ======================= 设备端 ======================= */
#include "gzip_lite.h"

bool weather_fetch_now(const app_config_t *cfg, weather_data_t *o)
{
    if (!cfg->qweather_host[0] || !cfg->qweather_key[0]) return false;

    char url[256];
    snprintf(url, sizeof(url),
        "https://%s/v7/weather/now?location=%.4f,%.4f&key=%s",
        cfg->qweather_host, cfg->longitude, cfg->latitude, cfg->qweather_key);

    WiFiClientSecure client;
    client.setInsecure();               /* 简化：不校验证书（可换根证书） */
    HTTPClient http;
    if (!http.begin(client, url)) return false;
    int code = http.GET();
    if (code != 200) { http.end(); return false; }
    String body = http.getString();
    http.end();

    static char raw[2048];
    int n = (int)body.length();
    if (n <= 0 || n >= (int)sizeof(raw)) return false;
    memcpy(raw, body.c_str(), (size_t)n); raw[n] = '\0';

    /* 和风无条件 gzip（连 Accept-Encoding: identity 都不理），嗅探命中即解压 */
    static char json[4096];             /* now 接口解压后约 1KB */
    const char *p = raw;
    if (gzip_inflate(raw, n, json, (int)sizeof(json)) > 0)
        p = json;
    return weather_parse_now(p, o);
}
#else
/* ======================= PC：用 curl 联调 ======================= */
bool weather_fetch_now(const app_config_t *cfg, weather_data_t *o)
{
    if (!cfg->qweather_host[0] || !cfg->qweather_key[0]) return false;
    char cmd[512];
    /* --compressed：和风无条件 gzip，让 curl 自动解压（设备端用 miniz 同样处理）。
     * 临时文件放当前目录：system() 在 Windows 下走 cmd.exe，/tmp 不存在（Git Bash 才有） */
    snprintf(cmd, sizeof(cmd),
        "curl -s --compressed --max-time 12 \"https://%s/v7/weather/now?location=%.4f,%.4f&key=%s\" -o qw.json",
        cfg->qweather_host, cfg->longitude, cfg->latitude, cfg->qweather_key);
    if (system(cmd) != 0) return false;
    FILE *f = fopen("qw.json", "rb");
    if (!f) return false;
    static char buf[2048];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f); buf[n] = '\0'; fclose(f);
    return weather_parse_now(buf, o);
}
#endif
