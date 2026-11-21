/**
 * data_board.c —— 飞常准 MCP 客户端（航班动态页数据源）
 *
 * 为什么走 MCP 而不是旧 REST：飞常准个人 Key（sk-user-...）开放的是
 * MCP 端点（c-gw.variflight.com/chat_message/mcp/api），JSON-RPC over HTTPS，
 * Bearer 认证 + Mcp-Session-Id 会话。响应可能是纯 JSON 或 SSE 帧
 * （event:message\ndata:{...}），解析时统一取首个 "data:" 之后的部分。
 *
 * 计费约束：每次 tools/call 都计费（试用额度），所以：
 *   - 机场探测（getAirportByLatLng）结果按坐标缓存，坐标不变不重探
 *   - 刷新间隔由调用方控制（main 里 10 分钟）
 *   - 出发大屏一次拉全天（400+ 条），本地过滤"未来出发"前 10 条
 */
#include "data_board.h"
#include "data_time.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define VF_GATEWAY "https://c-gw.variflight.com/chat_message/mcp/api"

#ifdef ESP_PLATFORM
/* 设备端：net/http_get.cpp 导出的 POST（Bearer + 会话头 + 状态码） */
#include <esp_heap_caps.h>
#include "gzip_lite.h"
extern char *http_post_json_ex(const char *url, const char *bearer,
                               const char *json_body, const char *sid_in,
                               char *sid_out, int sid_cap,
                               int *out_len, int *out_code);
#else
/* PC 端：curl POST（body/响应/响应头落临时文件，绕开 cmd.exe 引号地狱） */
#define VF_RESP_CAP (512 * 1024)
static char *s_resp = NULL;

static int pc_post(const char *bearer, const char *body,
                   char *sid, int sid_cap, int *out_code)
{
    if (!s_resp) s_resp = (char *)malloc(VF_RESP_CAP);
    if (!s_resp) return -1;
    FILE *f = fopen("vf_body.json", "wb");
    if (!f) return -1;
    fputs(body, f);
    fclose(f);

    char cmd[768];
    if (sid[0]) {
        snprintf(cmd, sizeof(cmd),
            "curl -s --compressed --max-time 30 -D vf_hdr.txt -o vf_resp.txt -X POST "
            "-H \"Content-Type: application/json\" "
            "-H \"Accept: application/json, text/event-stream\" "
            "-H \"Authorization: Bearer %s\" "
            "-H \"Mcp-Session-Id: %s\" "
            "--data @vf_body.json \"" VF_GATEWAY "\"",
            bearer, sid);
    } else {
        snprintf(cmd, sizeof(cmd),
            "curl -s --compressed --max-time 30 -D vf_hdr.txt -o vf_resp.txt -X POST "
            "-H \"Content-Type: application/json\" "
            "-H \"Accept: application/json, text/event-stream\" "
            "-H \"Authorization: Bearer %s\" "
            "--data @vf_body.json \"" VF_GATEWAY "\"",
            bearer);
    }
    if (system(cmd) != 0) { *out_code = -1; return -1; }
    *out_code = 0;

    /* 从响应头文件里抓 Mcp-Session-Id（MinGW 用 _strnicmp） */
    f = fopen("vf_hdr.txt", "rb");
    if (f) {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            if (_strnicmp(line, "mcp-session-id:", 15) == 0) {
                char *v = line + 15;
                while (*v == ' ' || *v == '\t') v++;
                int i = 0;
                while (*v && *v != '\r' && *v != '\n' && i < sid_cap - 1)
                    sid[i++] = *v++;
                sid[i] = '\0';
                break;
            }
        }
        fclose(f);
    }

    f = fopen("vf_resp.txt", "rb");
    if (!f) return -1;
    size_t n = fread(s_resp, 1, VF_RESP_CAP - 1, f);
    fclose(f);
    s_resp[n] = '\0';
    return (int)n;
}
#endif

/* ================= MCP 会话与调用 ================= */
static char s_sid[80];          /* 会话 ID：initialize 时下发，后续复用 */
static int  s_rpc_id = 0;

/* 拆掉 SSE 帧壳：取首个 "data:" 之后的 JSON；非 SSE 响应原样返回 */
static const char *strip_sse(const char *body)
{
    const char *p = strstr(body, "data:");
    if (!p) return body;
    p += 5;
    while (*p == ' ') p++;
    return p;
}

