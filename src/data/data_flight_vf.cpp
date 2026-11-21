/**
 * data_flight_vf.cpp —— 雷达实时机群（飞常准官方端点）
 *
 * 数据方案完全沿用原仓库 emwstudio/esp32-radar 的设计：
 *   ① searchFlightsByDepArr 按航线扫出"在飞"航班（拿到机尾号）
 *   ② getRealtimeLocationByAnum 按机尾号轮询实时位置（经纬度/高度/速度/航向）
 *
 * 成本控制（官方按调用计费）：
 *   · 航线扫描 15 分钟一轮（6 城市 × 2 方向 = 12 次调用）
 *   · 单机位置轮询最少间隔 150 秒，每次 fetch 最多查 5 架（轮流摊成本）
 *   · 两次位置刷新之间由 data_flight 的死推算外推，画面不跳
 */
#include "vf_client.h"
#include "data_flight_vf.h"
#include "data_time.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

#define SCAN_INTERVAL_MS (30UL * 60UL * 1000UL)   /* 每轮 12 次调用 ~1MB，30 分钟一轮控流量 */
#define POLL_MIN_MS      (150UL * 1000UL)
#define POLL_PER_FETCH   5
#define MAX_TRACK        16
#define MAX_CAND         64   /* 扫描候选上限（全量收集后再排序） */
#define N_CITIES         6

static const char *CITIES[N_CITIES] = { "CAN", "PEK", "SZX", "CKG", "CTU", "XIY" };
static const char *HUBS[2] = { "SHA", "PVG" };   /* 只用上海两场的进出港 */

struct VfTrack
{
    char anum[8];
    char fnum[12];
    char dep[5];
    char arr[5];
    bool pos_ok;
    VfPosition pos;
    uint32_t lastPoll;
};
static VfTrack s_tr[MAX_TRACK];
static int s_nTr = 0;
static uint32_t s_lastScan = 0;
static bool s_scanned = false;
static bool s_needFullPoll = false;     /* 扫描后先把全部飞机的位置查一遍 */
static bool s_quotaDead = false;        /* 服务端判额度不足 → 6 小时内不再重试 */
static char s_keyInited = 0;
static int  s_failStreak = 0;         /* 连续失败次数（扫描全挂） */
static uint32_t s_backoffUntil = 0;   /* 退避截止（毫秒） */

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

/* 航线扫描：发现"起飞"（在飞）航班，重建跟踪表 */
static bool scan_routes(void)
{
    weather_data_t t;
    memset(&t, 0, sizeof(t));
    time_now(&t);
    char date[12];
    snprintf(date, sizeof(date), "%04d-%02d-%02d", t.year, t.mon, t.day);

    /* 全量收集在飞航班（不限城市顺序），按"计划落地时间"升序后取前 MAX_TRACK——
     * 越早落地越靠近本场上空，避免被远航程航班把临近的挤掉 */
    struct Cand { VfTrack tr; int landMin; };
    static Cand cand[MAX_CAND];
    int nCand = 0;
    bool anyOk = false;

    for (int ci = 0; ci < N_CITIES; ci++) {
        for (int h = 0; h < 2; h++) {
            for (int dir = 0; dir < 2; dir++) {
            const char *dep = dir == 0 ? HUBS[h] : CITIES[ci];
            const char *arr = dir == 0 ? CITIES[ci] : HUBS[h];
            VfRouteFlight res[48];
            int cnt = 0;
            if (!vf_search_route(date, dep, arr, res, 48, &cnt)) {
                const char *err = vf_last_error();
                if (err && (strstr(err, "BALANCE") || strstr(err, "INSUFFICIENT") ||
                            strstr(err, "余额") || strstr(err, "积分"))) {
                    s_quotaDead = true;
                    Serial.println("[vf] 额度不足：6 小时内不再请求（去 ai.variflight.com 充值）");
                    return false;
                }
                vTaskDelay(pdMS_TO_TICKS(400));
                continue;
            }
            anyOk = true;
            vTaskDelay(pdMS_TO_TICKS(400));
            for (int i = 0; i < cnt && nCand < MAX_CAND; i++) {
                if (strcmp(res[i].state, "起飞") != 0 || !res[i].anum[0]) continue;
                bool dup = false;
                for (int k = 0; k < nCand; k++)
                    if (strcmp(cand[k].tr.anum, res[i].anum) == 0) { dup = true; break; }
                if (dup) continue;
                Cand &c = cand[nCand++];
                memset(&c, 0, sizeof(c));
                snprintf(c.tr.anum, sizeof(c.tr.anum), "%s", res[i].anum);
                snprintf(c.tr.fnum, sizeof(c.tr.fnum), "%s", res[i].fnum);
                snprintf(c.tr.dep, sizeof(c.tr.dep), "%s", res[i].dep);
                snprintf(c.tr.arr, sizeof(c.tr.arr), "%s", res[i].arr);
                /* "2026-11-20 09:00:00" → 当日分钟数；解析失败排最后 */
                int hh = -1, mm = -1;
                if (sscanf(res[i].arrPlan, "%*s %d:%d", &hh, &mm) != 2 || hh < 0)
                    c.landMin = 24 * 60 + 1;
                else
                    c.landMin = hh * 60 + mm;
            }
            }
        }
    }

    /* 按落地分钟升序（插入排序，量小无所谓） */
    for (int i = 1; i < nCand; i++) {
        Cand key = cand[i];
        int j = i - 1;
        while (j >= 0 && cand[j].landMin > key.landMin) { cand[j + 1] = cand[j]; j--; }
        cand[j + 1] = key;
    }
    int nFresh = nCand < MAX_TRACK ? nCand : MAX_TRACK;
    static VfTrack freshArr[MAX_TRACK];        /* 候选是 {tr,landMin} 结构，拷出 tr 再用 */
    for (int k = 0; k < nFresh; k++) freshArr[k] = cand[k].tr;
    VfTrack *fresh = freshArr;

    if (!anyOk) return false;                 /* 网络/额度问题：保留旧表 */
    /* 继承旧位置缓存（同一架机还在飞） */
    for (int k = 0; k < nFresh; k++) {
        for (int i = 0; i < s_nTr; i++) {
            if (strcmp(s_tr[i].anum, fresh[k].anum) == 0) {
                fresh[k].pos = s_tr[i].pos;
                fresh[k].pos_ok = s_tr[i].pos_ok;
                fresh[k].lastPoll = s_tr[i].lastPoll;
                break;
            }
        }
    }
    memcpy(s_tr, fresh, (size_t)nFresh * sizeof(VfTrack));
    s_nTr = nFresh;
    s_lastScan = millis();
    s_scanned = true;
    s_needFullPoll = true;
    Serial.printf("[vf] 航线扫描完成：%d 架在飞\n", s_nTr);
    return true;
}

