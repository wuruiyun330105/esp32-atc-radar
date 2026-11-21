/**
 * ui_clock_weather.h —— 暖色时钟天气页
 */
#ifndef UI_CLOCK_WEATHER_H
#define UI_CLOCK_WEATHER_H

#include "lvgl.h"
#include "data_models.h"
#ifdef __cplusplus
extern "C" {
#endif


lv_obj_t *ui_clock_weather_create(void);
void ui_clock_weather_update(const weather_data_t *d);


#ifdef __cplusplus
}
#endif
#endif
