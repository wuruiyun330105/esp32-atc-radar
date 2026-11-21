/**
 * ip_locate.cpp —— IP 级自动定位实现
 * 响应样例：{"status":"success","city":"上海","lat":31.2222,"lon":121.4581,...}
 */
#include "ip_locate.h"
#include <Arduino.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

extern "C" char *http_get_psram_ex(const char *url, int *out_len, int *out_code);

/* 从 JSON 里取 "key":数字 */
static int jnum(const char *buf, const char *key, double *out)
{
    char pat[24];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(buf, pat);
    if (!p) return 0;
    p += strlen(pat);
    char *endp = NULL;
    double v = strtod(p, &endp);
    if (endp == p) return 0;
    *out = v;
    return 1;
}

/* 从 JSON 里取 "key":"文本"（中文 UTF-8 原样拷贝） */
static void jstr(const char *buf, const char *key, char *out, int cap)
{
    char pat[24];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char *p = strstr(buf, pat);
    out[0] = '\0';
    if (!p) return;
    p += strlen(pat);
    int m = 0;
    while (*p && *p != '"' && m < cap - 1) out[m++] = *p++;
    out[m] = 0;
}

static char s_city[32] = "";

/* 最近一次 IP 定位到的城市；未定位过返回 "上海"（默认雷达中心） */
extern "C" const char *ip_locate_city(void)
{
    return s_city[0] ? s_city : "上海";
}

extern "C" int ip_locate(double *lat, double *lon, char *city, int cap)
{
    int len = 0, code = 0;
    char *buf = http_get_psram_ex(
        "http://ip-api.com/json/?fields=status,lat,lon,city&lang=zh-CN",
        &len, &code);
    if (!buf || code != 200 || !strstr(buf, "\"success\"")) {
        Serial.printf("[loc] IP 定位失败 http=%d\n", code);
        return 0;
    }
    double la = 0, lo = 0;
    if (!jnum(buf, "lat", &la) || !jnum(buf, "lon", &lo)) {
        Serial.println("[loc] IP 定位解析失败");
        return 0;
    }
    if (city && cap > 0) jstr(buf, "city", city, cap);
    snprintf(s_city, sizeof(s_city), "%s", (city && city[0]) ? city : "上海");
    if (lat) *lat = la;
    if (lon) *lon = lo;
    Serial.printf("[loc] IP 定位成功：%.4f,%.4f %s\n", la, lo, city ? city : "");
    return 1;
}
