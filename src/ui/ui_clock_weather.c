/**
 * ui_clock_weather.c —— 时钟天气页（canvas 整帧重绘版 v2）
 *
 * 为什么重写：旧版用 LVGL 控件增量渲染，"只重画变化区域"的机制在部分重绘
 * 状态错乱时产生残影/缺笔画（开发者拍到的乱码）。新方案与雷达页同款管线：
 *   单个 480×480 canvas（PSRAM）+ 每次整帧全量重绘 —— 没有增量状态，
 *   残影/缺字笔画在物理上不可能出现。
 *
 * 独立性：本页只依赖 data_time（本地时钟）和 data_weather（和风），
 * 不引用播报/雷达/网络模块的任何东西。
 *
 * 字体：big128 = Bahnschrift 128px 数字字体（时间/温度），cn_font = 中文 16px。
 */
#include "ui_clock_weather.h"
#include "ui_theme.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#ifdef ESP_PLATFORM
#include <Arduino.h>
#include <esp_heap_caps.h>
#else
#include <stdlib.h>
#define heap_caps_malloc(s, c) malloc(s)
#endif

#include <lvgl.h>

LV_FONT_DECLARE(cn_font);        /* 中文 16px 点阵 */
LV_FONT_DECLARE(big128);         /* Bahnschrift 128px 数字/冒号/度数 */

/* 暖色主题 */
#define CW_BG        lv_color_hex(0xF6EBDD)
#define CW_INK       lv_color_hex(0x7A4230)
#define CW_INK_SOFT  lv_color_hex(0x8A4B36)
#define CW_SUB       lv_color_hex(0xB58E78)
#define CW_ACCENT    lv_color_hex(0xC98E6E)
#define CW_LINE      lv_color_hex(0xE1CDB8)
#define CW_SUN       lv_color_hex(0xF5B841)

static const char *WEEK_CN[7] = { "日", "一", "二", "三", "四", "五", "六" };

static lv_obj_t *s_scr = NULL;
static lv_obj_t *s_canvas = NULL;
static uint16_t *s_buf = NULL;

/* ---------- 内部绘制助手 ---------- */

/* 在 canvas 上写字（LVGL 9.3：init_layer → lv_draw_label → finish_layer） */
static void draw_txt(int x, int y, int w, int h, const lv_font_t *font,
                     lv_color_t color, lv_text_align_t align, const char *txt)
{
    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.font = font;
    dsc.color = color;
    dsc.align = align;
    dsc.text = txt;
    lv_area_t a = { (int32_t)x, (int32_t)y, (int32_t)(x + w - 1), (int32_t)(y + h - 1) };
    lv_layer_t layer;
    lv_canvas_init_layer(s_canvas, &layer);
    lv_draw_label(&layer, &dsc, &a);
    lv_canvas_finish_layer(s_canvas, &layer);
}

/* 画实心圆角矩形（半径=宽高一半即圆） */
static void draw_rect(int x, int y, int w, int h, int radius, lv_color_t color)
{
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = color;
    dsc.bg_opa = LV_OPA_COVER;
    dsc.radius = (int16_t)radius;
    dsc.border_width = 0;
    dsc.shadow_width = 0;
    lv_area_t a = { (int32_t)x, (int32_t)y, (int32_t)(x + w - 1), (int32_t)(y + h - 1) };
    lv_layer_t layer;
    lv_canvas_init_layer(s_canvas, &layer);
    lv_draw_rect(&layer, &dsc, &a);
    lv_canvas_finish_layer(s_canvas, &layer);
}

/* 画横线 */
static void draw_hline(int x, int y, int w, lv_color_t color)
{
    draw_rect(x, y, w, 2, 1, color);
}

/* 太阳 + 云（手绘感：橙圆光晕 + 白云） */
static void draw_sun_cloud(int x, int y)
{
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = CW_SUN;
    dsc.bg_opa = LV_OPA_30;
    dsc.radius = LV_RADIUS_CIRCLE;
    dsc.border_width = 0;
    lv_area_t halo = { (int32_t)x, (int32_t)y, (int32_t)(x + 55), (int32_t)(y + 55) };
    lv_layer_t layer;
    lv_canvas_init_layer(s_canvas, &layer);
    lv_draw_rect(&layer, &dsc, &halo);
    lv_canvas_finish_layer(s_canvas, &layer);

    draw_rect(x + 12, y + 12, 32, 32, 16, CW_SUN);                  /* 太阳 */
    draw_rect(x + 18, y + 30, 20, 20, 10, lv_color_hex(0xFFFFFF));  /* 云朵 */
    draw_rect(x + 8,  y + 40, 40, 14, 7,  lv_color_hex(0xFFFFFF));
}

