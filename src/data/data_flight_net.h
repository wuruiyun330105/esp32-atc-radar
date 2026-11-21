/**
 * data_flight_net.h —— 真实航班数据获取（ADS-B 公开源）
 *
 * 三个公益源 + OpenSky 兜底（2026-11 实测现状，细节见 README 第三节）：
 *   adsb.fi          opendata.adsb.fi/api/v2/lat/{lat}/lon/{lon}/dist/{r}   免 key，实测可用
 *   adsb.lol         api.adsb.lol/v2/point/{lat}/{lon}/{r}                  免 key，部分网络被 CF 拦
 *   airplanes.live   api.airplanes.live/v2/point/{lat}/{lon}/{r}            2026 起需邮件申请，未通过则 403
 *   OpenSky Network  opensky-network.org/api/states/all?lamin=...           匿名 400 credits/天
 *
 * 源健康记忆：403 的源自动暂停 1 小时、429/限流 5 分钟，期间不再浪费请求，
 * 恢复后自动回归轮换。OpenSky 收到 429 进入 15 分钟冷却，轮询间隔自动放宽。
 */
#ifndef DATA_FLIGHT_NET_H
#define DATA_FLIGHT_NET_H

#include "data_models.h"
#ifdef __cplusplus
extern "C" {
#endif


/**
 * 拉取并解析。
 * @param clat/clon  雷达中心（观测点）经纬度
 * @param range_nm   搜索半径（海里）
 * @param src        FL_SRC_ADSB / FL_SRC_OPENSKY
 * @param out        输出列表（只填 count/items 的飞行参数，屏幕坐标由上层换算）
 * @return 解析到的目标数；0 表示失败
 */
int flight_net_fetch(float clat, float clon, int range_nm, int src, flight_list_t *out);

/* 带本地接收机地址的版本（FL_SRC_LOCAL 时使用 adsb_host，如 192.168.1.50:8080） */
int flight_net_fetch_host(float clat, float clon, int range_nm, int src,
                          const char *adsb_host, flight_list_t *out);

/* 单独暴露解析器，便于 PC 端用本地样本联调 */
int flight_parse_adsb_json(const char *json, float clat, float clon, int range_nm,
                           flight_list_t *out);
int flight_parse_opensky_json(const char *json, float clat, float clon, int range_nm,
                              flight_list_t *out);

/* OpenSky 429 冷却剩余毫秒数（0 = 正常）。net_task 把它叠加进轮询间隔，
 * 配额用尽时自动从 5 分钟放宽到 20 分钟，恢复后自动收回。 */
uint32_t flight_net_opensky_extra_wait(void);


#ifdef __cplusplus
}
#endif
#endif
