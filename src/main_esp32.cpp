/**
 * main_esp32.cpp —— 设备端主入口（Arduino 框架）
 *
 * 流程：
 *   board_init() → lvgl_port_init() → 载入配置
 *     ├─ 有 WiFi 凭据：连 WiFi → NTP 校时 → 起天气/航班后台任务 → 进双页 UI
 *     └─ 无凭据：进 AP 配网门户（手机配置后重启）
 *   loop()：lv_timer_handler + app_pages_tick + 后台结果落地
 *
 * 线程模型：网络（HTTPS + 大 JSON）全在 net_task 里跑，
 *          解析结果打标记，由 loop() 在主线程接管，LVGL 永不被跨线程调用。
 */
#include <Arduino.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "port.h"
#include "app_pages.h"
#include "data/data_config.h"
#include "data/data_weather.h"
#include "data/data_time.h"
#include "data/data_flight.h"
#include "data/data_flight_net.h"
#include "data/data_flight_vf.h"
#include "data/data_board.h"
#include "net/web_config.h"
#include "port/port_audio.h"
#include "net/announce.h"
#include "net/ip_locate.h"

extern "C" void port_snap_begin(void);
extern "C" void port_snap_dump(void);
extern "C" int port_snap_tick(void);
volatile int g_log_mute = 0;          /* 1=全任务静音（截屏期间防日志污染数据流） */
#include "net/ip_locate.h"

extern "C" void port_snap_begin(void);
extern "C" void port_snap_dump(void);
extern "C" int port_snap_tick(void);
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <esp_heap_caps.h>
#include <string.h>

static app_config_t g_cfg;
static weather_data_t g_wx_buf;
static flight_list_t g_fl_buf;
static volatile bool g_new_wx = false;
static volatile bool g_new_fl = false;
static volatile bool g_wifi_ok = false;
/* 航班大结构体（~3KB）跨核交接：序号锁防止主循环读到拷贝一半的数据。
 * g_fl_seq 奇数 = net_task 正在写；偶数 = 稳定可读。 */
static flight_list_t s_fl_local;        /* 主循环侧静态暂存（省 loopTask 栈） */
static volatile uint32_t g_fl_seq = 0;
/* 航班动态（飞常准，~1KB）：同样序号锁交接 */
static board_list_t g_bd_buf;
static board_list_t s_bd_local;
static volatile bool g_new_bd = false;
static volatile uint32_t g_bd_seq = 0;

/* 雷达实时机群：ADS-B 与飞常准轨迹双源，合并后喂给雷达
 * （国内 ADS-B 公益源常态 0~3 架，飞常准自家 ADS-B 覆盖好） */
static flight_list_t s_adsb_list;
static flight_list_t s_vf_list;
static bool s_adsb_ok = false, s_vf_ok = false;
#define VF_INTERVAL_MS (2UL * 60UL * 1000UL)   /* 飞常准官方端点：2 分钟一 fetch（内部自带扫描/轮询预算） */
/* 飞常准"本月积分不足"后暂停（避免每轮白打接口）：6 小时后再试 */
#define VF_SUSPEND_MS  (6UL * 60UL * 60UL * 1000UL)
static uint32_t s_vf_suspend_until = 0;
static uint32_t s_bd_suspend_until = 0;
/* 双源连续空：切演示模式填充画面（左下角明确标 SIM DEMO；真实数据到达自动切回） */
static volatile bool g_want_demo = false;
static int s_empty_cycles = 0;

