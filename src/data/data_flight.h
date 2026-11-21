/**
 * data_flight.h —— 航班数据（真实 ADS-B / 演示兜底）
 *
 * 职责：
 *   · 维护目标列表（屏幕坐标 + 飞行参数）
 *   · 经纬度 → 极坐标 → 屏幕坐标换算
 *   · 两次网络刷新之间按航向/地速做死推算（dead reckoning），动画不卡顿
 *   · 扫描激发后的余晖亮度 glow（渲染层用）
 */
#ifndef DATA_FLIGHT_H
#define DATA_FLIGHT_H

#include "data_models.h"
#ifdef __cplusplus
extern "C" {
#endif


/* 初始化。demo=0 时表示"有网就用真实数据"，失败自动回落演示 */
void flight_init(int demo);

/* 周期调用（dt 秒）：推进目标位置 / 刷新余晖 / 到点触发网络刷新 */
void flight_update(float dt_sec);

/* 取当前航班列表（只读） */
const flight_list_t *flight_get(void);

/* 设置雷达中心与量程 */
void flight_set_view(float lat, float lon, int range_nm);

/* 绑定配置（数据源、中心点、量程），供网络层使用 */
void flight_bind_config(const app_config_t *cfg);

/* 网络层解析完成后调这里，接管为真实数据（保留同呼号目标的余晖/尾迹） */
void flight_apply_live(const flight_list_t *src);

/* 是否已经拿到过真实数据 */
int  flight_is_live(void);

/* 最近一次网络刷新结果：0=未尝试 1=成功 -1=失败 */
int  flight_last_result(void);

/* 供渲染层：某方位被扫描过时激发目标余晖 */
void flight_excite(float sweep_deg);

/* 命中检测：返回屏幕最近目标索引，超出 tol_px 返回 -1 */
int  flight_pick(int px, int py, int tol_px);

/* 取第 i 个目标（NULL 表示无） */
const flight_t *flight_at(int i);


#ifdef __cplusplus
}
#endif
#endif
