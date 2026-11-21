/**
 * data_flight_net.c —— 真实航班数据：抓取 + 轻量 JSON 解析
 *
 * 不引入 ArduinoJson / cJSON：ADS-B 返回的字段结构固定，
 * 用「定位 key → 取值」的扫描器即可，代码小、内存零分配、解析 300KB 仅几毫秒。
 *
 * 平台差异只在于 HTTP 抓取：
 *   PC    : system("curl ...")，方便联调
 *   ESP32 : 调用 net/http_get.cpp 导出的 http_get_psram()
 */
#include "data_flight_net.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979
#endif

#define BODY_CAP   (384 * 1024)

#ifdef ESP_PLATFORM
#include <esp_timer.h>
extern char *http_get_psram(const char *url, int *out_len);
extern char *http_get_psram_ex(const char *url, int *out_len, int *out_code);
#endif

/* 单调毫秒时钟（源健康记忆用；uint32 回绕安全，比较用 (int32_t) 差值） */
static uint32_t mono_ms(void)
{
#ifdef ESP_PLATFORM
    return (uint32_t)(esp_timer_get_time() / 1000);
#else
    return (uint32_t)clock();     /* PC 联调够用：进程内单调递增 */
#endif
}

/* ================= 源健康记忆 =================
 * 实测（2026-11）：
 *   · adsb.lol 部分网络被 Cloudflare bot 拦（连接失败或 403）
 *   · airplanes.live 对新客户端 403，需邮件申请
 *   · 部分限流器返回 HTTP 200 + "You have been rate limited" 文本（不是 429）
 * 被拒的源暂停一段时间再试，避免每个轮询周期都浪费请求：
 *   403 → 1 小时（多半要人工申请，但申请通过后能自动恢复）
 *   429 / 限流文本 → 5 分钟
 *   网络失败（连接层）→ 不暂停（可能是 WiFi 瞬断） */
#define HOST_N              3
#define HOST_SUSPEND_403_MS (3600UL * 1000UL)
#define HOST_SUSPEND_429_MS (5UL * 60UL * 1000UL)
static uint32_t s_host_until[HOST_N] = { 0, 0, 0 };   /* 与 FMT 下标对应 */

/* OpenSky 429 冷却：配额耗尽后 15 分钟内不再请求 */
#define OS_COOLDOWN_MS (15UL * 60UL * 1000UL)
static uint32_t s_os_cooldown_until = 0;
/* 合并链里的 OpenSky 节流：匿名配额 400/天，20s 轮询烧不起，
 * 每 5 分钟补采一次（288 次/天）。初值取负，让开机第一轮就尝试 */
#define OS_CHAIN_INTERVAL_MS (5UL * 60UL * 1000UL)
static uint32_t s_os_last_try = (uint32_t)-OS_CHAIN_INTERVAL_MS;

/* ================= 几何：经纬度 → 距离/方位 ================= */
/* 120NM 量级用平面近似，误差 < 0.5%，与雷达显示精度匹配 */
static void geo_polar(float clat, float clon, float lat, float lon,
                      float *dist_nm, float *brg)
{
    double dlat = ((double)lat - (double)clat) * 60.0;
    double dlon = ((double)lon - (double)clon) * 60.0 * cos((double)clat * M_PI / 180.0);
    *dist_nm = (float)sqrt(dlat * dlat + dlon * dlon);
    double b = atan2(dlon, dlat) * 180.0 / M_PI;
    if (b < 0) b += 360.0;
    *brg = (float)b;
}

