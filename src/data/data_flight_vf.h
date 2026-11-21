/**
 * data_flight_vf.h —— 雷达实时机群（飞常准在飞航班轨迹）
 */
#ifndef DATA_FLIGHT_VF_H
#define DATA_FLIGHT_VF_H

#include "data_models.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 拉一轮"在飞航班"并换算成雷达目标列表。
 * 返回目标数（>=0）；<0 = 未配置 Key / 会话失败。
 * 计费：1 次（大屏）+ 每架飞机 1 次（轨迹），每轮最多 12 架。 */
int flight_vf_fetch(const app_config_t *cfg, flight_list_t *out);

#ifdef __cplusplus
}
#endif
#endif
