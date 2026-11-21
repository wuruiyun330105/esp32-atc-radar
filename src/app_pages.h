/**
 * app_pages.h —— 多页框架：双页（雷达 / 时钟天气）+ 手势滑动切换 + 页码指示
 * 航班动态已并入雷达页底部航班条，不再单独占一页
 */
#ifndef APP_PAGES_H
#define APP_PAGES_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

enum { PAGE_RADAR = 0, PAGE_CLOCK, PAGE_COUNT };

/* 初始化：创建各页与指示器，载入第一页 */
void app_pages_init(void);

/* 切到指定页（带动画可选） */
void app_pages_goto(int page, int anim);

/* 当前页 */
int app_pages_current(void);

/* 外部注入最新天气数据（时间字段由内部每秒刷新） */
#include "data/data_models.h"

void app_pages_set_weather(const weather_data_t *w);

/* 外部注入最新航班动态（飞常准机场大屏）→ 显示在雷达页底部航班条 */
void app_pages_set_board(const board_list_t *b);

/* 无线电状态 + 播报字幕（雷达页右上 TX/STBY、底部滚动字幕） */
void app_pages_set_radio(int tx, const char *subtitle);

/* 全局 tick：驱动雷达扫描、时间刷新等，由主循环周期调用 */
void app_pages_tick(void);


#ifdef __cplusplus
}
#endif
#endif
