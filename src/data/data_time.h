/**
 * data_time.h —— 时间获取（PC: 系统时间 / 设备: NTP 校时后取本地时间）
 */
#ifndef DATA_TIME_H
#define DATA_TIME_H

#include "data_models.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 时间是否已同步（设备端 NTP 完成前为 false） */
bool time_is_synced(void);
/* 设备端：启动 NTP 校时（PC 端为空实现） */
void time_begin_ntp(void);
/* 获取当前本地时间（UTC+8），填入 weather_data_t 的日期/时间字段 */
void time_now(weather_data_t *d);

#ifdef __cplusplus
}
#endif

#endif