/* ---------- 整帧重绘 ---------- */
static void redraw(const weather_data_t *d)
{
    char b[64];

    lv_canvas_fill_bg(s_canvas, CW_BG, LV_OPA_COVER);

    if (d) {
        /* 日期 */
        snprintf(b, sizeof(b), "%d月%d日  星期%s", d->mon, d->day, WEEK_CN[d->weekday % 7]);
        draw_txt(34, 22, 300, 22, &cn_font, CW_INK_SOFT, LV_TEXT_ALIGN_LEFT, b);

        /* 天气徽章（右上角） */
        if (d->condition[0])
            draw_txt(388, 20, 64, 22, &cn_font, CW_ACCENT, LV_TEXT_ALIGN_CENTER, d->condition);

        /* 大时间（128px，铺满上半屏）+ 秒数 */
        snprintf(b, sizeof(b), "%02d:%02d", d->hour, d->min);
        draw_txt(12, 62, 320, 140, &big128, CW_INK, LV_TEXT_ALIGN_LEFT, b);
        snprintf(b, sizeof(b), "%02d", d->sec);
        draw_txt(352, 128, 80, 40, &lv_font_montserrat_28, CW_SUB, LV_TEXT_ALIGN_LEFT, b);

        /* 太阳云 + 温度（128px，铺满中腹） */
        draw_sun_cloud(30, 214);
        snprintf(b, sizeof(b), "%d°", d->temp);
        draw_txt(124, 202, 200, 140, &big128, CW_INK_SOFT, LV_TEXT_ALIGN_LEFT, b);

        /* 体感 + 提示（温度右侧） */
        snprintf(b, sizeof(b), "体感 %d°", d->feels_like);
        draw_txt(336, 248, 130, 22, &cn_font, CW_INK_SOFT, LV_TEXT_ALIGN_LEFT, b);
        if (d->condition[0])
            draw_txt(336, 276, 130, 22, &cn_font, CW_ACCENT, LV_TEXT_ALIGN_LEFT, d->condition);
        draw_txt(336, 304, 130, 22, &cn_font, CW_SUB, LV_TEXT_ALIGN_LEFT,
                 (d->wind_scale <= 3) ? "窗外微风" : "风略大添衣");

        /* 分隔线 + 四项指标 */
        draw_hline(24, 336, 432, CW_LINE);
        const char *caps[4] = { "湿度", "风力", "气压", "降水" };
        char vals[4][16];
        snprintf(vals[0], 16, "%d%%", d->humidity);
        snprintf(vals[1], 16, "%d级", d->wind_scale);
        snprintf(vals[2], 16, "%d", d->pressure);
        snprintf(vals[3], 16, "%dmm", d->precip);
        int xs[4] = { 30, 148, 266, 384 };
        for (int i = 0; i < 4; i++) {
            draw_txt(xs[i], 350, 110, 22, &cn_font, CW_SUB, LV_TEXT_ALIGN_LEFT, caps[i]);
            draw_txt(xs[i], 378, 110, 24, &cn_font, CW_INK_SOFT, LV_TEXT_ALIGN_LEFT, vals[i]);
        }

        /* 页脚 */
        snprintf(b, sizeof(b), "数据更新于 %02d:%02d", d->update_hour, d->update_min);
        draw_txt(90, 436, 300, 22, &cn_font, CW_SUB, LV_TEXT_ALIGN_CENTER, b);
    }

    /* 页点（两页：本页 = 第 2 个） */
    draw_rect(228, 463, 6, 6, 3, lv_color_hex(0xC9BBA8));
    draw_rect(242, 462, 8, 8, 4, lv_color_hex(0xC98E6E));
}

/* ---------- 对外接口 ---------- */
lv_obj_t *ui_clock_weather_create(void)
{
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, CW_BG, 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

#ifdef ESP_PLATFORM
    s_buf = (uint16_t *)heap_caps_malloc(480 * 480 * 2, MALLOC_CAP_SPIRAM);
#else
    s_buf = (uint16_t *)malloc(480 * 480 * 2);
#endif
    if (!s_buf) {
        printf("[cw] canvas 缓冲分配失败\n");
        return s_scr;
    }
    memset(s_buf, 0, 480 * 480 * 2);

    s_canvas = lv_canvas_create(s_scr);
    lv_canvas_set_buffer(s_canvas, s_buf, 480, 480, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(s_canvas, 0, 0);
    lv_obj_clear_flag(s_canvas, LV_OBJ_FLAG_SCROLLABLE);

    /* 先画一帧占位（无数据也画背景+页点），避免切页瞬间黑屏 */
    redraw(NULL);
    return s_scr;
}

void ui_clock_weather_update(const weather_data_t *d)
{
    if (!s_canvas || !s_buf || !d) return;
    static weather_data_t last;
    static bool first = true;
    if (!first && memcmp((const void *)d, &last, sizeof(*d)) == 0) return;   /* 无变化不重绘 */
    last = *d;
    first = false;
    redraw(d);
}
