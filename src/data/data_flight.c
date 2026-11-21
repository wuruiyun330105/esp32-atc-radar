/**
 * data_flight.c —— 航班目标管理
 *
 * 坐标体系：屏幕像素为准（x/y），派生 brg/dist。
 *   · 真实模式：网络层给出 lat/lon → 这里换算成 brg/dist → x/y
 *   · 演示模式：本地按极坐标播种，按航向/地速推进
 *   · 两次刷新之间用死推算（dead reckoning）外推，动画不跳
 */
#include "data_flight.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

#ifndef M_PI
#define M_PI 3.14159265358979
#endif
#define DEG2RAD (M_PI / 180.0)

#define RADIUS_PX   232.0f     /* 与 ui_radar.c 的 R_OUT 保持一致（铺满屏幕） */
#define CX_PX       240.0f
#define CY_PX       240.0f

/* 演示模式时间压缩：真实飞机 5 秒只挪 1 像素，肉眼像静止。
 * 这里放快 15 倍，观感接近「有生命」的雷达屏，同时不牺牲航向/速度的真实数值。 */
#define DEMO_TIME_SCALE 15.0f

static flight_list_t s_list;
static const app_config_t *s_cfg = NULL;
static int s_force_demo = 1;
static int s_live = 0;          /* 已拿到过真实数据 */
static int s_last_result = 0;   /* 1 成功 / -1 失败 / 0 未尝试 */
static float s_trail_acc = 0.0f;

/* ---------- 演示种子（真实航司呼号 + 真实机型 + 真实巡航参数） ---------- */
typedef struct {
    float ang, frac, hdg;
    int   alt, spd, vs;
    const char *cs, *type, *hex;
    int ssr;
} demo_seed_t;

static const demo_seed_t SEED[] = {
    {  38, 0.52f,  52, 320, 452,  -640, "CES5102", "B738", "780a21", 1 },
    { 136, 0.70f, 132, 280, 438,      0, "CSN6218", "A320", "7812b3", 1 },
    { 312, 0.44f, 315, 360, 492,  1280, "CCA1834", "B77W", "7809c4", 1 },
    { 198, 0.66f, 197, 120, 302, -1920, "CSZ9931", "A321", "781c05", 1 },
    {  84, 0.30f,  82,  90, 265,  1600, "CHH7782", "B738", "7806d7", 0 },
    { 170, 0.86f, 170, 340, 481,    0, "CSN4406", "A333", "7815e8", 1 },
    {  62, 0.76f,  60, 380, 501,      0, "CES5801", "B789", "780bf9", 1 },
    { 334, 0.92f, 338, 150, 318,  2240, "CCA1210", "A320", "7817aa", 0 },
    { 228, 0.58f, 225, 240, 425,  -960, "CQH8891", "A320", "7813bb", 1 },
    { 292, 0.22f, 288,  60, 240,  1920, "CSC8002", "A319", "7819cc", 0 },
};
#define NSEED ((int)(sizeof(SEED) / sizeof(SEED[0])))

/* ---------- 坐标换算 ---------- */
static void polar_to_xy(float brg, float dist_nm, int range_nm, float *x, float *y)
{
    float r = dist_nm / (float)(range_nm > 0 ? range_nm : 120) * RADIUS_PX;
    float a = brg * DEG2RAD;
    *x = CX_PX + sinf(a) * r;
    *y = CY_PX - cosf(a) * r;
}

static void xy_to_polar(float x, float y, int range_nm, float *brg, float *dist_nm)
{
    float dx = x - CX_PX;
    float dy = y - CY_PX;
    float r = sqrtf(dx * dx + dy * dy);
    *dist_nm = r / RADIUS_PX * (float)(range_nm > 0 ? range_nm : 120);
    float b = atan2f(dx, -dy) * (180.0f / (float)M_PI);
    if (b < 0) b += 360.0f;
    *brg = b;
}

static void refresh_polar(void)
{
    for (int i = 0; i < s_list.count; i++)
        xy_to_polar(s_list.items[i].x, s_list.items[i].y, s_list.range_nm,
                    &s_list.items[i].brg, &s_list.items[i].dist_nm);
}

