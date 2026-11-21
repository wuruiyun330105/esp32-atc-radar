/**
 * ui_theme.h —— 全局配色与尺寸常量（各页共用）
 * 屏幕：480×480 方屏
 */
#ifndef UI_THEME_H
#define UI_THEME_H

#include "lvgl.h"

/* ---------- 屏幕 ---------- */
#define SCR_W 480
#define SCR_H 480

/* ---------- 雷达页：塔台 PPI 磷光绿 ---------- */
#define RD_BG          lv_color_hex(0x020502)
#define RD_GREEN       lv_color_hex(0x5EF08A)   /* 主磷光绿 */
#define RD_GREEN_DIM   lv_color_hex(0x2F9C60)   /* 暗绿（刻度/辅助） */
#define RD_GREEN_LT    lv_color_hex(0x9DFFC0)   /* 亮绿（N/S/E/W） */
#define RD_CYAN        lv_color_hex(0xA9FFD0)   /* 二次雷达目标（偏青） */
#define RD_AMBER       lv_color_hex(0xFFC266)   /* 天气/告警琥珀 */
#define RD_TXT         lv_color_hex(0xBFFFD6)   /* 数据块文字 */

/* ---------- 时钟天气页：暖色奶油 ---------- */
#define CW_BG          lv_color_hex(0xF6EBDD)
#define CW_INK         lv_color_hex(0x7A4230)   /* 主文字砖红 */
#define CW_INK_SOFT    lv_color_hex(0x8A4B36)
#define CW_SUB         lv_color_hex(0xB58E78)   /* 次要文字 */
#define CW_ACCENT      lv_color_hex(0xC98E6E)   /* 冒号/度数 */
#define CW_SUN         lv_color_hex(0xEDB24E)   /* 太阳 */
#define CW_SUN_RAY     lv_color_hex(0xE0A23C)
#define CW_BADGE_BG    lv_color_hex(0xF0D9C0)
#define CW_BADGE_BD    lv_color_hex(0xE3C3A2)
#define CW_LINE        lv_color_hex(0xE1CDB8)

/* ---------- 预留配色 ---------- */
#define AI_BG          lv_color_hex(0xEDEAE3)
#define AI_INK         lv_color_hex(0x3A3A38)
#define AI_AVATAR      lv_color_hex(0x5B6B63)
#define AI_AVATAR_TX   lv_color_hex(0xF2F0EA)
#define AI_GREEN       lv_color_hex(0x7FA98C)   /* 在线点/波形 */
#define AI_GREEN_TX    lv_color_hex(0x8A9A90)
#define AI_BUBBLE_AI   lv_color_hex(0xFBFAF7)
#define AI_BUBBLE_BD   lv_color_hex(0xDEDAD1)
#define AI_LINE        lv_color_hex(0xD6D2C9)

#endif