/* ================= 极简 JSON ================= */
static const char *jfind(const char *b, const char *e, const char *key)
{
    size_t kl = strlen(key);
    for (const char *p = b; p + kl + 2 < e; p++) {
        if (*p != '"') continue;
        if (strncmp(p + 1, key, kl) != 0) continue;
        if (p[1 + kl] != '"') continue;
        const char *q = p + 2 + kl;
        while (q < e && (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')) q++;
        if (q < e && *q == ':') return q + 1;
    }
    return NULL;
}

static bool jnum(const char *b, const char *e, const char *key, double *out)
{
    const char *v = jfind(b, e, key);
    if (!v) return false;
    while (v < e && (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r')) v++;
    if (v >= e || *v == '"' || *v == 'n' || *v == 'N') return false;
    char *end = NULL;
    double d = strtod(v, &end);
    if (end == v) return false;
    *out = d;
    return true;
}

static bool jstr(const char *b, const char *e, const char *key, char *out, int cap)
{
    const char *v = jfind(b, e, key);
    if (!v) return false;
    while (v < e && *v != '"' && *v != ',' && *v != '}') v++;
    if (v >= e || *v != '"') return false;
    v++;
    int i = 0;
    while (v < e && *v != '"' && i < cap - 1) out[i++] = *v++;
    out[i] = 0;
    while (i > 0 && out[i - 1] == ' ') out[--i] = 0;   /* ADS-B 呼号右补空格 */
    return i > 0;
}

/* ================= 目标入表（按距离保留最近的 FL_MAX 个） ================= */
static void push_target(flight_list_t *o, float lat, float lon, float alt_ft,
                        float spd_kt, float hdg, float vs_fpm,
                        const char *cs, const char *type,
                        const char *icao, const char *squawk)
{
    if (spd_kt < 30.0f) return;                 /* 地面/滑行目标不上 PPI */
    float d, b;
    geo_polar(o->center_lat, o->center_lon, lat, lon, &d, &b);
    if (d > (float)o->range_nm) return;

    int idx = o->count;
    if (idx >= FL_MAX) {
        /* 满了：找最远的替换掉 */
        int far = 0;
        for (int i = 1; i < FL_MAX; i++)
            if (o->items[i].dist_nm > o->items[far].dist_nm) far = i;
        if (d >= o->items[far].dist_nm) return;
        idx = far;
    } else {
        o->count++;
    }

    flight_t *f = &o->items[idx];
    memset(f, 0, sizeof(*f));
    f->hdg = hdg;
    f->spd = spd_kt;
    f->alt = (int)(alt_ft / 100.0f + 0.5f);
    f->vs = (int)vs_fpm;
    f->dist_nm = d;
    f->brg = b;
    f->active = 1;
    f->glow = 0.0f;
    snprintf(f->callsign, sizeof(f->callsign), "%s", cs && cs[0] ? cs : "--------");
    snprintf(f->type, sizeof(f->type), "%s", type ? type : "");
    snprintf(f->icao, sizeof(f->icao), "%s", icao ? icao : "");
    snprintf(f->squawk, sizeof(f->squawk), "%s", squawk ? squawk : "");
    f->is_ssr = (squawk && squawk[0]) || (cs && cs[0]) ? 1 : 0;
}

static void sort_by_dist(flight_list_t *o)
{
    for (int i = 1; i < o->count; i++) {
        flight_t t = o->items[i];
        int j = i - 1;
        while (j >= 0 && o->items[j].dist_nm > t.dist_nm) {
            o->items[j + 1] = o->items[j];
            j--;
        }
        o->items[j + 1] = t;
    }
}

/* ================= airplanes.live / adsb.fi ================= */
int flight_parse_adsb_json(const char *json, float clat, float clon, int range_nm,
                           flight_list_t *out)
{
    if (!json || !out) return 0;
    memset(out, 0, sizeof(*out));
    out->center_lat = clat;
    out->center_lon = clon;
    out->range_nm = range_nm;

    const char *a = strstr(json, "\"ac\"");
    if (!a) a = strstr(json, "\"aircraft\"");
    if (!a) return 0;
    const char *p = strchr(a, '[');
    if (!p) return 0;
    p++;

    char cs[16] = {0}, tp[16] = {0}, hex[12] = {0}, sq[8] = {0};
    double lat, lon, alt, gs, trk, vr;

    while (*p && *p != ']') {
        const char *ob = strchr(p, '{');
        if (!ob) break;
        const char *oe = ob;
        int depth = 0;
        while (*oe) {
            if (*oe == '{') depth++;
            else if (*oe == '}') { if (--depth == 0) break; }
            oe++;
        }
        if (!*oe) break;

        cs[0] = tp[0] = hex[0] = sq[0] = 0;
        bool ok = jnum(ob, oe, "lat", &lat) && jnum(ob, oe, "lon", &lon);
        if (ok) {
            if (!jnum(ob, oe, "alt_baro", &alt)) alt = 0;         /* "ground" 会失败 */
            if (!jnum(ob, oe, "gs", &gs))       gs = 0;
            if (!jnum(ob, oe, "track", &trk))   trk = 0;
            if (!jnum(ob, oe, "baro_rate", &vr)) vr = 0;
            jstr(ob, oe, "flight", cs, sizeof(cs));
            jstr(ob, oe, "t", tp, sizeof(tp));
            jstr(ob, oe, "hex", hex, sizeof(hex));
            jstr(ob, oe, "squawk", sq, sizeof(sq));
            /* 有的接收站把机型放在 "type" 且值为 adsb_icao，过滤掉 */
            if (strncmp(tp, "adsb", 4) == 0) tp[0] = 0;
            if (alt > 100) push_target(out, (float)lat, (float)lon, (float)alt,
                                       (float)gs, (float)trk, (float)vr,
                                       cs, tp, hex, sq);
        }
        p = oe + 1;
    }
    sort_by_dist(out);
    out->valid = out->count > 0;
    out->src = 1;
    return out->count;
}

/* ================= OpenSky Network ================= */
/* states 每个元素是 17 项数组，字段位置固定 */
static int split_tokens(const char *b, const char *e, char tok[20][24], int max)
{
    int n = 0;
    const char *p = b;
    bool inq = false;
    while (p < e && n < max) {
        while (p < e && (*p == ' ' || *p == ',' || *p == '[')) p++;
        if (p >= e) break;
        const char *s = p;
        inq = false;
        while (p < e && (inq || (*p != ',' && *p != ']'))) {
            if (*p == '"') inq = !inq;
            p++;
        }
        int len = (int)(p - s);
        if (len >= 24) len = 23;
        memcpy(tok[n], s, len);
        tok[n][len] = 0;
        /* 去引号 */
        char *q = tok[n];
        while (*q == '"') q++;
        int l = (int)strlen(q);
        while (l > 0 && q[l - 1] == '"') q[--l] = 0;
        if (q != tok[n]) memmove(tok[n], q, l + 1);
        n++;
    }
    return n;
}

int flight_parse_opensky_json(const char *json, float clat, float clon, int range_nm,
                              flight_list_t *out)
{
    if (!json || !out) return 0;
    memset(out, 0, sizeof(*out));
    out->center_lat = clat;
    out->center_lon = clon;
    out->range_nm = range_nm;

    const char *a = strstr(json, "\"states\"");
    if (!a) return 0;
    const char *p = strchr(a, '[');
    if (!p) return 0;
    p++;

    char tok[20][24];
    char sq[8];
    while (*p && *p != ']') {
        const char *ob = strchr(p, '[');
        if (!ob) break;
        const char *oe = ob;
        int depth = 0;
        while (*oe) {
            if (*oe == '[') depth++;
            else if (*oe == ']') { if (--depth == 0) break; }
            oe++;
        }
        if (!*oe) break;

        int n = split_tokens(ob, oe, tok, 20);
        if (n >= 15) {
            const char *hex = tok[0], *cs = tok[1];
            bool has_pos = strcmp(tok[5], "null") != 0 && strcmp(tok[6], "null") != 0;
            bool on_ground = (strcmp(tok[8], "true") == 0);
            if (has_pos && !on_ground) {
                double lat = atof(tok[6]), lon = atof(tok[5]);
                double alt_m = strcmp(tok[7], "null") ? atof(tok[7]) : 0;
                double vel   = strcmp(tok[9], "null") ? atof(tok[9]) : 0;
                double trk   = strcmp(tok[10], "null") ? atof(tok[10]) : 0;
                double vr    = strcmp(tok[11], "null") ? atof(tok[11]) : 0;
                sq[0] = 0;
                if (n > 14 && strcmp(tok[14], "null") != 0)
                    snprintf(sq, sizeof(sq), "%s", tok[14]);
                push_target(out, (float)lat, (float)lon,
                            (float)(alt_m * 3.28084),      /* m → ft */
                            (float)(vel * 1.94384),        /* m/s → kt */
                            (float)trk,
                            (float)(vr * 196.85),          /* m/s → ft/min */
                            cs, "", hex, sq);
            }
        }
        p = oe + 1;
    }
    sort_by_dist(out);
    out->valid = out->count > 0;
    out->src = 1;
    return out->count;
}

/* ================= 抓取 ================= */
#ifndef ESP_PLATFORM
/* PC：curl 落地再读回（联调方便，也能验证解析）
 * Windows 陷阱：system() 走 cmd.exe——单引号是普通字符，URL 里的 & 会被
 * 当成命令分隔符把 URL 拆碎；必须用双引号包 URL，重定向也要用 NUL。
 * 临时文件同理不能写 /tmp（MSYS 专属路径），改写当前目录。 */
#if defined(_WIN32)
#define CURL_TMP   "radar_adsb.tmp.json"
#define CURL_DEVNULL "2>NUL"
#else
#define CURL_TMP   "/tmp/radar_adsb.json"
#define CURL_DEVNULL "2>/dev/null"
#endif
static int curl_get(const char *url, char *buf, int cap)
{
    char cmd[768];
    snprintf(cmd, sizeof(cmd),
             "curl -sf --compressed --max-time 12 -A \"esp32-ppi-radar/1.0\" -o \"" CURL_TMP "\" \"%s\" " CURL_DEVNULL,
             url);
    if (system(cmd) != 0) return -1;
    FILE *f = fopen(CURL_TMP, "rb");
    if (!f) return -1;
    size_t n = fread(buf, 1, (size_t)cap - 1, f);
    fclose(f);
    buf[n] = 0;
    return (int)n;
}
static char *fetch_body(const char *url, int *len)
{
    static char *body = NULL;
    if (!body) body = (char *)malloc(BODY_CAP);
    if (!body) return NULL;
    *len = curl_get(url, body, BODY_CAP);
    return *len > 0 ? body : NULL;
}
static char *fetch_body_status(const char *url, int *len, int *code)
{
    /* PC 端拿不到 HTTP 状态码（curl -f 非 2xx 直接失败），按未知处理 */
    *code = 0;
    return fetch_body(url, len);
}
#else
/* 设备：net/http_get.cpp 直接返回 PSRAM 里的大缓冲 */
static char *fetch_body(const char *url, int *len)
{
    return http_get_psram(url, len);
}
static char *fetch_body_status(const char *url, int *len, int *code)
{
    return http_get_psram_ex(url, len, code);
}
#endif

int flight_net_fetch_host(float clat, float clon, int range_nm, int src,
                          const char *adsb_host, flight_list_t *out);

int flight_net_fetch(float clat, float clon, int range_nm, int src, flight_list_t *out)
{
    return flight_net_fetch_host(clat, clon, range_nm, src, NULL, out);
}

/* 把 s 并进 dst：按 ICAO24 去重，dst 缺失的字段用后来者补齐。
 * 各公益源的馈站不同，同一架飞机可能只被其中一部分看到——
 * 合并而不是"命中即返回"，能把聚合源的覆盖榨干。 */
static void merge_one(flight_list_t *dst, const flight_t *s)
{
    for (int i = 0; i < dst->count; i++) {
        flight_t *d = &dst->items[i];
        if (d->icao[0] && s->icao[0] && strcmp(d->icao, s->icao) == 0) {
            if (!d->callsign[0] && s->callsign[0]) { strncpy(d->callsign, s->callsign, sizeof(d->callsign) - 1); }
            if (!d->type[0] && s->type[0])         { strncpy(d->type, s->type, sizeof(d->type) - 1); }
            if (!d->squawk[0] && s->squawk[0])     { strncpy(d->squawk, s->squawk, sizeof(d->squawk) - 1); d->is_ssr = s->is_ssr; }
            if (d->alt == 0 && s->alt)   d->alt = s->alt;
            if (d->spd == 0 && s->spd)   d->spd = s->spd;
            if (d->hdg == 0 && s->hdg)   d->hdg = s->hdg;
            return;
        }
    }
    if (dst->count < FL_MAX) dst->items[dst->count++] = *s;
}

int flight_net_fetch_host(float clat, float clon, int range_nm, int src,
                          const char *adsb_host, flight_list_t *out)
{
    char url[320];
    int n = 0;

    /* 局域网 dump1090（自己架 RTL-SDR，覆盖和刷新率都最好，且不走公网） */
    if (src == FL_SRC_LOCAL) {
        if (!adsb_host || !adsb_host[0]) return 0;
        snprintf(url, sizeof(url), "http://%s/data/aircraft.json", adsb_host);
        int len = 0;
        char *body = fetch_body(url, &len);
        if (!body) return 0;
        return flight_parse_adsb_json(body, clat, clon, range_nm, out);
    }

    if (src == FL_SRC_OPENSKY) {
        /* 429 冷却期内直接返回 0：配额耗尽时不再白打请求，
         * net_task 通过 flight_net_opensky_extra_wait() 自动放宽轮询间隔 */
        if ((int32_t)(s_os_cooldown_until - mono_ms()) > 0) return 0;

        double dlat = range_nm / 60.0;
        double dlon = range_nm / (60.0 * cos((double)clat * M_PI / 180.0) + 1e-6);
        snprintf(url, sizeof(url),
            "https://opensky-network.org/api/states/all?lamin=%.4f&lomin=%.4f&lamax=%.4f&lomax=%.4f",
            clat - dlat, clon - dlon, clat + dlat, clon + dlon);
        int len = 0, code = 0;
        char *body = fetch_body_status(url, &len, &code);
        if (code == 429) {
            s_os_cooldown_until = mono_ms() + OS_COOLDOWN_MS;
            return 0;
        }
        s_os_cooldown_until = 0;    /* 成功拿到响应，冷却解除 */
        if (body) n = flight_parse_opensky_json(body, clat, clon, range_nm, out);
        return n;
    }

    /* FL_SRC_ADSB：多源合并（带机型信息，延迟 1s 级）
     *   adsb.fi 与 adsb.lol/airplanes.live 是两种 URL 形状（后者用 /v2/point）。
     *   各源馈站不同，全部请求后按 ICAO24 去重合并；被拒源按健康记忆跳过
     *   （403 停 1h / 429 停 5min）。合并后仍为空自动退 OpenSky 兜底。 */
    static const char *FMT[HOST_N] = {
        "https://opendata.adsb.fi/api/v2/lat/%.4f/lon/%.4f/dist/%d",
        "https://api.adsb.lol/v2/point/%.4f/%.4f/%d",
        "https://api.airplanes.live/v2/point/%.4f/%.4f/%d",
    };
    uint32_t now_ms = mono_ms();
    memset(out, 0, sizeof(*out));
    out->range_nm = range_nm;
    out->center_lat = clat;
    out->center_lon = clon;
    for (int h = 0; h < HOST_N; h++) {
        if ((int32_t)(s_host_until[h] - now_ms) > 0) continue;   /* 暂停中的源跳过 */
        snprintf(url, sizeof(url), FMT[h], clat, clon, range_nm);
        int len = 0, code = 0;
        char *body = fetch_body_status(url, &len, &code);
        if (body) {
            flight_list_t tmp;
            int k = flight_parse_adsb_json(body, clat, clon, range_nm, &tmp);
            for (int i = 0; i < k; i++) merge_one(out, &tmp.items[i]);
        }
        /* 失败分类：只有"明确被拒"才暂停，网络瞬断不暂停 */
        int kind = 0;                        /* 0=未知/网络失败 1=403 2=429/限流 */
        if (code == 403) kind = 1;
        else if (code == 429) kind = 2;
        else if (body && strstr(body, "rate limited")) kind = 2; /* 200+限流文本的坑 */
        if (kind == 1)      s_host_until[h] = now_ms + HOST_SUSPEND_403_MS;
        else if (kind == 2) s_host_until[h] = now_ms + HOST_SUSPEND_429_MS;
    }
    /* OpenSky 补充合并：实测馈站与上面三家互补（它们看不见的它看得见）。
     * 5 分钟节流，429 走 15min 冷却，配额 288 次/天 < 400 上限 */
    int os_tried = 0;
    if ((int32_t)(now_ms - s_os_last_try) >= (int32_t)OS_CHAIN_INTERVAL_MS) {
        s_os_last_try = now_ms;
        os_tried = 1;
        if ((int32_t)(s_os_cooldown_until - now_ms) <= 0) {
            double dlat = range_nm / 60.0;
            double dlon = range_nm / (60.0 * cos((double)clat * M_PI / 180.0) + 1e-6);
            snprintf(url, sizeof(url),
                "https://opensky-network.org/api/states/all?lamin=%.4f&lomin=%.4f&lamax=%.4f&lomax=%.4f",
                clat - dlat, clon - dlon, clat + dlat, clon + dlon);
            int len = 0, code = 0;
            char *body = fetch_body_status(url, &len, &code);
            if (code == 429) {
                s_os_cooldown_until = now_ms + OS_COOLDOWN_MS;
            } else {
                s_os_cooldown_until = 0;    /* 成功拿到响应，冷却解除 */
                if (body) {
                    flight_list_t tmp;
                    int k = flight_parse_opensky_json(body, clat, clon, range_nm, &tmp);
                    for (int i = 0; i < k; i++) merge_one(out, &tmp.items[i]);
                }
            }
        }
    }
    if (out->count > 0) {
        sort_by_dist(out);
        out->valid = 1;
        out->src = 1;
        return out->count;
    }
    /* 全空：直接返回 0（OpenSky 刚查过，不再重复打请求），
     * 画面由数据层的演示回退兜住，5 分钟后 OpenSky 补采自动重试 */
    (void)os_tried;
    return 0;
}

uint32_t flight_net_opensky_extra_wait(void)
{
    int32_t rem = (int32_t)(s_os_cooldown_until - mono_ms());
    return rem > 0 ? (uint32_t)rem : 0;
}