/* ---------- 随机 ---------- */
static uint32_t s_rand = 0x2545F491u;
static uint32_t rnd(void)
{
    s_rand = s_rand * 1664525u + 1013904223u;
    return s_rand >> 8;
}

/* ---------- 播种演示目标 ---------- */
static void spawn_demo(int idx, int at_edge)
{
    flight_t *f = &s_list.items[idx];
    float ang, frac;
    if (at_edge) {
        /* 从圆周随机方位进入，朝圆心方向飞 */
        ang = (float)(rnd() % 3600) / 10.0f;
        frac = 0.97f;
    } else {
        ang = SEED[idx].ang;
        frac = SEED[idx].frac;
    }
    f->hdg = at_edge ? fmodf(ang + 180.0f + (float)((int)(rnd() % 60) - 30), 360.0f)
                     : SEED[idx].hdg;
    if (f->hdg < 0) f->hdg += 360.0f;
    f->alt = SEED[idx].alt;
    f->spd = (float)SEED[idx].spd;
    f->vs = SEED[idx].vs;
    f->is_ssr = (uint8_t)SEED[idx].ssr;
    f->active = 1;
    f->glow = 0.3f;
    f->hist_n = 0;
    snprintf(f->callsign, sizeof(f->callsign), "%s", SEED[idx].cs);
    snprintf(f->type, sizeof(f->type), "%s", SEED[idx].type);
    snprintf(f->icao, sizeof(f->icao), "%s", SEED[idx].hex);
    snprintf(f->squawk, sizeof(f->squawk), "%04d", 1000 + (int)(rnd() % 6777));
    polar_to_xy(ang, frac * (float)s_list.range_nm, s_list.range_nm, &f->x, &f->y);
    xy_to_polar(f->x, f->y, s_list.range_nm, &f->brg, &f->dist_nm);
}

/* ---------- 初始化 ---------- */
void flight_init(int demo)
{
    s_force_demo = demo ? 1 : 0;
    s_live = 0;
    s_last_result = 0;
    memset(&s_list, 0, sizeof(s_list));
    s_list.range_nm = 120;
    s_list.center_lat = 30.23f;
    s_list.center_lon = 120.43f;
    s_list.valid = 1;
    s_list.src = 0;

    s_list.count = NSEED;
    for (int i = 0; i < NSEED; i++) spawn_demo(i, 0);
}

void flight_bind_config(const app_config_t *cfg)
{
    if (!cfg) return;
    s_cfg = cfg;
    s_list.range_nm = cfg->range_nm > 0 ? cfg->range_nm : 120;
    s_list.center_lat = cfg->latitude;
    s_list.center_lon = cfg->longitude;
    if (!s_force_demo && cfg->flight_src != FL_SRC_DEMO) s_list.src = cfg->flight_src;
}

void flight_set_view(float lat, float lon, int range_nm)
{
    s_list.center_lat = lat;
    s_list.center_lon = lon;
    if (range_nm > 0) s_list.range_nm = range_nm;
    refresh_polar();
}

/* ---------- 真实数据注入（网络层解析完调这里） ----------
 * 仅主循环调用，无并发：旧数据暂存用静态缓冲，
 * 不在 loopTask（默认 8KB 栈）上叠 ~3KB 的 flight_list_t。 */
static flight_list_t s_old_list;

void flight_apply_live(const flight_list_t *src)
{
    if (!src || src->count <= 0) { s_last_result = -1; return; }

    /* 保留旧目标的余晖与尾迹，按呼号匹配 */
    s_old_list = s_list;
    const flight_list_t *old = &s_old_list;

    s_list.count = 0;
    s_list.valid = 1;
    s_list.src = 1;
    s_list.updated_ms = 0;
    if (src->range_nm > 0) s_list.range_nm = src->range_nm;

    int n = src->count > FL_MAX ? FL_MAX : src->count;
    for (int i = 0; i < n; i++) {
        const flight_t *in = &src->items[i];
        flight_t *f = &s_list.items[s_list.count++];
        *f = *in;
        f->active = 1;
        polar_to_xy(f->brg, f->dist_nm, s_list.range_nm, &f->x, &f->y);

        /* 找同一架飞机，继承尾迹 */
        for (int k = 0; k < old->count; k++) {
            if (old->items[k].active && f->callsign[0] &&
                strcmp(old->items[k].callsign, f->callsign) == 0) {
                f->glow = old->items[k].glow;
                f->hist_n = old->items[k].hist_n;
                memcpy(f->hist, old->items[k].hist, sizeof(f->hist));
                break;
            }
        }
    }
    s_live = 1;
    s_last_result = 1;
}