/* 合并双源：ADS-B 优先，飞常准补充；按呼号去重，上限 FL_MAX */
static void merge_sources(flight_list_t *out)
{
    memset(out, 0, sizeof(*out));
    out->range_nm = g_cfg.range_nm;
    out->center_lat = g_cfg.latitude;
    out->center_lon = g_cfg.longitude;
    out->src = 1;
    int n = 0;
    const flight_list_t *srcs[2] = { &s_adsb_list, &s_vf_list };
    for (int s = 0; s < 2; s++) {
        const flight_list_t *L = srcs[s];
        if (!L->valid) continue;
        for (int i = 0; i < L->count && n < FL_MAX; i++) {
            const flight_t *cand = &L->items[i];
            bool dup = false;
            for (int k = 0; k < n; k++) {
                if (cand->callsign[0] &&
                    strcmp(out->items[k].callsign, cand->callsign) == 0) { dup = true; break; }
            }
            if (!dup) out->items[n++] = *cand;
        }
    }
    out->count = n;
    out->valid = n > 0;
}

/* 航班刷新间隔（流量收紧，开发者要求）：
 * 公益 ADS-B 每次响应 ~100KB 且国内常态 0~3 架 → 20s 轮询一天要拉 400MB+，改为
 *   · 飞常准有机群时 10 分钟一拉（保底补充）
 *   · 无机群时 5 分钟一拉
 * 本地 dump1090 5s；OpenSky 300s（匿名配额限制） */
static uint32_t flight_interval_ms(void)
{
    if (g_cfg.flight_src == FL_SRC_OPENSKY) return 300000;
    if (g_cfg.flight_src == FL_SRC_LOCAL)   return 5000;
    if (g_cfg.flight_src == FL_SRC_ADSB)    return s_vf_ok ? 600000 : 300000;
    return 300000;
}

/* 后台任务：WiFi（断线自动重连，永不放弃）→ NTP → 天气 15min / 航班按源定频。
 * 首循环立即拉取，避免开机后长时间停留在演示/假数据。 */