/* 从 JSON 里提取 "key":"value"（处理 \" 转义） */
static void json_str(const char *buf, const char *key, char *out, int cap)
{
    char pat[40];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char *p = strstr(buf, pat);
    out[0] = '\0';
    if (!p) return;
    p += strlen(pat);
    int i = 0;
    while (*p && *p != '"' && i < cap - 1) {
        if (*p == '\\' && p[1]) { out[i++] = p[1]; p += 2; }
        else out[i++] = *p++;
    }
    out[i] = '\0';
}

/* 执行一次 tools/call，返回响应体（缓冲在下次调用前有效） */
static const char *mcp_call(const char *bearer, const char *tool,
                            const char *args, int *out_code)
{
    char body[512];
    snprintf(body, sizeof(body),
        "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"%s\",\"arguments\":%s}}",
        ++s_rpc_id, tool, args);

#ifdef ESP_PLATFORM
    int len = 0;
    char *resp = http_post_json_ex(VF_GATEWAY, bearer, body, s_sid,
                                   s_sid, sizeof(s_sid), &len, out_code);
    if (!resp) return NULL;
    if (out_code && *out_code != 200 && resp[0]) {
        /* 诊断：打印网关报错原文前 200 字节（400/401/429 等原因一目了然） */
        printf("[board] HTTP %d 响应: %.200s\n", *out_code, resp);
    }
    /* 大响应可能被网关 gzip（HTTPClient 不自动解压），嗅探命中则解压到 PSRAM 暂存。
     * 原 static 160KB 数组会把 dram0_0_seg 撑爆（内部 RAM 只有 ~320KB），改一次性
     * PSRAM 懒分配，生命周期与原静态缓冲等价（不复用、不释放，调用方安全）。 */
    static char *inflated = NULL;
    if (!inflated) {
        inflated = (char *)heap_caps_malloc(160 * 1024, MALLOC_CAP_SPIRAM);
        if (!inflated) {
            printf("[board] PSRAM inflated buf alloc FAILED, 用原文\n");
        }
    }
    if (inflated && gzip_inflate(resp, len, inflated, 160 * 1024) > 0)
        return strip_sse(inflated);
    return strip_sse(resp);
#else
    int len = pc_post(bearer, body, s_sid, sizeof(s_sid), out_code);
    if (len < 0 || !s_resp) return NULL;
    return strip_sse(s_resp);
#endif
}

/* MCP initialize：拿会话 ID（不计费） */
static int mcp_init(const char *bearer)
{
    char body[320];
    snprintf(body, sizeof(body),
        "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"initialize\",\"params\":"
        "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{},"
        "\"clientInfo\":{\"name\":\"esp32-ppi-radar\",\"version\":\"1.0\"}}}",
        ++s_rpc_id);
    s_sid[0] = '\0';

#ifdef ESP_PLATFORM
    int len = 0, code = 0;
    char *resp = http_post_json_ex(VF_GATEWAY, bearer, body, NULL,
                                   s_sid, sizeof(s_sid), &len, &code);
    if (resp && s_sid[0]) {
        printf("[board] 会话建立 HTTP %d sid=%.8s...\n", code, s_sid);
    }
    if (!resp || s_sid[0] == '\0') return -1;
    return 0;
#else
    int code = 0;
    int len = pc_post(bearer, body, s_sid, sizeof(s_sid), &code);
    if (len < 0 || s_sid[0] == '\0') return -1;
    return 0;
#endif
}

/* ================= 机场自动选择（带缓存） ================= */
static char s_ap_code[BD_AP_MAX];       /* "SHA" */
static char s_ap_name[BD_APNAME_MAX];   /* "上海虹桥" */
static float s_cache_lat = -999, s_cache_lon = -999;

static int detect_airport(const app_config_t *cfg, int *code)
{
    /* 坐标没变就复用上次探测结果（省一次计费调用） */
    if (s_ap_code[0] &&
        fabsf(cfg->latitude - s_cache_lat) < 0.05f &&
        fabsf(cfg->longitude - s_cache_lon) < 0.05f)
        return 1;

    char args[96];
    snprintf(args, sizeof(args), "{\"lat\":%.4f,\"lng\":%.4f}",
             cfg->latitude, cfg->longitude);
    const char *resp = mcp_call(cfg->vf_key, "getAirportByLatLng", args, code);
    if (!resp) return 0;

    char code_s[BD_AP_MAX], name_s[BD_APNAME_MAX];
    json_str(resp, "airportCode", code_s, sizeof(code_s));
    json_str(resp, "airportName", name_s, sizeof(name_s));
    if (!code_s[0]) return 0;

    strcpy(s_ap_code, code_s);
    strcpy(s_ap_name, name_s);
    s_cache_lat = cfg->latitude;
    s_cache_lon = cfg->longitude;
    return 1;
}

