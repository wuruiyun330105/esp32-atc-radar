/**
 * main_pc.c —— PC 模拟器入口
 *  - LVGL 内置 SDL 驱动模拟 480x480 屏 + 鼠标手势（拖动=滑动切页）
 *  - 键盘 ← → 切页；Q/Esc 退出
 *  - 后台线程每 15s 拉一次真实 ADS-B 数据（配置了数据源时）
 *
 * 环境变量（调试/截图用）：
 *   START_PAGE=0|1|2   启动页
 *   DEBUG_CYCLE=毫秒    自动轮播切页
 *   RADAR_SRC=0|1|2    0=演示 1=airplanes.live/adsb.fi/adsb.lol 2=OpenSky
 *   OPEN_TARGET=n      启动时打开第 n 个目标的详情卡
 */
#include "lvgl.h"
#include "app_pages.h"
#include "ui/ui_theme.h"
#include "ui/ui_radar.h"
#include "data/data_config.h"
#include "data/data_weather.h"
#include "data/data_flight.h"
#include "data/data_flight_net.h"
#include "data/data_board.h"
#include <unistd.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL.h>
#include "sim_audio.h"   /* PC 模拟器程序化音效（header-only, 可静默降级） */
#include "sim_atc.h"     /* PC 模拟器模拟塔台对话（SAPI TTS 离线合成） */

/* 音效总开关：SIM_SOUND=0 可关闭（默认开，无设备时自动静音） */
static int s_sound_on = 1;

static app_config_t s_cfg;
static flight_list_t s_pending;
static volatile int s_pending_ready = 0;
static volatile int s_stop = 0;
static volatile uint32_t s_fl_seq = 0;   /* 序号锁：奇数=写入中，偶数=稳定 */
/* 天气结果同样只在线程里打标记，由主循环落地——
 * LVGL 非线程安全，后台线程不得直接调 app_pages_set_weather */
static weather_data_t s_wx_pending;
static volatile int s_wx_ready = 0;
/* 航班动态（飞常准，计费接口，10 分钟一次） */
static board_list_t s_bd_pending;
static volatile int s_bd_ready = 0;
static volatile uint32_t s_bd_seq = 0;

static void *weather_thread(void *arg)
{
    (void)arg;
    weather_data_t w;
    memset(&w, 0, sizeof(w));
    if (weather_fetch_now(&s_cfg, &w)) {
        s_wx_pending = w;
        s_wx_ready = 1;
        printf("[weather] 拉取成功：%s %d°C 湿度%d%%\n", w.condition, w.temp, w.humidity);
    } else {
        printf("[weather] 未配置 Key 或拉取失败，使用演示数据\n");
    }
    return NULL;
}

static void *flight_thread(void *arg)
{
    (void)arg;
    int first = 1;
    while (!s_stop) {
        flight_list_t tmp;
        int n = flight_net_fetch_host(s_cfg.latitude, s_cfg.longitude,
                                      s_cfg.range_nm, s_cfg.flight_src,
                                      s_cfg.adsb_host, &tmp);
        if (n > 0) {
            s_fl_seq++;
            s_pending = tmp;
            s_fl_seq++;
            s_pending_ready = 1;
            printf("[flight] 收到 %d 架次（源 %s）\n", n,
                   s_cfg.flight_src == FL_SRC_OPENSKY ? "OpenSky" : "ADS-B");
        } else {
            printf("[flight] 拉取失败，继续演示数据\n");
        }
        first = 0;
        (void)first;
        for (int i = 0; i < 150 && !s_stop; i++) usleep(100000);   /* 15s */
    }
    return NULL;
}

static void *board_thread(void *arg)
{
    (void)arg;
    while (!s_stop) {
        if (s_cfg.vf_key[0]) {
            board_list_t bd;
            int n = board_fetch(&s_cfg, &bd);
            if (n >= 0) {
                s_bd_seq++;
                s_bd_pending = bd;
                s_bd_seq++;
                s_bd_ready = 1;
                printf("[board] 航班动态 %d 条（%s %s，10min 后再刷）\n",
                       n, bd.airport, bd.airport_name);
            } else {
                printf("[board] 拉取失败（检查飞常准 Key/额度）\n");
            }
        }
        for (int i = 0; i < 6000 && !s_stop; i++) usleep(100000);   /* 10min */
    }
    return NULL;
}

