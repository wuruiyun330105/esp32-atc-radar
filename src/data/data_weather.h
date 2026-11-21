/**
 * data_weather.h —— 和风天气：按经纬度查询实时天气
 */
#ifndef DATA_WEATHER_H
#define DATA_WEATHER_H

#include "data_models.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 拉取实时天气（阻塞，建议放到独立任务/线程或低频调用）
 * @param cfg  配置（含 key/host/经纬度）
 * @param out  输出
 * @return true 成功
 */
bool weather_fetch_now(const app_config_t *cfg, weather_data_t *out);

/**
 * 解析和风 /v7/weather/now 的 JSON 响应（独立出来便于 PC 单测）
 * 同时把 icon/text 映射成中文 condition、icon 代码
 */
bool weather_parse_now(const char *json, weather_data_t *out);

#ifdef __cplusplus
}
#endif

#endif
