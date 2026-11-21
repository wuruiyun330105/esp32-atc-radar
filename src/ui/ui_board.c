/**
 * ui_board.c —— 航班动态页（暖色奶油风，与时钟页同一色板）
 *
 * 版式：
 *   ┌────────────────────────────┐
 *   │ 航班动态   [SHA·上海虹桥]   │  标题 + 机场徽章
 *   │ 更新 19:32 · 未来出发        │  副标题
 *   │ MU5315  →广州白云   19:35  登机结束 │
 *   │ ...                         │  可滚动列表（10 条）
 *   └────────────────────────────┘
 * 状态配色：取消=红 延误=琥珀 其余=绿
 */
#include "ui_board.h"
#include "ui_theme.h"
#include "data/data_time.h"
#include <stdio.h>
#include <string.h>

LV_FONT_DECLARE(cn_font);

static lv_obj_t *s_ap_badge;    /* 机场徽章文本 */
static lv_obj_t *s_sub;         /* 副标题（更新时间） */
static lv_obj_t *s_list;        /* 条目容器（可滚动） */
static lv_obj_t *s_hint;        /* 空态提示 */

static lv_color_t status_color(const char *st, const char *delay)
{
    if (strstr(st, "取消")) return lv_color_hex(0xC0503C);
    if (strstr(st, "延误") || (delay && delay[0] == '+'))
        return lv_color_hex(0xD98A2B);
    return lv_color_hex(0x6E9B7E);
}

lv_obj_t *ui_board_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, CW_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(scr, 22, 0);

    /* ---- 标题 + 机场徽章 ---- */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "航班动态");
    lv_obj_set_style_text_color(title, CW_INK, 0);
    lv_obj_set_style_text_font(title, &cn_font, 0);
    lv_obj_set_pos(title, 22, 26);

    s_ap_badge = lv_label_create(scr);
    lv_label_set_text(s_ap_badge, "---");
    lv_obj_set_style_text_color(s_ap_badge, CW_INK_SOFT, 0);
    lv_obj_set_style_text_font(s_ap_badge, &cn_font, 0);
    lv_obj_align(s_ap_badge, LV_ALIGN_TOP_RIGHT, -24, 28);

    s_sub = lv_label_create(scr);
    lv_label_set_text(s_sub, "等待数据…");
    lv_obj_set_style_text_color(s_sub, CW_SUB, 0);
    lv_obj_set_style_text_font(s_sub, &cn_font, 0);
    lv_obj_set_pos(s_sub, 22, 60);

    /* 分隔线 */
    lv_obj_t *line = lv_obj_create(scr);
    lv_obj_set_size(line, 436, 1);
    lv_obj_set_pos(line, 22, 88);
    lv_obj_set_style_bg_color(line, CW_LINE, 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(line, 0, 0);

    /* ---- 列表（flex 列，可滚动） ---- */
    s_list = lv_obj_create(scr);
    lv_obj_set_size(s_list, 436, 344);
    lv_obj_set_pos(s_list, 22, 98);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, 6, 0);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);

    /* ---- 底部提示 ---- */
    lv_obj_t *foot = lv_label_create(scr);
    lv_label_set_text(foot, "滑动切页 · 出发大屏每 10 分钟刷新");
    lv_obj_set_style_text_color(foot, lv_color_hex(0xA88976), 0);
    lv_obj_set_style_text_font(foot, &cn_font, 0);
    lv_obj_align(foot, LV_ALIGN_BOTTOM_LEFT, 0, -16);

    s_hint = NULL;   /* 按需创建 */
    return scr;
}

/* 重建全部条目 */
void ui_board_set(const board_list_t *b)
{
    if (!b) return;

    char buf[64];
    snprintf(buf, sizeof(buf), "%s · %s",
             b->airport[0] ? b->airport : "---",
             b->airport_name[0] ? b->airport_name : "机场");
    lv_label_set_text(s_ap_badge, buf);

    weather_data_t t;
    memset(&t, 0, sizeof(t));
    time_now(&t);

    lv_obj_clean(s_list);

    if (!b->valid || b->count == 0) {
        lv_obj_t *empty = lv_label_create(s_list);
        lv_label_set_text(empty, "今日该机场暂无出发航班\n或飞常准 Key 未配置/无效");
        lv_obj_set_style_text_color(empty, CW_SUB, 0);
        lv_obj_set_style_text_font(empty, &cn_font, 0);
        snprintf(buf, sizeof(buf), "%02d:%02d · 无数据", t.hour, t.min);
        lv_label_set_text(s_sub, buf);
        return;
    }

    for (int i = 0; i < b->count; i++) {
        const board_item_t *it = &b->items[i];

        /* 条目行：无背景容器，内部绝对布局 */
        lv_obj_t *row = lv_obj_create(s_list);
        lv_obj_set_size(row, lv_pct(100), 44);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        /* 航班号 */
        lv_obj_t *l_f = lv_label_create(row);
        lv_label_set_text(l_f, it->fnum);
        lv_obj_set_style_text_color(l_f, CW_INK, 0);
        lv_obj_set_style_text_font(l_f, &cn_font, 0);
        lv_obj_set_pos(l_f, 0, 4);

        /* → 目的地 + 航站楼 */
        char d[40];
        if (it->term[0]) snprintf(d, sizeof(d), "→%s %s", it->dest, it->term);
        else             snprintf(d, sizeof(d), "→%s", it->dest);
        lv_obj_t *l_d = lv_label_create(row);
        lv_label_set_text(l_d, d);
        lv_obj_set_style_text_color(l_d, CW_INK_SOFT, 0);
        lv_obj_set_style_text_font(l_d, &cn_font, 0);
        lv_obj_set_pos(l_d, 96, 4);

        /* 计划/预计时刻 */
        const char *tm = it->est[0] ? it->est : it->plan;
        lv_obj_t *l_t = lv_label_create(row);
        lv_label_set_text(l_t, tm);
        lv_obj_set_style_text_color(l_t,
            it->est[0] && strcmp(it->est, it->plan) != 0
                ? lv_color_hex(0xD98A2B) : CW_ACCENT, 0);
        lv_obj_set_style_text_font(l_t, &cn_font, 0);
        lv_obj_align(l_t, LV_ALIGN_RIGHT_MID, -108, -4);

        /* 状态（延误时附 +xx） */
        char st[BD_STAT_MAX + 12];
        if (it->delay[0] == '+') snprintf(st, sizeof(st), "%s 延%s分", it->status, it->delay + 1);
        else                     snprintf(st, sizeof(st), "%s", it->status);
        lv_obj_t *l_s = lv_label_create(row);
        lv_label_set_text(l_s, st);
        lv_obj_set_style_text_color(l_s, status_color(it->status, it->delay), 0);
        lv_obj_set_style_text_font(l_s, &cn_font, 0);
        lv_obj_align(l_s, LV_ALIGN_RIGHT_MID, 0, -4);

        /* 行间细线 */
        if (i < b->count - 1) {
            lv_obj_t *ln = lv_obj_create(s_list);
            lv_obj_set_size(ln, lv_pct(100), 1);
            lv_obj_set_style_bg_color(ln, CW_LINE, 0);
            lv_obj_set_style_bg_opa(ln, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(ln, 0, 0);
            lv_obj_set_style_pad_all(ln, 0, 0);
        }
    }

    snprintf(buf, sizeof(buf), "%02d:%02d 更新 · 未来出发 %d 条",
             t.hour, t.min, b->count);
    lv_label_set_text(s_sub, buf);
}
