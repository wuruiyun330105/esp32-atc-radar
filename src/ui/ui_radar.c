/**
 * ui_radar.c —— 塔台 PPI 磷光绿雷达页（真实余晖模型）
 *
 * 与「每帧清屏重画」的假雷达不同，这里模拟真实 CRT 荧光屏的物理过程：
 *   · 帧缓冲持久存在，不清屏，每帧整体衰减 → 磷光余辉自然拖尾
 *   · 波束扫过时把该扇区重新激发到最亮 → 一圈扫过，屏幕亮起又暗下
 *   · 目标也遵守同样规律：波束扫到才亮，之后按 glow 衰减（真正的 PPI blip）
 *   · 距离环/方位刻度是压在荧光屏上的印刷标尺，每帧重绘、不衰减
 *   · 中心近距撒地物杂波噪点，模拟 ground clutter
 *
 * 性能：只遍历圆内像素（约 12 万），一遍完成「衰减 + 楔形覆盖」，
 *       480×480 RGB565 在 ESP32-S3 上可跑 20fps（PSRAM 顺序访问 + cache 命中）。
 */
#include "ui_radar.h"
#include "ui_theme.h"
#include "data/data_flight.h"
#include "net/announce.h"
#include "port/port_audio.h"
#include <math.h>
#include <Arduino.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

LV_FONT_DECLARE(cn_font);

#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979
#endif
#define DEG2RAD (M_PI / 180.0)

/* ---------- 几何 ---------- */
#define CX      240
#define CY      240
#define R_OUT   232            /* PPI 圆半径（480 屏直径 464，铺满留 8px 边） */
/* 衰减区覆盖整个屏幕：圆内重写，圆外只做衰减。
 * 之前只比圆大 10px，圆外画过的东西（超量程目标、引导线）会永久留在屏上。 */
#define R_DECAY 340
#define RING_N  5              /* 距离环数；每环 = 量程/RING_N（120NM 时每环 24NM，标签动态生成） */
#define TAIL10  3400           /* 余晖张角 340° (×10)：留 20° 给未扫区，整圈都有残留 */

#define FL_LABEL_MAX 10