/* ================= 条目挑选 ================= */
/* hh:mm → 分钟；失败返回 -1 */
static int hm_to_mins(const char *s)
{
    int h = 0, m = 0;
    if (sscanf(s, "%d:%d", &h, &m) != 2) return -1;
    if (h < 0 || h > 23 || m < 0 || m > 59) return -1;
    return h * 60 + m;
}

/* 从一个条目对象里提取字段；返回 0=有效 */
static int parse_one(const char *obj, board_item_t *it, const char *today)
{
    memset(it, 0, sizeof(*it));
    json_str(obj, "flightNumber", it->fnum, sizeof(it->fnum));
    if (!it->fnum[0]) return -1;
    json_str(obj, "arrival",      it->dest,   sizeof(it->dest));
    if (!it->dest[0]) json_str(obj, "arrivalCode", it->dest, sizeof(it->dest));
    json_str(obj, "flightStatus", it->status, sizeof(it->status));
    json_str(obj, "delayData",    it->delay,  sizeof(it->delay));
    json_str(obj, "depPlanHour",  it->plan,   sizeof(it->plan));
    json_str(obj, "depEstHour",   it->est,    sizeof(it->est));
    json_str(obj, "depTerminal",  it->term,   sizeof(it->term));
    it->mins = hm_to_mins(it->plan);

    /* 只保留今天、计划时刻已知的航班 */
    char depdate[16];
    json_str(obj, "depDate", depdate, sizeof(depdate));
    if (depdate[0] && strcmp(depdate, today) != 0) return -1;
    if (it->mins < 0) return -1;
    return 0;
}

/* 遍历 structuredContent.items 数组，对每个顶层对象调用 fn */
static void for_each_item(const char *json,
                          void (*fn)(board_list_t *, const board_item_t *,
                                     board_list_t *),
                          board_list_t *out, board_list_t *scratch,
                          const char *today)
{
    const char *arr = strstr(json, "\"items\":[");
    if (!arr) return;
    arr += strlen("\"items\":[");

    const char *p = arr;
    while (*p && *p != ']') {
        while (*p && *p != '{' && *p != ']') p++;
        if (*p != '{') break;
        const char *obj = p;
        int depth = 0;
        while (*p) {
            if (*p == '{') depth++;
            else if (*p == '}') { depth--; if (depth == 0) { p++; break; } }
            p++;
        }
        if (depth != 0) break;

        board_item_t it;
        if (parse_one(obj, &it, today) == 0)
            fn(out, &it, scratch);
    }
}

/* 模式一：保留最早出发的 BD_MAX 条（未来出发优先，按分钟升序） */
static void pick_upcoming(board_list_t *out, const board_item_t *cand,
                          board_list_t *scratch)
{
    (void)scratch;
    if (cand->mins < out->cutoff_mins) return;           /* 已起飞一段时间 */
    if (out->count >= BD_MAX) return;                    /* 已保留的更早，直接丢弃 */
    int pos = out->count;
    for (int i = 0; i < out->count; i++) {
        if (cand->mins < out->items[i].mins) { pos = i; break; }
    }
    for (int i = out->count; i > pos; i--) out->items[i] = out->items[i - 1];
    out->items[pos] = *cand;
    out->count++;
}

/* 模式二（深夜兜底）：全天已起飞 → 保留最晚起飞的 BD_MAX 条（降序） */
static void pick_latest(board_list_t *out, const board_item_t *cand,
                        board_list_t *scratch)
{
    (void)scratch;
    if (out->count >= BD_MAX && cand->mins <= out->items[BD_MAX - 1].mins)
        return;
    int pos = out->count;
    for (int i = 0; i < out->count; i++) {
        if (cand->mins > out->items[i].mins) { pos = i; break; }
    }
    if (pos >= BD_MAX) return;
    if (out->count >= BD_MAX) out->count = BD_MAX - 1;   /* 挤掉最晚位 */
    for (int i = out->count; i > pos; i--) out->items[i] = out->items[i - 1];
    out->items[pos] = *cand;
    out->count++;
}

