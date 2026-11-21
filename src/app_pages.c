/**
 * app_pages.c —— 多页框架实现
 *  - 双页屏幕 + 手势左右滑动切换
 *  - 页码指示器放在 top layer（跨页常驻）
 *  - 数据分发：时钟每秒刷新、天气由外部注入、雷达动画仅在雷达页跑
 */
#include "app_pages.h"
extern volatile int g_log_mute;
#include "ui/ui_radar.h"
#include "ui/ui_clock_weather.h"
#include "ui/ui_theme.h"
#include "data/data_time.h"
#include "data/data_flight.h"
#include <string.h>
#include <stdio.h>

static lv_obj_t *s_scr[PAGE_COUNT];
static lv_obj_t *s_dots[PAGE_COUNT];
static int s_cur = PAGE_RADAR;

static weather_data_t s_wx;
static board_list_t s_bd;                 /* 最新航班动态（供进入页面时重绘） */
static uint32_t s_last_sec = 0;
static uint32_t s_last_radar = 0;
static uint32_t s_last_dt  = 0;

/* ---------- 页码指示器 ---------- */
static void build_dots(void)
{
    lv_obj_t *top = lv_layer_top();
    lv_obj_clear_flag(top, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < PAGE_COUNT; i++) {
        lv_obj_t *d = lv_obj_create(top);
        lv_obj_set_size(d, 6, 6);
        lv_obj_set_pos(d, 224 + i * 16, 462);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(d, 0, 0);
        lv_obj_set_style_bg_color(d, lv_color_hex(0x808080), 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_40, 0);
        lv_obj_clear_flag(d, LV_OBJ_FLAG_SCROLLABLE);
        s_dots[i] = d;
    }
}

static void update_dots(void)
{
    for (int i = 0; i < PAGE_COUNT; i++) {
    if (i == s_cur) {
        lv_color_t c = (s_cur == PAGE_RADAR) ? RD_GREEN : CW_INK_SOFT;
            lv_obj_set_style_bg_opa(s_dots[i], LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(s_dots[i], c, 0);
            lv_obj_set_size(s_dots[i], 8, 8);
        } else {
            lv_obj_set_size(s_dots[i], 6, 6);
            lv_obj_set_style_bg_opa(s_dots[i], LV_OPA_40, 0);
            lv_obj_set_style_bg_color(s_dots[i], lv_color_hex(0x808080), 0);
        }
    }
}

/* ---------- 手势 ---------- */
static void gesture_cb(lv_event_t *e)
{
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    /* 详情卡打开时，滑动先关卡片，不切页 */
    if (ui_radar_card_open() && s_cur == PAGE_RADAR) {
        ui_radar_card_close();
        return;
    }
    if (dir == LV_DIR_LEFT)       app_pages_goto(s_cur + 1, 1);
    else if (dir == LV_DIR_RIGHT) app_pages_goto(s_cur - 1, 1);
    (void)e;
}

static void attach_gesture(lv_obj_t *scr)
{
    lv_obj_add_event_cb(scr, gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_GESTURE_BUBBLE);
}

void app_pages_init(void)
{
    memset(&s_wx, 0, sizeof(s_wx));
    s_wx.temp = 26; s_wx.feels_like = 28; s_wx.humidity = 65;
    s_wx.wind_scale = 3; s_wx.pressure = 1013; s_wx.precip = 0;
    strcpy(s_wx.wind_dir, "东风"); strcpy(s_wx.condition, "多云");
    s_wx.icon = 101; s_wx.update_hour = 8; s_wx.update_min = 5;
    time_now(&s_wx);

    s_scr[PAGE_RADAR] = ui_radar_create();
    s_scr[PAGE_CLOCK] = ui_clock_weather_create();

    for (int i = 0; i < PAGE_COUNT; i++) attach_gesture(s_scr[i]);


    const flight_list_t *fl = flight_get();
    ui_radar_set_flights(fl);
    ui_radar_set_range(fl->range_nm);

    build_dots();
    s_cur = PAGE_RADAR;
    lv_scr_load(s_scr[PAGE_RADAR]);
    ui_clock_weather_update(&s_wx);
    update_dots();
    s_last_dt = lv_tick_get();
}

void app_pages_goto(int page, int anim)
{
    if (page < 0) page = PAGE_COUNT - 1;
    if (page >= PAGE_COUNT) page = 0;
    if (page == s_cur) return;
    s_cur = page;
    lv_scr_load_anim(s_scr[s_cur],
        anim ? LV_SCR_LOAD_ANIM_MOVE_LEFT : LV_SCR_LOAD_ANIM_NONE,
        anim ? 200 : 0, 0, false);
    update_dots();
    if (s_cur == PAGE_CLOCK) {
        time_now(&s_wx);
        ui_clock_weather_update(&s_wx);
    }
}

int app_pages_current(void) { return s_cur; }

void app_pages_set_weather(const weather_data_t *w)
{
    if (!w) return;
    int y = s_wx.year, mo = s_wx.mon, d = s_wx.day, wd = s_wx.weekday;
    int h = s_wx.hour, mi = s_wx.min, se = s_wx.sec;
    s_wx = *w;
    s_wx.year = y; s_wx.mon = mo; s_wx.day = d; s_wx.weekday = wd;
    s_wx.hour = h; s_wx.min = mi; s_wx.sec = se;
    if (s_cur == PAGE_CLOCK) ui_clock_weather_update(&s_wx);
}

void app_pages_set_board(const board_list_t *b)
{
    if (!b) return;
    s_bd = *b;
    ui_radar_set_board(&s_bd);      /* 航班动态并入雷达页底部航班条 */
}

void app_pages_set_radio(int tx, const char *subtitle)
{
    ui_radar_set_radio(tx, subtitle);
}

void app_pages_tick(void)
{
    uint32_t now = lv_tick_get();

    /* 雷达：20fps 渲染 + 真实 dt 推进 */
    if (s_cur == PAGE_RADAR && now - s_last_radar >= 50) {
        float dt = (float)(now - s_last_radar) / 1000.0f;
        s_last_radar = now;
        flight_update(dt);
        ui_radar_tick();

        /* 左下角：数据源状态 */
        static int last_src = -1, last_live = -1;
        int src = flight_is_live() ? 1 : 0;
        if (src != last_src) {
            last_src = src;
            ui_radar_set_status(src ? "LIVE  ADS-B" : "SIM   DEMO", NULL);
        }
        (void)last_live;
    }

    /* 秒级刷新 */
    if (now - s_last_sec >= 1000) {
        s_last_sec = now;
        time_now(&s_wx);
        if (s_cur == PAGE_CLOCK) ui_clock_weather_update(&s_wx);
        if (s_cur == PAGE_RADAR) {
            char utc[32];
            snprintf(utc, sizeof(utc), "UTC %02d:%02d:%02d", s_wx.hour, s_wx.min, s_wx.sec);
            ui_radar_set_status(NULL, utc);
        }
    }
}