/* ---------- 颜色 ---------- */
static inline uint16_t rgb565(int r, int g, int b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
#define C_GRID      rgb565(16, 74, 42)
#define C_GRID_OUT  rgb565(26, 108, 60)
#define C_TICK      rgb565(22, 92, 52)
#define C_TICK_LT   rgb565(40, 150, 82)
#define C_SSR       rgb565(150, 255, 192)
#define C_PRI       rgb565(96, 242, 140)
#define C_SEL       rgb565(255, 255, 255)

/* ---------- 缓冲 ---------- */
static uint16_t *s_buf = NULL;
static int16_t  *s_bearing = NULL;
static uint8_t   s_wlut[TAIL10 + 1];
static uint16_t  s_wcol[256];

static lv_obj_t *s_canvas = NULL;
static lv_obj_t *s_lbl_nesw[4];
static lv_obj_t *s_lbl_ring[RING_N];
static lv_obj_t *s_lbl_top_l, *s_lbl_top_r, *s_lbl_bot_l, *s_lbl_bot_r;
static lv_obj_t *s_fl_line[2];      /* 底部航班条：机场动态 2 行 */
static lv_obj_t *s_tx_lbl;          /* 右上角无线电状态 TX/STBY */

static lv_obj_t *s_lbl_calls[FL_LABEL_MAX];
static lv_obj_t *s_lbl_num[FL_LABEL_MAX];
/* 数据块布局结果（供引导线使用，避免标签压标签） */
static int16_t s_blk_x[FL_LABEL_MAX], s_blk_y[FL_LABEL_MAX];
static uint8_t s_blk_on[FL_LABEL_MAX];

/* 详情卡 */
static lv_obj_t *s_card = NULL;
static lv_obj_t *s_card_cs = NULL, *s_card_sub = NULL, *s_card_v[6] = { 0 };
static char      s_sel_cs[FL_CALL_MAX] = { 0 };
static int       s_sel_live = 0;

/* 状态 */
static float s_sweep = 0.0f;
static uint32_t s_last_tick = 0;
static const flight_list_t *s_fl = NULL;
static char s_status_l[32] = "SIM";
static char s_status_utc[32] = "UTC --:--:--";
static char s_top_l[40] = "ASR-9 PPI";
static int  s_range_nm = 120;
static int  s_frame = 0;

/* ================= 初始化 ================= */
static void init_luts(void)
{
    for (int d = 0; d <= TAIL10; d++) {
        float deg = (float)d / 10.0f;          /* 波束后沿扫过的角度 */
        /* 双时间常数：4° 的快衰减给出扫描线附近的陡降，
         * 48° 的慢衰减拖出长尾。两者叠加才像真实荧光屏。 */
        float v = 0.70f * expf(-deg / 3.5f) + 0.30f * expf(-deg / 105.0f);
        /* 张角末端平滑归零，避免 180° 处出现直边 */
        float x = (float)d / (float)TAIL10;
        float e = (x - 0.90f) / 0.10f;
        if (e > 0.0f) { if (e > 1.0f) e = 1.0f; v *= (1.0f - e * e * (3.0f - 2.0f * e)); }
        s_wlut[d] = (uint8_t)(v * 132.0f);
    }
    /* 磷光色温：暗处深绿、亮处趋白 */
    for (int i = 0; i < 256; i++) {
        float k = (float)i / 255.0f;
        int r = (int)(208.0f * powf(k, 2.35f));
        int g = (int)(255.0f * powf(k, 0.88f));
        int b = (int)(228.0f * powf(k, 1.70f));
        s_wcol[i] = rgb565(r, g, b);
    }
}

static void build_bearing(void)
{
#ifdef ESP_PLATFORM
    s_bearing = (int16_t *)heap_caps_malloc(SCR_W * SCR_H * sizeof(int16_t), MALLOC_CAP_SPIRAM);
#else
    s_bearing = (int16_t *)malloc(SCR_W * SCR_H * sizeof(int16_t));
#endif
    if (!s_bearing) return;
    int R2 = R_OUT * R_OUT;
    int RD2 = R_DECAY * R_DECAY;
    for (int y = 0; y < SCR_H; y++) {
        for (int x = 0; x < SCR_W; x++) {
            int dx = x - CX, dy = y - CY;
            int d2 = dx * dx + dy * dy;
            if (d2 > RD2 || d2 > R2) { s_bearing[y * SCR_W + x] = -1; continue; }
            float b = atan2f((float)dx, (float)-dy) * (180.0f / (float)M_PI);
            if (b < 0) b += 360.0f;
            s_bearing[y * SCR_W + x] = (int16_t)(b * 10.0f);
        }
    }
}

/* ================= 像素工具 ================= */
static inline void px(int x, int y, uint16_t c)
{
    if ((unsigned)x < SCR_W && (unsigned)y < SCR_H)
        s_buf[y * SCR_W + x] = c;
}

static inline uint16_t scale565(uint16_t c, float k)
{
    if (k <= 0.0f) return 0;
    if (k > 1.0f) k = 1.0f;
    int r = (int)(((c >> 11) & 0x1F) * k);
    int g = (int)(((c >> 5) & 0x3F) * k);
    int b = (int)((c & 0x1F) * k);
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static void line(int x0, int y0, int x1, int y1, uint16_t c)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        px(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void circle(int cx, int cy, int r, uint16_t c)
{
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        px(cx + x, cy + y, c); px(cx + y, cy + x, c);
        px(cx - y, cy + x, c); px(cx - x, cy + y, c);
        px(cx - x, cy - y, c); px(cx - y, cy - x, c);
        px(cx + y, cy - x, c); px(cx + x, cy - y, c);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

static void block(int cx, int cy, int h, uint16_t c)
{
    for (int y = -h; y <= h; y++)
        for (int x = -h; x <= h; x++)
            px(cx + x, cy + y, c);
}

/* ================= 主渲染：衰减 + 波束激发（单遍遍历） ================= */
static void render_disc(void)
{
    if (!s_bearing) return;
    int RD2 = R_DECAY * R_DECAY;
    int sw10 = (int)(s_sweep * 10.0f);
    int y0 = CY - R_DECAY; if (y0 < 0) y0 = 0;
    int y1 = CY + R_DECAY; if (y1 > SCR_H - 1) y1 = SCR_H - 1;
    for (int y = y0; y <= y1; y++) {
        int dy = y - CY;
        int half = (int)sqrtf((float)(RD2 - dy * dy));
        int x0 = CX - half, x1 = CX + half;
        if (x0 < 0) x0 = 0;
        if (x1 > SCR_W - 1) x1 = SCR_W - 1;
        uint16_t *row = s_buf + y * SCR_W;
        const int16_t *brow = s_bearing + y * SCR_W;
        for (int x = x0; x <= x1; x++) {
            int b10 = brow[x];
            if (b10 >= 0) {
                int d = sw10 - b10;
                if (d < 0) d += 3600;
                if (d < TAIL10) { row[x] = s_wcol[s_wlut[d]]; continue; }
            }
            uint16_t c = row[x];
            if (!c) continue;
            uint16_t r = (c >> 11) & 0x1F;
            uint16_t g = (c >> 5) & 0x3F;
            uint16_t b = c & 0x1F;
            row[x] = (uint16_t)((((r * 15) >> 4) << 11) |
                                (((g * 15) >> 4) << 5) | ((b * 15) >> 4));
        }
    }
}

/* 地物杂波 + 屏噪：中心近距撒点形成闪烁绿雾，全盘散布极暗颗粒 */
static void draw_clutter(void)
{
    static uint32_t seed = 0x1F35A7u;
    /* 近距地物回波 */
    for (int i = 0; i < 62; i++) {
        seed = seed * 1103515245u + 12345u;
        int ang = (int)((seed >> 8) % 3600);
        seed = seed * 1103515245u + 12345u;
        int rad = 15 + (int)((seed >> 8) % 46);
        seed = seed * 1103515245u + 12345u;
        int br = 30 + (int)((seed >> 9) % 95);
        float a = (float)ang / 10.0f * DEG2RAD;
        int x = CX + (int)(sinf(a) * rad);
        int y = CY - (int)(cosf(a) * rad);
        px(x, y, s_wcol[br]);
        if ((seed & 3) == 0) px(x + 1, y, s_wcol[br / 2]);
    }
    /* 全盘 CRT 颗粒噪声：只落在暗处，不脏化波束和标尺 */
    for (int i = 0; i < 260; i++) {
        seed = seed * 1103515245u + 12345u;
        int ang = (int)((seed >> 8) % 3600);
        seed = seed * 1103515245u + 12345u;
        int rr = (int)((seed >> 8) % 10000);
        float rf = sqrtf((float)rr / 10000.0f) * (float)R_OUT;
        seed = seed * 1103515245u + 12345u;
        int br = 3 + (int)((seed >> 10) % 20);
        float a = (float)ang / 10.0f * DEG2RAD;
        int x = CX + (int)(sinf(a) * rf);
        int y = CY - (int)(cosf(a) * rf);
        if ((unsigned)x >= SCR_W || (unsigned)y >= SCR_H) continue;
        uint16_t cur = s_buf[y * SCR_W + x];
        if (cur < 300) s_buf[y * SCR_W + x] = s_wcol[br];
    }
}

/* 印刷标尺：距离环 + 十字 + 方位刻度 + 中心点 */
static void draw_static(void)
{
    for (int i = 1; i <= RING_N; i++) {
        int r = R_OUT * i / RING_N;
        circle(CX, CY, r, i == RING_N ? C_GRID_OUT : C_GRID);
    }
    for (int d = -R_OUT; d <= R_OUT; d += 2) {
        px(CX + d, CY, C_GRID);
        px(CX, CY + d, C_GRID);
    }
    for (int a = 0; a < 360; a += 5) {
        float rad = (a - 90) * DEG2RAD;
        int len = (a % 30 == 0) ? 13 : (a % 10 == 0 ? 8 : 4);
        uint16_t c = (a % 30 == 0) ? C_TICK_LT : C_TICK;
        int x1 = CX + (int)(cosf(rad) * (R_OUT + 1));
        int y1 = CY + (int)(sinf(rad) * (R_OUT + 1));
        int x2 = CX + (int)(cosf(rad) * (R_OUT + 1 + len));
        int y2 = CY + (int)(sinf(rad) * (R_OUT + 1 + len));
        line(x1, y1, x2, y2, c);
    }
    block(CX, CY, 1, C_TICK_LT);
}

/* 目标 + 尾迹 + 速度矢量 */
static void draw_targets(void)
{
    if (!s_fl) return;
    float nm_per_px = (float)(s_range_nm > 0 ? s_range_nm : 120) / (float)R_OUT;
    for (int i = 0; i < s_fl->count; i++) {
        const flight_t *f = &s_fl->items[i];
        if (!f->active) continue;
        int x = (int)(f->x + 0.5f);
        int y = (int)(f->y + 0.5f);
        if ((unsigned)x >= SCR_W || (unsigned)y >= SCR_H) continue;
        /* 量程外不画：真实雷达也只显示量程内的目标，
         * 而且圆外像素不在衰减区，画上去会变成永久残影 */
        float ddx = f->x - CX, ddy = f->y - CY;
        if (ddx * ddx + ddy * ddy > (float)(R_OUT - 4) * (R_OUT - 4)) continue;

        float g = f->glow;
        if (g < 0.26f) g = 0.26f;
        uint16_t base = f->is_ssr ? C_SSR : C_PRI;

        /* 引导线：目标 → 数据块左下角（雷达屏幕的经典画法） */
        if (i < FL_LABEL_MAX && s_blk_on[i]) {
            uint16_t lc = scale565(base, g * 0.55f);
            int ax = s_blk_x[i], ay = s_blk_y[i] + 22;
            line(x, y, ax + 2, ay, lc);
            px(ax, ay, scale565(base, g * 0.5f));
        }
        int selected = (s_sel_live && s_sel_cs[0] &&
                        strcmp(f->callsign, s_sel_cs) == 0);

        /* 历史尾迹：越旧越暗 */
        for (int k = 0; k < f->hist_n; k++) {
            float kk = 0.62f * (1.0f - (float)k / (float)FL_HIST_N);
            uint16_t hc = scale565(base, kk * g);
            int tx = (int)(f->hist[k][0] + 0.5f), ty = (int)(f->hist[k][1] + 0.5f);
            px(tx, ty, hc); px(tx + 1, ty, hc);
            px(tx, ty + 1, hc); px(tx + 1, ty + 1, hc);
        }

        /* 速度矢量：按 2 分钟航程比例 */
        float vlen = (f->spd * 2.0f / 60.0f) / nm_per_px;
        if (vlen < 6.0f) vlen = 6.0f;
        if (vlen > 40.0f) vlen = 40.0f;
        float rad = (f->hdg - 90.0f) * DEG2RAD;
        uint16_t vc = scale565(base, g * 0.8f);
        line(x, y, x + (int)(cosf(rad) * vlen), y + (int)(sinf(rad) * vlen), vc);

        /* 符号：二次雷达=圆点带环，一次雷达=实心方 */
        uint16_t sc = scale565(selected ? C_SEL : base, g);
        if (f->is_ssr) {
            block(x, y, 1, sc);
            circle(x, y, 5, scale565(base, g * 0.5f));
        } else {
            block(x, y, 2, sc);
        }

        /* 选中：四角括号 */
        if (selected) {
            int r = 9, arm = 4;
            for (int s = -1; s <= 1; s += 2) {
                line(x + s * r, y - r, x + s * (r - arm), y - r, C_SEL);
                line(x + s * r, y - r, x + s * r, y - r + arm, C_SEL);
                line(x + s * r, y + r, x + s * (r - arm), y + r, C_SEL);
                line(x + s * r, y + r, x + s * r, y + r - arm, C_SEL);
            }
        }
    }
}

/* 扫描前沿亮线 */
static void draw_sweep_edge(void)
{
    /* 只留一条细亮线：真机扫描线是 1° 内的窄束，画两条会糊成光锥的硬边 */
    float rad = (s_sweep - 90.0f) * DEG2RAD;
    line(CX, CY, CX + (int)(cosf(rad) * R_OUT), CY + (int)(sinf(rad) * R_OUT),
         s_wcol[232]);
    float r2 = (s_sweep - 2.6f - 90.0f) * DEG2RAD;
    line(CX, CY, CX + (int)(cosf(r2) * R_OUT), CY + (int)(sinf(r2) * R_OUT),
         s_wcol[96]);
}

/* ================= LVGL 控件 ================= */
static lv_obj_t *mk_label(lv_obj_t *parent, const char *txt, lv_color_t col,
                          const lv_font_t *font)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, col, 0);
    lv_obj_set_style_text_font(l, font, 0);
    return l;
}

static lv_obj_t *mk_card_row(lv_obj_t *card, int x, int y, const char *cap,
                             const char *val)
{
    lv_obj_t *c = mk_label(card, cap, lv_color_hex(0x2F9C60), &lv_font_montserrat_14);
    lv_obj_set_pos(c, x, y);
    lv_obj_t *v = mk_label(card, val, RD_GREEN_LT, &lv_font_montserrat_24);
    lv_obj_set_pos(v, x, y + 17);
    return v;
}

static void on_card_click(lv_event_t *e) { (void)e; ui_radar_card_close(); }

static void card_build(lv_obj_t *scr)
{
    s_card = lv_obj_create(scr);
    lv_obj_set_size(s_card, 420, 268);
    lv_obj_set_pos(s_card, 30, 106);
    lv_obj_set_style_bg_color(s_card, lv_color_hex(0x04120A), 0);
    lv_obj_set_style_bg_opa(s_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_card, 1, 0);
    lv_obj_set_style_outline_width(s_card, 3, 0);
    lv_obj_set_style_outline_color(s_card, lv_color_hex(0x123A24), 0);
    lv_obj_set_style_outline_opa(s_card, LV_OPA_60, 0);
    lv_obj_set_style_border_color(s_card, RD_GREEN_DIM, 0);
    lv_obj_set_style_radius(s_card, 4, 0);
    lv_obj_set_style_pad_all(s_card, 0, 0);
    lv_obj_clear_flag(s_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_card, LV_OBJ_FLAG_HIDDEN);

    s_card_cs = mk_label(s_card, "--------", RD_GREEN_LT, &lv_font_montserrat_28);
    lv_obj_set_pos(s_card_cs, 18, 10);

    s_card_sub = mk_label(s_card, "", RD_GREEN_DIM, &lv_font_montserrat_14);
    lv_obj_set_pos(s_card_sub, 19, 48);

    lv_obj_t *ln = lv_obj_create(s_card);
    lv_obj_set_size(ln, 388, 1);
    lv_obj_set_pos(ln, 16, 70);
    lv_obj_set_style_bg_color(ln, RD_GREEN_DIM, 0);
    lv_obj_set_style_bg_opa(ln, LV_OPA_60, 0);
    lv_obj_set_style_border_width(ln, 0, 0);
    lv_obj_set_style_radius(ln, 0, 0);
    lv_obj_clear_flag(ln, LV_OBJ_FLAG_SCROLLABLE);

    s_card_v[0] = mk_card_row(s_card, 18, 84, "ALT (FT)", "-----");
    s_card_v[1] = mk_card_row(s_card, 156, 84, "GS (KT)", "---");
    s_card_v[2] = mk_card_row(s_card, 292, 84, "HDG", "---");
    s_card_v[3] = mk_card_row(s_card, 18, 146, "V/S (FPM)", "-----");
    s_card_v[4] = mk_card_row(s_card, 156, 146, "DIST (NM)", "---");
    s_card_v[5] = mk_card_row(s_card, 292, 146, "BRG", "---");

    lv_obj_t *ln2 = lv_obj_create(s_card);
    lv_obj_set_size(ln2, 388, 1);
    lv_obj_set_pos(ln2, 16, 218);
    lv_obj_set_style_bg_color(ln2, RD_GREEN_DIM, 0);
    lv_obj_set_style_bg_opa(ln2, LV_OPA_40, 0);
    lv_obj_set_style_border_width(ln2, 0, 0);
    lv_obj_set_style_radius(ln2, 0, 0);
    lv_obj_clear_flag(ln2, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hint = mk_label(s_card, "TAP ANYWHERE TO CLOSE", lv_color_hex(0x1E6B45),
                              &lv_font_montserrat_10);
    lv_obj_set_pos(hint, 18, 232);

    lv_obj_add_flag(s_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_card, on_card_click, LV_EVENT_CLICKED, NULL);
}

static void card_fill(const flight_t *f)
{
    if (!s_card || !f) return;
    lv_label_set_text(s_card_cs, f->callsign[0] ? f->callsign : "--------");

    char sub[80], hex[12];
    const char *type = f->type[0] ? f->type : "AIRCRAFT";
    const char *mode = f->is_ssr ? "MODE S" : "PRIMARY";
    int hn = 0;
    for (; f->icao[hn] && hn < (int)sizeof(hex) - 1; hn++) {
        char ch = f->icao[hn];
        hex[hn] = (ch >= 'a' && ch <= 'z') ? (char)(ch - 32) : ch;
    }
    hex[hn] = 0;
    if (f->squawk[0] && hex[0])
        snprintf(sub, sizeof(sub), "%s  |  %s  |  %s  |  SQ %s", type, mode, hex, f->squawk);
    else if (f->squawk[0])
        snprintf(sub, sizeof(sub), "%s  |  %s  |  SQ %s", type, mode, f->squawk);
    else
        snprintf(sub, sizeof(sub), "%s  |  %s", type, mode);
    lv_label_set_text(s_card_sub, sub);

    char b[24];
    snprintf(b, sizeof(b), "%d", f->alt * 100);
    lv_label_set_text(s_card_v[0], b);
    snprintf(b, sizeof(b), "%d", (int)(f->spd + 0.5f));
    lv_label_set_text(s_card_v[1], b);
    snprintf(b, sizeof(b), "%03d", ((int)(f->hdg + 0.5f)) % 360);
    lv_label_set_text(s_card_v[2], b);
    if (f->vs) snprintf(b, sizeof(b), "%+d", f->vs);
    else       snprintf(b, sizeof(b), "0");
    lv_label_set_text(s_card_v[3], b);
    snprintf(b, sizeof(b), "%.1f", f->dist_nm);
    lv_label_set_text(s_card_v[4], b);
    snprintf(b, sizeof(b), "%03d", ((int)(f->brg + 0.5f)) % 360);
    lv_label_set_text(s_card_v[5], b);
}

static const flight_t *find_by_callsign(const char *cs)
{
    if (!s_fl || !cs || !cs[0]) return NULL;
    for (int i = 0; i < s_fl->count; i++)
        if (s_fl->items[i].active && strcmp(s_fl->items[i].callsign, cs) == 0)
            return &s_fl->items[i];
    return NULL;
}

void ui_radar_card_close(void)
{
    if (s_card) lv_obj_add_flag(s_card, LV_OBJ_FLAG_HIDDEN);
    s_sel_live = 0;
    s_sel_cs[0] = 0;
}

int ui_radar_card_open(void) { return s_sel_live; }

/* 调试/自动化用：直接选中第 i 个目标并弹卡 */
void ui_radar_select_index(int i)
{
    const flight_t *f = flight_at(i);
    if (!f || !s_card) return;
    snprintf(s_sel_cs, sizeof(s_sel_cs), "%s", f->callsign);
    s_sel_live = 1;
    card_fill(f);
    lv_obj_clear_flag(s_card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_card);
}

static void on_canvas_click(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    printf("[radar] 点击 (%d,%d)\n", p.x, p.y);
    int idx = flight_pick(p.x, p.y, 44);
    if (idx < 0) { ui_radar_card_close(); return; }
    const flight_t *f = flight_at(idx);
    if (!f) return;
    snprintf(s_sel_cs, sizeof(s_sel_cs), "%s", f->callsign);
    s_sel_live = 1;
    card_fill(f);
    lv_obj_clear_flag(s_card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_card);
    /* 点击即播：立刻用塔台口吻语音播报这架飞机 */
    announce_speak_now(f);
}

/* 右上角 AUTO/STBY 开关：点击切换"进圈自动播报" */
static void on_tx_click(lv_event_t *e)
{
    (void)e;
    int on = announce_auto() ? 0 : 1;
    announce_set_auto(on);
    extern void app_set_ann_auto(int on);   /* main_esp32.cpp：写入 g_cfg 并存 NVS */
    app_set_ann_auto(on);
    if (s_tx_lbl) {
        lv_label_set_text(s_tx_lbl, on ? "AUTO" : "STBY");
        lv_obj_set_style_text_color(s_tx_lbl,
            on ? lv_color_hex(0x3FAE79) : lv_color_hex(0x2E7D52), 0);
    }
    if (on) audio_chime(); else audio_blip();
    printf("[radar] 自动播报: %s\n", on ? "AUTO" : "STBY");
}

/* ================= 创建页面 ================= */
lv_obj_t *ui_radar_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

#ifdef ESP_PLATFORM
    s_buf = (uint16_t *)heap_caps_malloc(SCR_W * SCR_H * 2, MALLOC_CAP_SPIRAM);
#else
    s_buf = (uint16_t *)malloc(SCR_W * SCR_H * 2);
#endif
    memset(s_buf, 0, SCR_W * SCR_H * 2);

    init_luts();
    build_bearing();

    s_canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(s_canvas, s_buf, SCR_W, SCR_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(s_canvas, 0, 0);
    lv_obj_clear_flag(s_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_canvas, on_canvas_click, LV_EVENT_CLICKED, NULL);

    s_lbl_top_l = mk_label(scr, "ASR-9  PPI", RD_GREEN_DIM, &lv_font_montserrat_10);
    lv_obj_set_pos(s_lbl_top_l, 12, 8);
    s_lbl_top_r = mk_label(scr, "UTC --:--:--", RD_GREEN_DIM, &lv_font_montserrat_10);
    lv_obj_align(s_lbl_top_r, LV_ALIGN_TOP_RIGHT, -12, 8);

    s_lbl_bot_l = mk_label(scr, "SIM", lv_color_hex(0x2E7D52), &lv_font_montserrat_10);
    lv_obj_align(s_lbl_bot_l, LV_ALIGN_BOTTOM_LEFT, 12, -8);
    s_lbl_bot_r = mk_label(scr, "TGT 00", RD_GREEN_DIM, &lv_font_montserrat_10);
    lv_obj_align(s_lbl_bot_r, LV_ALIGN_BOTTOM_RIGHT, -14, -32);   /* 给开关让位 */

    /* 播报开关放右下角（右上角不好点）：AUTO=进圈自动播报 / STBY=只点击播报 */
    s_tx_lbl = mk_label(scr, "AUTO", lv_color_hex(0x3FAE79), &lv_font_montserrat_14);
    lv_obj_align(s_tx_lbl, LV_ALIGN_BOTTOM_RIGHT, -14, -8);
    lv_obj_set_style_pad_hor(s_tx_lbl, 6, 0);
    lv_obj_set_style_pad_ver(s_tx_lbl, 4, 0);
    lv_obj_set_style_bg_color(s_tx_lbl, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_tx_lbl, LV_OPA_40, 0);
    lv_obj_add_flag(s_tx_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_tx_lbl, on_tx_click, LV_EVENT_CLICKED, NULL);

    /* 底部航班条（原第 4 页并入：虹桥/浦东动态 2 行） */
    for (int i = 0; i < 2; i++) {
        s_fl_line[i] = mk_label(scr, "", lv_color_hex(0x2E7D52), &cn_font);
        lv_obj_set_pos(s_fl_line[i], 14, 418 + i * 20);
        lv_obj_set_style_bg_color(s_fl_line[i], lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(s_fl_line[i], LV_OPA_60, 0);
    }

    const char *nesw[4] = { "N", "E", "S", "W" };
    const int nx[4] = { 0, 220, 0, -220 };
    const int ny[4] = { -220, 0, 220, 0 };
    for (int i = 0; i < 4; i++) {
        s_lbl_nesw[i] = mk_label(scr, nesw[i], lv_color_hex(0x2E7D52),
                                 &lv_font_montserrat_12);
        lv_obj_set_pos(s_lbl_nesw[i], CX + nx[i] - 4, CY + ny[i] - 7);
    }

    for (int i = 1; i <= RING_N; i++) {
        char b[8];
        snprintf(b, sizeof(b), "%d", i * (s_range_nm / RING_N));
        s_lbl_ring[i - 1] = mk_label(scr, b, lv_color_hex(0x2A8A5C), &lv_font_montserrat_10);
        /* 黑底打断环线：真实雷达的环标注就是压在环上、把环断开 */
        lv_obj_set_style_bg_color(s_lbl_ring[i - 1], lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(s_lbl_ring[i - 1], LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(s_lbl_ring[i - 1], 1, 0);
        lv_obj_set_style_pad_ver(s_lbl_ring[i - 1], 0, 0);
        int r = R_OUT * i / RING_N;
        lv_obj_set_pos(s_lbl_ring[i - 1],
                       CX - (int)(r * 0.7071f) - 20,
                       CY - (int)(r * 0.7071f) - 11);
    }

    for (int i = 0; i < FL_LABEL_MAX; i++) {
        s_lbl_calls[i] = mk_label(scr, "", lv_color_hex(0xE4FFF0), &lv_font_montserrat_16);
        s_lbl_num[i]   = mk_label(scr, "", RD_GREEN, &lv_font_montserrat_14);
        lv_obj_add_flag(s_lbl_calls[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_lbl_num[i], LV_OBJ_FLAG_HIDDEN);
    }

    card_build(scr);
    return scr;
}

/* ================= 对外 ================= */
void ui_radar_set_flights(const flight_list_t *fl) { s_fl = fl; }

/* 无线电状态 + 播报字幕 */
void ui_radar_set_radio(int tx, const char *subtitle)
{
    (void)subtitle;
    if (s_tx_lbl) {
        if (tx) {
            lv_label_set_text(s_tx_lbl, "TX");
            lv_obj_set_style_text_color(s_tx_lbl, lv_color_hex(0x7CFFB2), 0);
        } else {
            lv_label_set_text(s_tx_lbl, announce_auto() ? "AUTO" : "STBY");
            lv_obj_set_style_text_color(s_tx_lbl,
                announce_auto() ? lv_color_hex(0x3FAE79) : lv_color_hex(0x2E7D52), 0);
        }
    }
}

/* 底部航班条：显示机场动态前 2 条（航班号 + 目的地 + 时刻 + 状态） */
void ui_radar_set_board(const board_list_t *b)
{
    if (!b || !s_fl_line[0]) return;
    if (!b->valid || b->count == 0) {
        lv_label_set_text(s_fl_line[0], "");
        lv_label_set_text(s_fl_line[1], "");
        return;
    }
    for (int i = 0; i < 2; i++) {
        if (i >= b->count) { lv_label_set_text(s_fl_line[i], ""); continue; }
        const board_item_t *it = &b->items[i];
        char buf[96];
        const char *tm = it->est[0] ? it->est : it->plan;
        snprintf(buf, sizeof(buf), "%s %s  %s%s",
                 it->fnum, it->dest, tm,
                 it->delay[0] == '+' ? " 延误" : "");
        lv_label_set_text(s_fl_line[i], buf);
        lv_obj_set_style_text_color(s_fl_line[i],
            strstr(it->status, "取消") ? lv_color_hex(0xC0503C)
          : (strstr(it->status, "延误") || it->delay[0] == '+')
                ? lv_color_hex(0xD98A2B) : lv_color_hex(0x3FAE79), 0);
    }
}

void ui_radar_set_status(const char *left_txt, const char *utc_txt)
{
    if (left_txt) snprintf(s_status_l, sizeof(s_status_l), "%s", left_txt);
    if (utc_txt)  snprintf(s_status_utc, sizeof(s_status_utc), "%s", utc_txt);
}

void ui_radar_set_range(int range_nm)
{
    if (range_nm <= 0) return;
    if (s_range_nm == range_nm) return;
    s_range_nm = range_nm;
    snprintf(s_top_l, sizeof(s_top_l), "ASR-9  PPI / %dNM", range_nm);
    if (s_lbl_top_l) lv_label_set_text(s_lbl_top_l, s_top_l);
    for (int i = 1; i <= RING_N; i++) {
        if (!s_lbl_ring[i - 1]) continue;
        char b[8];
        snprintf(b, sizeof(b), "%d", i * (range_nm / RING_N));
        lv_label_set_text(s_lbl_ring[i - 1], b);
    }
}

void ui_radar_tick(void)
{
    if (!s_buf || !s_bearing) return;

    uint32_t now = lv_tick_get();
    float dt = s_last_tick ? (now - s_last_tick) / 1000.0f : 0.05f;
    if (dt > 0.4f) dt = 0.4f;
    if (dt <= 0.0f) dt = 0.005f;
    s_last_tick = now;

    render_disc();
    draw_clutter();
    draw_static();
    draw_targets();
    draw_sweep_edge();
    lv_obj_invalidate(s_canvas);

#ifdef ESP_PLATFORM
    /* 到货实测：每 10s 报一次真实帧率（设计目标 18~20fps，以串口日志为准） */
    static uint32_t fps_t0 = 0, fps_frames = 0;
    fps_frames++;
    if (now - fps_t0 >= 10000) {
        printf("[radar] 实测 %.1f fps\n", fps_frames * 1000.0f / (float)(now - fps_t0));
        fps_t0 = now;
        fps_frames = 0;
    }
#endif

    /* 12 rpm ≈ 5s/圈（真实一次雷达 12~15rpm） */
    s_sweep += 72.0f * dt;
    if (s_sweep >= 360.0f) s_sweep -= 360.0f;
    flight_excite(s_sweep);

    /* 数据块：每 4 帧更新，减少文字重排 */
    s_frame++;
    if (s_frame % 4 == 0) {
        /* 数据块避让：每块 58×26，按 6 个候选位逐个试，避开已占位 */
        int16_t occ[FL_LABEL_MAX][4];
        int nocc = 0;
        for (int i = 0; i < FL_LABEL_MAX; i++) {
            const flight_t *f = (s_fl && i < s_fl->count) ? &s_fl->items[i] : NULL;
            if (!f || !f->active) {
                s_blk_on[i] = 0;
                lv_obj_add_flag(s_lbl_calls[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_lbl_num[i], LV_OBJ_FLAG_HIDDEN);
                continue;
            }
            lv_label_set_text(s_lbl_calls[i], f->callsign);
            char b[20];
            snprintf(b, sizeof(b), "%d  %d", f->alt, (int)(f->spd + 0.5f));
            lv_label_set_text(s_lbl_num[i], b);

            /* 候选位分三圈：近位、中位、远位。目标越挤（如机场进近），
             * 标签被推到外圈，靠引导线连回去——真实航管屏就是这么排的。 */
            static const int8_t OFF[12][2] = {
                { 13, -28 }, { -88, -28 }, { 13, 12 }, { -88, 12 },
                { 13, -62 }, { -88, -62 }, { 13, 46 }, { -88, 46 },
                { 16, -108 }, { -91, -108 }, { 16, 88 }, { -91, 88 }
            };
            int bx = 0, by = 0, ok = 0;
            for (int c = 0; c < 12 && !ok; c++) {
                int cx2 = (int)f->x + OFF[c][0];
                int cy2 = (int)f->y + OFF[c][1];
                if (cx2 < 3) cx2 = 3;
                if (cx2 > SCR_W - 79) cx2 = SCR_W - 79;
                if (cy2 < 19) cy2 = 19;
                if (cy2 > SCR_H - 47) cy2 = SCR_H - 47;
                int clash = 0;
                for (int k = 0; k < nocc; k++) {
                    /* 块宽 76：7 位呼号在 12px 字号下约 62px，留出字距 */
                    if (cx2 < occ[k][2] + 2 && cx2 + 76 > occ[k][0] - 2 &&
                        cy2 < occ[k][3] + 2 && cy2 + 28 > occ[k][1] - 1) { clash = 1; break; }
                }
                if (!clash) { bx = cx2; by = cy2; ok = 1; }
            }
            if (!ok) {
                /* 12 个位置全被占：这条干脆不标。
                 * 宁可少标几条，也不能让字叠成一团——符号还在，点一下照样看详情。 */
                s_blk_on[i] = 0;
                lv_obj_add_flag(s_lbl_calls[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_lbl_num[i], LV_OBJ_FLAG_HIDDEN);
                continue;
            }
            if (nocc < FL_LABEL_MAX) {
                occ[nocc][0] = (int16_t)bx; occ[nocc][1] = (int16_t)by;
                occ[nocc][2] = (int16_t)(bx + 76); occ[nocc][3] = (int16_t)(by + 28);
                nocc++;
            }
            s_blk_x[i] = (int16_t)bx;
            s_blk_y[i] = (int16_t)by;
            s_blk_on[i] = 1;
            lv_obj_set_pos(s_lbl_calls[i], bx, by);
            lv_obj_set_pos(s_lbl_num[i], bx, by + 15);
            lv_obj_clear_flag(s_lbl_calls[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_lbl_num[i], LV_OBJ_FLAG_HIDDEN);
        }
        char t[16];
        snprintf(t, sizeof(t), "TGT %02d", s_fl ? s_fl->count : 0);
        lv_label_set_text(s_lbl_bot_r, t);
    }

    /* 状态文字：值未变就不重排 */
    static char last_l[32] = { 0 }, last_u[32] = { 0 };
    if (strcmp(last_l, s_status_l) != 0) {
        snprintf(last_l, sizeof(last_l), "%s", s_status_l);
        lv_label_set_text(s_lbl_bot_l, last_l);
    }
    if (strcmp(last_u, s_status_utc) != 0) {
        snprintf(last_u, sizeof(last_u), "%s", s_status_utc);
        lv_label_set_text(s_lbl_top_r, last_u);
    }
    static int last_top_inited = 0;
    if (!last_top_inited) {
        last_top_inited = 1;
        snprintf(s_top_l, sizeof(s_top_l), "ASR-9  PPI / %dNM", s_range_nm);
        lv_label_set_text(s_lbl_top_l, s_top_l);
    }

    /* 详情卡跟随数据刷新 */
    if (s_sel_live && (s_frame % 20 == 0)) {
        const flight_t *f = find_by_callsign(s_sel_cs);
        if (f) card_fill(f);
        else ui_radar_card_close();
    }
}