/* ================= 对外接口 ================= */
/* 网关额度/积分不足：返回 true，调用方应暂停一段时间再试 */
static bool quota_error(const char *resp)
{
    return resp && (strstr(resp, "积分") || strstr(resp, "额度") || strstr(resp, "quota"));
}

int board_fetch(const app_config_t *cfg, board_list_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!cfg || !cfg->vf_key[0]) return -1;          /* 未配 Key */

    /* 1. MCP 会话（断线/Key 失效自动重建） */
    if (s_sid[0] == '\0' && mcp_init(cfg->vf_key) != 0) return -1;

    /* 2. 机场探测（缓存命中不花钱） */
    int code = 0;
    if (!detect_airport(cfg, &code)) {
        if (code == 401 || code == 403) { s_sid[0] = '\0'; return -1; }
        if (!s_ap_code[0]) {                          /* 探测失败兜底 */
            strcpy(s_ap_code, "SHA");
            strcpy(s_ap_name, "上海虹桥");
        }
    }
    strcpy(out->airport, s_ap_code);
    strcpy(out->airport_name, s_ap_name);

    /* 3. 出发大屏（一次拉全天，本地过滤） */
    char args[80];
    snprintf(args, sizeof(args), "{\"local\":\"%s\",\"out\":0}", s_ap_code);
    const char *resp = mcp_call(cfg->vf_key, "getAirportFlightBoard", args, &code);
    if (!resp) {
        if (code == 401 || code == 403) s_sid[0] = '\0';   /* 下次重建会话 */
        return -1;
    }

    /* 4. 当前时刻与今天日期（本地时间：设备 NTP / PC 系统） */
    weather_data_t t;
    memset(&t, 0, sizeof(t));
    time_now(&t);
    char today[16];
    snprintf(today, sizeof(today), "%04d-%02d-%02d", t.year, t.mon, t.day);

    /* 第一遍：今天尚未出发（或刚出发 30 分钟内）的最早 10 条 */
    out->cutoff_mins = t.hour * 60 + t.min - 30;
    for_each_item(resp, pick_upcoming, out, NULL, today);

    /* 深夜兜底：全天已起飞 → 改收最近才起飞的 10 条，画面不空 */
    if (out->count == 0) {
        out->cutoff_mins = -1;
        for_each_item(resp, pick_latest, out, NULL, today);
        /* 降序转升序，显示顺序一致 */
        for (int i = 0; i < out->count / 2; i++) {
            board_item_t tmp = out->items[i];
            out->items[i] = out->items[out->count - 1 - i];
            out->items[out->count - 1 - i] = tmp;
        }
    }

    out->valid = out->count > 0;
    if (out->count == 0 && quota_error(resp)) return -2;   /* 额度不足 */
    return out->count;
}

/* ================= 雷达实时机群（在飞航班位置） ================= */

/* 读 JSON 里 "key":数字（兼容引号包裹、负数、小数） */
static int json_num_d(const char *obj, const char *key, double *out)
{
    char pat[40];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(obj, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '"') p++;
    char *endp = NULL;
    double v = strtod(p, &endp);
    if (endp == p) return 0;
    *out = v;
    return 1;
}

/* 定位 "key":[ 后逐个顶层对象回调 cb */
static void scan_array(const char *json, const char *arr_key,
                       void (*cb)(const char *obj, void *ctx), void *ctx)
{
    char pat[24];
    snprintf(pat, sizeof(pat), "\"%s\":[", arr_key);
    const char *p = strstr(json, pat);
    if (!p) return;
    p += strlen(pat);
    while (*p && *p != ']') {
        while (*p && *p != '{' && *p != ']') p++;
        if (*p != '{') break;
        const char *obj = p;
        int depth = 0;
        while (*p) {
            if (*p == '{') depth++;
            else if (*p == '}') { depth--; if (depth == 0) { p++; break; } }
            p++;
        }
        if (depth != 0) break;
        cb(obj, ctx);
    }
}

typedef struct {
    board_inflight_t *out;
    int n, max, now_mins;
} infl_ctx_t;