static void net_task(void *arg)
{
    (void)arg;
    bool ntp_started = false;
    bool loc_done = false;   /* IP 定位只做一次 */

    /* 首拉时间戳向前"预支"一个周期：第一次条件判断即成立，立即拉取 */
    uint32_t now0 = millis();
    uint32_t last_wx = now0 - (15u * 60u * 1000u) - 1u;
    uint32_t s_wx_retry_ms = 15UL * 60UL * 1000UL;   /* 下次天气重试间隔（失败时缩短） */
    uint32_t last_fl = now0 - flight_interval_ms() - 1u;
    uint32_t last_vf = now0 - VF_INTERVAL_MS - 1u;
    /* 飞常准按调用计费：10 分钟一刷（~150 次/天），配了 Key 才拉 */
    uint32_t last_bd = now0 - (10u * 60u * 1000u) - 1u;

    for (;;) {
        /* WiFi 首连 / 断线重连：连不上等 30s 再试，任务不退出 */
        if (WiFi.status() != WL_CONNECTED) {
            if (g_wifi_ok) {                        /* 之前连过 → 现在是断线 */
                Serial.println("[net] !! WiFi 断线，开始自动重连");
                g_wifi_ok = false;
            }
            static int wifi_tries = 0;
            wifi_tries++;
            Serial.printf("[net] WiFi 连接尝试 #%d (\"%s\")...\n",
                          wifi_tries, g_cfg.wifi_ssid);
            WiFi.mode(WIFI_STA);
            WiFi.disconnect();
            WiFi.begin(g_cfg.wifi_ssid, g_cfg.wifi_pass);
            uint32_t t0 = millis();
            while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000)
                vTaskDelay(pdMS_TO_TICKS(300));
            if (WiFi.status() != WL_CONNECTED) {
                Serial.println("[net] !! 本轮连接失败，30s 后重试（检查路由/密码/2.4G）");
                vTaskDelay(pdMS_TO_TICKS(30000));   /* 路由未响应，稍后重试 */
                continue;
            }
            Serial.printf("[net] WiFi 已连接 IP=%s RSSI=%d dBm\n",
                          WiFi.localIP().toString().c_str(), WiFi.RSSI());
            /* 连接层诊断：DNS / 堆 / 网关 */
            Serial.printf("[net] diag 堆: 内部 %u / 最大块 %u | PSRAM %u | DNS=%s 网关=%s\n",
                          ESP.getFreeHeap(), ESP.getMaxAllocHeap(),
                          ESP.getFreePsram(),
                          WiFi.dnsIP().toString().c_str(),
                          WiFi.gatewayIP().toString().c_str());
            IPAddress a;
            if (WiFi.hostByName("api.adsb.lol", a)) {
                Serial.printf("[net] diag DNS api.adsb.lol -> %s\n", a.toString().c_str());
            } else {
                Serial.println("[net] diag DNS 解析失败!");
            }
            {   /* 纯 TCP 连接测试（不过 TLS） */
                WiFiClient t;
                Serial.printf("[net] diag TCP 223.5.5.5:443 -> %s\n",
                              t.connect(IPAddress(223,5,5,5), 443, 5000) ? "OK" : "FAIL");
                t.stop();
            }
        }
        if (!ntp_started) {
            ntp_started = true;
            g_wifi_ok = true;
            time_begin_ntp();
            vTaskDelay(pdMS_TO_TICKS(800));
        }

        /* IP 自动定位：用户没在配网页存过坐标时，按出口 IP 定到城市中心（只做一次） */
        if (!loc_done && !cfg_pos_saved()) {
            loc_done = true;
            double la = 0, lo = 0;
            char city[32] = "";
            if (ip_locate(&la, &lo, city, sizeof(city))) {
                g_cfg.latitude = la;
                g_cfg.longitude = lo;
                flight_set_view(la, lo, g_cfg.range_nm);
            }
        }

        /* IP 自动定位：用户没在配网页存过坐标时，按出口 IP 定到城市中心（只做一次） */
        if (!loc_done && !cfg_pos_saved()) {
            loc_done = true;
            double la = 0, lo = 0;
            char city[32] = "";
            if (ip_locate(&la, &lo, city, sizeof(city))) {
                g_cfg.latitude = la;
                g_cfg.longitude = lo;
                flight_set_view(la, lo, g_cfg.range_nm);
            }
        }

        uint32_t now = millis();
        uint32_t fl_interval = flight_interval_ms();
        if (g_cfg.flight_src == FL_SRC_OPENSKY)
            fl_interval += flight_net_opensky_extra_wait();  /* 429 冷却：自动放宽到 ~20min */

        /* 国内公益源长时间 0 架时降频到 60s，避免和 TTS/天气抢 TLS 会话 */
        if (s_empty_cycles >= 3 && fl_interval < 60000UL) fl_interval = 60000UL;
        if (!announce_busy() &&
            g_cfg.flight_src != FL_SRC_DEMO && now - last_fl > fl_interval) {
            last_fl = now;
            flight_list_t tmp;
            int n = flight_net_fetch_host(g_cfg.latitude, g_cfg.longitude,
                                          g_cfg.range_nm, g_cfg.flight_src,
                                          g_cfg.adsb_host, &tmp);
            if (n > 0) {
                Serial.printf("[net] ADS-B %d 架（源=%d）\n", n, g_cfg.flight_src);
                s_adsb_list = tmp;
                s_adsb_ok = true;
                s_empty_cycles = 0;
                merge_sources(&g_fl_buf);
                g_fl_seq++;                     /* 进入写：奇数 */
                g_fl_seq++;                     /* 写完：偶数 */
                g_new_fl = true;
            } else {
                Serial.printf("[net] ADS-B 0 架（源=%d；国内公益源常态稀薄，靠飞常准补）\n",
                              g_cfg.flight_src);

            }
        }

        /* 飞常准实时机群：在飞航班轨迹 → 雷达目标（国内覆盖主力） */
        if (!announce_busy() &&
            g_cfg.vf_key[0] && now - last_vf > VF_INTERVAL_MS &&
            (int32_t)(now - s_vf_suspend_until) > 0) {
            if (!time_is_synced()) { vTaskDelay(pdMS_TO_TICKS(2000)); continue; }
            last_vf = now;
            int vn = flight_vf_fetch(&g_cfg, &s_vf_list);
            if (vn > 0) {
                Serial.printf("[net] 飞常准机群 %d 架（%dmin 后再刷）\n",
                              vn, (int)(VF_INTERVAL_MS / 60000));
                s_vf_ok = true;
                merge_sources(&g_fl_buf);
                g_fl_seq++;
                g_fl_seq++;
                g_new_fl = true;
            } else if (vn == -2) {
                s_vf_suspend_until = now + VF_SUSPEND_MS;
                Serial.println("[net] !! 飞常准本月积分不足，机群暂停 6 小时（充积分后自动恢复）");
            } else {
                Serial.printf("[net] 飞常准机群 0 架（返回 %d：当前无在飞航班）\n", vn);
            }
        }

        if (false && g_cfg.vf_key[0] && now - last_bd > 10 * 60 * 1000UL &&
            (int32_t)(now - s_bd_suspend_until) > 0) {   /* 停用：官方端点无大屏工具，旧 cmcp Key 已欠费 */
            /* 必须等 NTP 校时：未校时 today 是 1970-01-01，大屏当天航班会被全过滤成 0 条 */
            if (!time_is_synced()) { vTaskDelay(pdMS_TO_TICKS(2000)); continue; }
            last_bd = now;
            board_list_t bd;
            int n = board_fetch(&g_cfg, &bd);
            if (n >= 0) {
                Serial.printf("[net] 航班动态 %d 条（%s %s，10min 后再刷）\n",
                              n, bd.airport, bd.airport_name);
                g_bd_seq++;
                g_bd_buf = bd;
                g_bd_seq++;
                g_new_bd = true;
            } else if (n == -2) {
                s_bd_suspend_until = now + VF_SUSPEND_MS;
                Serial.println("[net] !! 飞常准本月积分不足，航班条暂停 6 小时");
            } else {
                Serial.println("[net] !! 航班动态拉取失败（检查飞常准 Key/额度）");
            }
        }

        /* 天气：成功 15min 再刷；失败 3min 就重试（别让旧数据挂一小时） */
        if (!announce_busy() && now - last_wx > s_wx_retry_ms) {
            last_wx = now;
            weather_data_t w;
            if (weather_fetch_now(&g_cfg, &w)) {
                s_wx_retry_ms = 15UL * 60UL * 1000UL;
                Serial.printf("[net] 天气 %d°C %s（15min 后再刷）\n", w.temp, w.condition);
                g_wx_buf = w;
                g_new_wx = true;
            } else {
                s_wx_retry_ms = 3UL * 60UL * 1000UL;
                Serial.printf("[net] !! 天气拉取失败，3min 后重试 | 内部堆 %u 最大块 %u\n",
                              (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                              (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void setup()
{
    Serial.begin(115200);
    delay(300);

    board_init();
    lvgl_port_init();
    audio_init();          /* I2S + ES8311 + 功放（失败不影响主流程） */

    bool has_cfg = cfg_load(&g_cfg);

    /* 播报：TTS 服务是本机 PC（Windows 本地语音，免费离线） */
    announce_init();
    announce_set_auto(g_cfg.ann_auto);
    announce_set_mbkey(g_cfg.mb_key);
    announce_set_host(g_cfg.tts_host);

    /* 到货自检：配置摘要（不打印 WiFi 密码） */
    Serial.printf("[app] 配置: ssid=\"%s\" src=%d(0演示/1公益/2OpenSky/3本地) "
                  "range=%dNM lat=%.4f lon=%.4f\n",
                  g_cfg.wifi_ssid, g_cfg.flight_src, g_cfg.range_nm,
                  g_cfg.latitude, g_cfg.longitude);
    Serial.printf("[app] 和风: %s | 本地接收机: %s | 飞常准: %s\n",
                  g_cfg.qweather_host[0] ? "已配置" : "未配置",
                  g_cfg.adsb_host[0] ? g_cfg.adsb_host : "未配置",
                  g_cfg.vf_key[0] ? "已配置" : "未配置");

    /* 只看 SSID 是否非空：配置文件/NVS 均可。NVS 命名空间不存在时 cfg_load 返回
     * false 但默认值里可能已带 WiFi（出厂预置），此时不该进配网。 */
    if (strlen(g_cfg.wifi_ssid) == 0) {
        Serial.println("[app] 进入配网模式 HGH-RADAR / 192.168.4.1");
        web_config_start();
    }

    flight_init(g_cfg.flight_src == FL_SRC_DEMO ? 1 : 0);
    flight_bind_config(&g_cfg);
    flight_set_view(g_cfg.latitude, g_cfg.longitude, g_cfg.range_nm);

    app_pages_init();
    {
        /* LVGL 池用量监控：56KB 池若不够会在这暴露（对象分配失败会丢控件） */
        lv_mem_monitor_t mon;
        lv_mem_monitor(&mon);
        Serial.printf("[app] LVGL 池 %uKB：已用 %uKB（%d%%）\n",
                      (unsigned)(mon.total_size / 1024),
                      (unsigned)((mon.total_size - mon.free_size) / 1024),
                      (int)(100 - (100 * mon.free_size) / mon.total_size));
    }

    if (strlen(g_cfg.wifi_ssid) > 0) {
        xTaskCreatePinnedToCore(net_task, "net", 20480, NULL, 1, NULL, 0);
    }
}

extern "C" void app_set_ann_auto(int on)
{
    g_cfg.ann_auto = on;
    announce_set_auto(on);
    cfg_save(&g_cfg);
}

void loop()
{
    if (web_config_active()) web_config_loop();

    /* 串口单字符调试命令：t=测试播报 r=录音识别 w=配网页 p=真机截屏 s=状态 */
    while (Serial.available()) {
        int c = Serial.read();
        if (c == 't') { Serial.println("[dbg] 触发测试播报"); announce_test(); }
        else if (c == 'r') {
            /* 录音 3 秒：先看电平（判断麦克风是否工作），再尝试送 PC 识别 */
            static uint8_t *vbuf = nullptr;
            if (!vbuf) vbuf = (uint8_t *)heap_caps_malloc(220 * 1024, MALLOC_CAP_SPIRAM);
            Serial.println("[dbg] 录音 3 秒…（说话）");
            int n = audio_record_wav(vbuf, 220 * 1024, 3000);
            if (n > 44) {
                int16_t *pcm = (int16_t *)(vbuf + 44);
                int cnt = (n - 44) / 2;
                double acc = 0; int peak = 0;
                for (int i = 0; i < cnt; i++) {
                    int v = pcm[i]; acc += (double)v * v;
                    if (v > peak) peak = v;
                    if (-v > peak) peak = -v;
                }
                Serial.printf("[dbg] 录音 %d 字节，RMS=%.0f 峰值=%d（RMS<80 基本是静音）\n",
                              n, cnt ? sqrt(acc / cnt) : 0.0, peak);
                if (g_cfg.tts_host[0]) {
                    WiFiClient cli;
                    HTTPClient http;
                    char url[96];
                    snprintf(url, sizeof(url), "http://%s/asr", g_cfg.tts_host);
                    if (http.begin(cli, url)) {
                        http.addHeader("Content-Type", "audio/wav");
                        int code = http.POST(vbuf, n);
                        Serial.printf("[dbg] ASR http %d\n", code);
                        if (code == 200) Serial.printf("[dbg] 识别结果: %s\n", http.getString().c_str());
                        http.end();
                    }
                }
            } else {
                Serial.println("[dbg] 录音失败");
            }
        }
        else if (c == 'w') {
            /* 随时进配网页：浏览器打开打印的地址，可改坐标/WiFi/Key */
            if (!web_config_active()) {
                web_config_start();
                Serial.printf("[dbg] 配网页已开启: http://%s/\n",
                              WiFi.localIP().toString().c_str());
            } else {
                Serial.println("[dbg] 配网页已在运行");
            }
        }
        else if (c == 'p') {
            /* 真机截屏：只标记开始，约 0.9s 后由主循环 dump（不阻塞 LVGL） */
            port_snap_begin();
        }
        else if (c == 'c') {
            app_pages_goto(1, 0);      /* 切到时钟天气页（截屏诊断用） */
        }
        else if (c == 'A') {
            /* 恢复/强制自动播报开，并写入 NVS */
            g_cfg.ann_auto = 1;
            announce_set_auto(1);
            cfg_save(&g_cfg);
            Serial.println("[dbg] 自动播报已恢复 AUTO（已存 NVS）");
        }
        else if (c == 's') {
            const flight_list_t *fl = flight_get();
            Serial.printf("[dbg] 目标 %d 架 | 播报 %s | TTS=%s\n",
                          fl ? fl->count : 0,
                          announce_busy() ? "TX" : "STBY",
                          g_cfg.tts_host[0] ? g_cfg.tts_host : "未配置");
        }
    }

    lv_timer_handler();

    /* 后台结果在主循环落地（LVGL 只在主线程被调用） */
    if (g_new_wx) {
        g_new_wx = false;
        app_pages_set_weather(&g_wx_buf);   /* 数据到位不再响铃（开发者嫌乱） */
    }
    if (g_new_fl) {
        /* 序号锁读取：拷到静态暂存后校验序号未变，才落地给 UI */
        uint32_t s0 = g_fl_seq;
        s_fl_local = g_fl_buf;
        uint32_t s1 = g_fl_seq;
        if (s0 == s1 && (s0 & 1u) == 0u) {
            g_new_fl = false;
            flight_apply_live(&s_fl_local);
        }
        /* 若校验失败（恰逢 net_task 在写）：g_new_fl 仍为 true，下一轮重取最新数据 */
    }

    if (g_new_bd) {
        uint32_t s0 = g_bd_seq;
        s_bd_local = g_bd_buf;
        uint32_t s1 = g_bd_seq;
        if (s0 == s1 && (s0 & 1u) == 0u) {
            g_new_bd = false;
            app_pages_set_board(&s_bd_local);
        }
    }

    if (port_snap_tick()) {
        g_log_mute = 1;
        delay(40);
        port_snap_dump();
        g_log_mute = 0;
    }
    app_pages_tick();

    /* 播报触发：目标进 30km 圈（模块内部做 15 分钟冷却） */
    {
        const flight_list_t *fl = flight_get();
        if (fl && fl->count > 0) {
            for (int i = 0; i < fl->count; i++) {
                const flight_t *f = &fl->items[i];
                if (!f->active) continue;
                float km = f->dist_nm * 1.852f;
                if (km > 0.5f && km <= 30.0f) announce_notify(f, km);
            }
        }
    }

    /* TX/STBY 与播报字幕：只在状态变化时刷新 LVGL */
    {
        static int last_tx = -1;
        static char last_sub[64] = "";
        int tx = announce_busy();
        char sub[64];
        announce_status(sub, sizeof(sub));
        if (tx != last_tx || strcmp(sub, last_sub) != 0) {
            last_tx = tx;
            snprintf(last_sub, sizeof(last_sub), "%s", sub);
            app_pages_set_radio(tx, sub);
        }
    }

    /* 切页音效：统一检测当前页变化，覆盖手势/任何入口 */
    static int s_audio_page = -1;
    int pg = app_pages_current();
    if (s_audio_page >= 0 && pg != s_audio_page) {
        if (pg > s_audio_page) audio_chime();
        else                   audio_blip();
    }
    s_audio_page = pg;

    if (web_config_active() && web_config_should_reboot()) {
        delay(600);
        ESP.restart();
    }

    delay(4);
}