int main(void)
{
    /* Windows 下重定向到文件是全缓冲（_IOLBF 在 MinGW 上不生效），
     * 异常退出会丢日志；直接改无缓冲保底 */
    setvbuf(stdout, NULL, _IONBF, 0);
    if (getenv("SIM_SOUND") && atoi(getenv("SIM_SOUND")) == 0) s_sound_on = 0;
    if (s_sound_on) {
        sim_audio_init();
        sim_audio_ping();   /* 启动确认音：一开窗口就能听到，证明音频链路通 */
        const char *eatc = getenv("SIM_ATC");
        if (!eatc || atoi(eatc) != 0) sim_atc_init();   /* 模拟塔台对话，SIM_ATC=0 关闭 */
    }
    lv_init();
    lv_display_t *disp = lv_sdl_window_create(SCR_W, SCR_H);
    (void)disp;
    lv_sdl_mouse_create();

    bool has = cfg_load(&s_cfg);

    /* 环境变量可临时覆盖数据源 */
    const char *esrc = getenv("RADAR_SRC");
    if (esrc) s_cfg.flight_src = atoi(esrc);
    const char *erange = getenv("RADAR_RANGE");
    if (erange) s_cfg.range_nm = atoi(erange);

    printf("=== ESP32 塔台雷达 · PC 模拟器 ===\n");
    printf("配置: %s (GPS %.3f,%.3f range=%dNM src=%d)\n",
           has ? "已载入" : "默认值", s_cfg.latitude, s_cfg.longitude,
           s_cfg.range_nm, s_cfg.flight_src);

    flight_init(s_cfg.flight_src == FL_SRC_DEMO ? 1 : 0);
    flight_bind_config(&s_cfg);
    flight_set_view(s_cfg.latitude, s_cfg.longitude, s_cfg.range_nm);

    app_pages_init();

    /* 页面切换统一检测：鼠标拖动/键盘/任何入口都触发音效 */
    int last_page = app_pages_current();

    const char *sp = getenv("START_PAGE");
    if (sp) app_pages_goto(atoi(sp), 0);

    const char *ot = getenv("OPEN_TARGET");
    if (ot) ui_radar_select_index(atoi(ot));

    int cycle_ms = 0;
    const char *dc = getenv("DEBUG_CYCLE");
    if (dc) cycle_ms = atoi(dc);
    uint32_t last_cycle = lv_tick_get();
    uint32_t last_atc   = lv_tick_get();   /* 模拟塔台对话播报计时 */

    pthread_t wth, fth, bth;
    bool w_started = false, f_started = false, b_started = false;
    if (s_cfg.qweather_key[0] && s_cfg.qweather_host[0]) {
        pthread_create(&wth, NULL, weather_thread, NULL);
        w_started = true;
    }
    if (s_cfg.flight_src != FL_SRC_DEMO) {
        pthread_create(&fth, NULL, flight_thread, NULL);
        f_started = true;
    }
    if (s_cfg.vf_key[0]) {
        pthread_create(&bth, NULL, board_thread, NULL);
        b_started = true;
    }

    printf("操作: 鼠标点击选中目标 / 拖动或 ←→ 切页 | Q 退出\n");

    int running = 1;
    while (running) {
        uint32_t now = lv_tick_get();
        lv_timer_handler();

        if (s_pending_ready) {
            uint32_t s0 = s_fl_seq;
            flight_apply_live(&s_pending);
            uint32_t s1 = s_fl_seq;
            if (s0 == s1 && (s0 & 1u) == 0u) {
                s_pending_ready = 0;
                printf("[flight] 已接管 %d 架次（真实）\n", flight_get()->count);
                if (s_sound_on) sim_audio_ping();   /* 新数据到位：清脆提示 */
            }
        }

        if (s_wx_ready) {
            s_wx_ready = 0;
            app_pages_set_weather(&s_wx_pending);
        }

        if (s_bd_ready) {
            uint32_t s0 = s_bd_seq;
            board_list_t bd = s_bd_pending;
            uint32_t s1 = s_bd_seq;
            if (s0 == s1 && (s0 & 1u) == 0u) {
                s_bd_ready = 0;
                app_pages_set_board(&bd);
            }
        }

        app_pages_tick();

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) running = 0;
            if (ev.type == SDL_KEYDOWN) {
                if (ev.key.keysym.sym == SDLK_q || ev.key.keysym.sym == SDLK_ESCAPE) running = 0;
                else if (ev.key.keysym.sym == SDLK_RIGHT) app_pages_goto(app_pages_current() + 1, 1);
                else if (ev.key.keysym.sym == SDLK_LEFT)  app_pages_goto(app_pages_current() - 1, 1);
            }
        }
        if (cycle_ms > 0 && now - last_cycle >= (uint32_t)cycle_ms) {
            last_cycle = now;
            app_pages_goto(app_pages_current() + 1, 1);
        }

        /* 模拟塔台对话：每 35 秒从航班列表挑目标播报一句 */
        if (now - last_atc >= 35000) {
            last_atc = now;
            sim_atc_announce(flight_get());
        }

        /* 页面切换音效：统一检测，覆盖鼠标拖动/键盘/自动轮播所有路径 */
        {
            int pg = app_pages_current();
            if (pg != last_page) {
                if (s_sound_on) {
                    if (pg > last_page) sim_audio_chime();  /* 向后翻页：柔和三音 */
                    else                sim_audio_blip();   /* 向前返回：短促键音 */
                }
                last_page = pg;
            }
        }
        usleep(6000);
    }

    s_stop = 1;
    if (w_started) pthread_join(wth, NULL);
    if (f_started) pthread_join(fth, NULL);
    if (b_started) pthread_join(bth, NULL);
    if (s_sound_on) {
        sim_atc_quit();
        sim_audio_quit();
    }
    return 0;
}