static void infl_cb(const char *obj, void *vctx)
{
    infl_ctx_t *c = (infl_ctx_t *)vctx;
    if (c->n >= c->max) return;

    char st[24] = "";
    json_str(obj, "flightStatus", st, sizeof(st));
    if (!strstr(st, "起飞")) return;              /* 只要已在空中的 */

    char plan[8] = "";
    json_str(obj, "arrPlanHour", plan, sizeof(plan));
    int m = hm_to_mins(plan);
    if (m < 0) return;                            /* 无计划到达时刻，无法判断远近 */
    if (m - c->now_mins < -20 || m - c->now_mins > 45) return;

    board_inflight_t *it = &c->out[c->n];
    memset(it, 0, sizeof(*it));
    json_str(obj, "flightNumber", it->fnum, sizeof(it->fnum));
    json_str(obj, "departureCode", it->dep, sizeof(it->dep));
    json_str(obj, "arrivalCode",  it->arr, sizeof(it->arr));
    json_str(obj, "depDate",      it->date, sizeof(it->date));
    snprintf(it->plan, sizeof(it->plan), "%s", plan);
    if (it->fnum[0] && it->dep[0] && it->arr[0] && it->date[0])
        c->n++;
}

int board_fetch_inflight(const app_config_t *cfg, board_inflight_t *out, int max)
{
    if (!cfg || !cfg->vf_key[0] || !out || max <= 0) return -1;
    if (s_sid[0] == '\0' && mcp_init(cfg->vf_key) != 0) return -1;

    int code = 0;
    if (!s_ap_code[0]) detect_airport(cfg, &code);
    const char *ap = s_ap_code[0] ? s_ap_code : "SHA";

    char args[64];
    snprintf(args, sizeof(args), "{\"local\":\"%s\",\"out\":1}", ap);
    const char *resp = mcp_call(cfg->vf_key, "getAirportFlightBoard", args, &code);
    if (!resp) return -1;

    weather_data_t t;
    memset(&t, 0, sizeof(t));
    time_now(&t);

    infl_ctx_t c = { out, 0, max, t.hour * 60 + t.min };
    scan_array(resp, "items", infl_cb, &c);
    if (c.n == 0 && quota_error(resp)) return -2;          /* 额度不足 */
    return c.n;
}

typedef struct {
    int found;
    double best_t;
    float lat, lng, alt, spd, hdg, vs;
} track_ctx_t;

static void trk_cb(const char *obj, void *vctx)
{
    track_ctx_t *c = (track_ctx_t *)vctx;
    double t = 0, lat = 0, lng = 0, alt = 0, spd = 0, hdg = 0, vs = 0;
    if (!json_num_d(obj, "updatetime", &t)) return;
    if (!json_num_d(obj, "lat", &lat) || !json_num_d(obj, "lng", &lng)) return;
    json_num_d(obj, "height", &alt);
    json_num_d(obj, "speed",  &spd);
    json_num_d(obj, "angle",  &hdg);
    json_num_d(obj, "vspeed", &vs);
    if (!c->found || t >= c->best_t) {
        c->found = 1;
        c->best_t = t;
        c->lat = (float)lat; c->lng = (float)lng;
        c->alt = (float)alt; c->spd = (float)spd;
        c->hdg = (float)hdg; c->vs  = (float)vs;
    }
}

int board_fetch_track(const app_config_t *cfg, const char *fnum,
                      const char *dep, const char *arr, const char *date,
                      float *lat, float *lng, float *alt_m,
                      float *spd_ms, float *hdg, float *vs_ms)
{
    if (!cfg || !cfg->vf_key[0] || !fnum || !fnum[0]) return -1;
    if (s_sid[0] == '\0' && mcp_init(cfg->vf_key) != 0) return -1;

    char args[192];
    snprintf(args, sizeof(args),
             "{\"fnum\":\"%s\",\"dep\":\"%s\",\"arr\":\"%s\",\"date\":\"%s\",\"isFull\":1}",
             fnum, dep, arr, date);
    int code = 0;
    const char *resp = mcp_call(cfg->vf_key, "getFlightTrackDetail", args, &code);
    if (!resp) return -1;

    track_ctx_t c;
    memset(&c, 0, sizeof(c));
    scan_array(resp, "lines", trk_cb, &c);
    if (!c.found) return quota_error(resp) ? -2 : 0;

    if (lat) *lat = c.lat;
    if (lng) *lng = c.lng;
    if (alt_m) *alt_m = c.alt;
    if (spd_ms) *spd_ms = c.spd;
    if (hdg) *hdg = c.hdg;
    if (vs_ms) *vs_ms = c.vs;
    return 1;
}