extern "C" int flight_vf_fetch(const app_config_t *cfg, flight_list_t *out)
{
    if (!cfg || !out || !cfg->vf_key[0]) return -1;
    if (!s_keyInited) { vf_set_key(cfg->vf_key); s_keyInited = 1; }

    uint32_t now = millis();

    if (s_quotaDead) {
        static uint32_t deadAt = 0;
        if (!deadAt) deadAt = millis();
        if (millis() - deadAt < 6UL * 3600UL * 1000UL) return -2;
        s_quotaDead = false; deadAt = 0;
    }

    /* 退避中：直接返回，别霸占 TLS（否则天气/播报都拉不到） */
    if (s_backoffUntil && (int32_t)(now - s_backoffUntil) < 0) return -3;

    /* 额度/Key 类错误（服务端明确拒绝）→ 内部退避 6 小时，避免每 2 分钟白打 12 次 */

    /* ① 周期扫描（首次立即） */
    if (!s_scanned || now - s_lastScan > SCAN_INTERVAL_MS) {
        if (!scan_routes()) {
            if (++s_failStreak >= 2) {
                s_backoffUntil = now + 30UL * 60UL * 1000UL;   /* 连挂两次 → 停 30 分钟 */
                Serial.println("[vf] 连续失败，暂停 30 分钟（省配额/让出 TLS）");
            }
            return -2;                        /* 接口失败 */
        }
        s_failStreak = 0;
        s_backoffUntil = 0;
    }

    /* ② 轮询到期飞机：扫描后首轮全量查（确定远近），之后每次最多 5 架摊成本 */
    int budget = s_needFullPoll ? MAX_TRACK : POLL_PER_FETCH;
    int polled = 0;
    for (int i = 0; i < s_nTr && polled < budget; i++) {
        if (!s_needFullPoll && now - s_tr[i].lastPoll < POLL_MIN_MS) continue;
        VfPosition p;
        if (vf_get_position(s_tr[i].anum, p)) {
            s_tr[i].pos = p;
            s_tr[i].pos_ok = p.ok;
            if (p.ok) {
                float d = 0, b = 0;
                geo_polar(cfg->latitude, cfg->longitude, (float)p.lat, (float)p.lon, &d, &b);
                Serial.printf("[vf] %s %s->%s 位置 ok：距 %.0fNM 高 %.0fm\n",
                              p.fnum, s_tr[i].dep, s_tr[i].arr, d, p.heightM);
            }
        } else {
            Serial.printf("[vf] %s 位置查询失败\n", s_tr[i].fnum);
        }
        s_tr[i].lastPoll = now;
        polled++;
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    if (s_needFullPoll && polled >= s_nTr) s_needFullPoll = false;

    /* ③ 汇总成雷达目标列表（用缓存位置，出量程的丢弃） */
    memset(out, 0, sizeof(*out));
    out->range_nm   = cfg->range_nm;
    out->center_lat = cfg->latitude;
    out->center_lon = cfg->longitude;
    out->src        = 1;

    int kept = 0;
    for (int i = 0; i < s_nTr && kept < FL_MAX; i++) {
        if (!s_tr[i].pos_ok) continue;
        float dist = 0, brg = 0;
        geo_polar(cfg->latitude, cfg->longitude,
                  (float)s_tr[i].pos.lat, (float)s_tr[i].pos.lon, &dist, &brg);
        if (dist > (float)cfg->range_nm) continue;

        flight_t *f = &out->items[kept++];
        memset(f, 0, sizeof(*f));
        snprintf(f->callsign, sizeof(f->callsign), "%s",
                 s_tr[i].pos.fnum[0] ? s_tr[i].pos.fnum : s_tr[i].fnum);
        f->alt     = (int)(s_tr[i].pos.heightM * 3.28084f / 100.0f);   /* 百英尺 */
        f->spd     = s_tr[i].pos.speedKmh / 1.852f;                     /* 节 */
        f->hdg     = s_tr[i].pos.angle;
        f->vs      = (int)(s_tr[i].pos.vspeedMs * 196.85f);             /* ft/min */
        f->dist_nm = dist;
        f->brg     = brg;
        f->is_ssr  = 1;
        f->active  = 1;
        f->glow    = 0.55f;
    }
    out->count = kept;
    out->valid = kept > 0;
    return kept;
}
