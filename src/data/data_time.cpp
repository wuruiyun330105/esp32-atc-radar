/**
 * data_time.c —— 时间获取实现
 */
#include "data_time.h"
#include <time.h>

#ifdef ESP_PLATFORM
/* ======================= 设备端：NTP ======================= */
#include <Arduino.h>
#include <sys/time.h>

static bool s_synced = false;

void time_begin_ntp(void);   /* 在 app 初始化里调用一次 */

void time_begin_ntp(void)
{
    configTzTime("CST-8", "ntp.aliyun.com", "ntp1.aliyun.com", "pool.ntp.org");
    s_synced = true;
}

bool time_is_synced(void) { return s_synced; }

void time_now(weather_data_t *d)
{
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    d->year = tmv.tm_year + 1900;
    d->mon  = tmv.tm_mon + 1;
    d->day  = tmv.tm_mday;
    d->weekday = tmv.tm_wday;   /* 0=周日 */
    d->hour = tmv.tm_hour;
    d->min  = tmv.tm_min;
    d->sec  = tmv.tm_sec;
}

#else
/* ======================= PC：系统时间 ======================= */
/* Windows 无 localtime_r（POSIX），用 localtime_s（注意实参顺序相反） */
#if defined(_WIN32)
#define LOCALTIME_R(tp, tmp) localtime_s((tmp), (tp))
#else
#define LOCALTIME_R(tp, tmp) localtime_r((tp), (tmp))
#endif
bool time_is_synced(void) { return true; }
void time_begin_ntp(void) { /* PC 使用系统时间，无需 NTP */ }

void time_now(weather_data_t *d)
{
    time_t t = time(NULL);
    struct tm tmv;
    LOCALTIME_R(&t, &tmv);
    d->year = tmv.tm_year + 1900;
    d->mon  = tmv.tm_mon + 1;
    d->day  = tmv.tm_mday;
    d->weekday = tmv.tm_wday;   /* 0=周日 */
    d->hour = tmv.tm_hour;
    d->min  = tmv.tm_min;
    d->sec  = tmv.tm_sec;
}
#endif