int flight_is_live(void) { return s_live; }
int flight_last_result(void) { return s_last_result; }

/* ---------- 每帧推进 ---------- */
void flight_update(float dt_sec)
{
    if (dt_sec <= 0.0f) return;
    if (dt_sec > 0.5f) dt_sec = 0.5f;

    float nm_per_px = (float)(s_list.range_nm > 0 ? s_list.range_nm : 120) / RADIUS_PX;
    float scale = s_live ? 1.0f : DEMO_TIME_SCALE;

    for (int i = 0; i < s_list.count; i++) {
        flight_t *f = &s_list.items[i];
        if (!f->active) continue;

        /* 位移：节 → 像素/秒 */
        float pxps = f->spd / 3600.0f / nm_per_px * scale;
        float a = f->hdg * DEG2RAD;
        f->x += sinf(a) * pxps * dt_sec;
        f->y -= cosf(a) * pxps * dt_sec;

        /* 余晖自然消退（真实秒，不参与时间压缩） */
        if (f->glow > 0.26f) f->glow -= dt_sec * 0.16f;
        if (f->glow < 0.26f) f->glow = 0.26f;

        /* 出界处理 */
        float dx = f->x - CX_PX, dy = f->y - CY_PX;
        if (!s_live && dx * dx + dy * dy > RADIUS_PX * RADIUS_PX) {
            spawn_demo(i, 1);
            continue;
        }

        /* 尾迹采样：每 0.7s 一个点 */
        xy_to_polar(f->x, f->y, s_list.range_nm, &f->brg, &f->dist_nm);
    }

    /* 尾迹整体前移（每 0.7s 丢最老的点、记最新的点） */
    s_trail_acc += dt_sec;
    if (s_trail_acc >= 0.7f) {
        s_trail_acc = 0.0f;
        for (int i = 0; i < s_list.count; i++) {
            flight_t *f = &s_list.items[i];
            if (!f->active) continue;
            for (int k = FL_HIST_N - 1; k > 0; k--) {
                f->hist[k][0] = f->hist[k - 1][0];
                f->hist[k][1] = f->hist[k - 1][1];
            }
            f->hist[0][0] = f->x;
            f->hist[0][1] = f->y;
            if (f->hist_n < FL_HIST_N) f->hist_n++;
        }
    }
}

/* ---------- 扫描激发 ---------- */
void flight_excite(float sweep_deg)
{
    for (int i = 0; i < s_list.count; i++) {
        flight_t *f = &s_list.items[i];
        if (!f->active) continue;
        float d = sweep_deg - f->brg;
        while (d > 180.0f) d -= 360.0f;
        while (d < -180.0f) d += 360.0f;
        if (d < 0.0f) d = -d;
        if (d < 5.0f) f->glow = 1.0f;
    }
}

/* ---------- 查询 ---------- */
const flight_list_t *flight_get(void) { return &s_list; }

const flight_t *flight_at(int i)
{
    if (i < 0 || i >= s_list.count) return NULL;
    return &s_list.items[i];
}

int flight_pick(int px, int py, int tol_px)
{
    int best = -1;
    float bestd = (float)(tol_px * tol_px);
    for (int i = 0; i < s_list.count; i++) {
        const flight_t *f = &s_list.items[i];
        if (!f->active) continue;
        float dx = f->x - (float)px, dy = f->y - (float)py;
        float d2 = dx * dx + dy * dy;
        if (d2 < bestd) { bestd = d2; best = i; }
    }
    return best;
}
