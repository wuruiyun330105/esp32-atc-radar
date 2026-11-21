/**
 * data_board.h —— 机场航班大屏（飞常准 MCP，按调用计费）
 *
 * 数据流：配置坐标 → getAirportByLatLng 自动选最近机场（缓存，坐标变了才重探）
 *        → getAirportFlightBoard 取出发大屏 → 过滤"未来出发"排序 → board_list_t
 * 刷新节奏由调用方控制（建议 ≥10 分钟/次：每次 1~2 次计费调用）
 */
#ifndef DATA_BOARD_H
#define DATA_BOARD_H

#include "data_models.h"
#ifdef __cplusplus
extern "C" {
#endif


/* 拉取航班动态。返回条数（0=机场今日无航班，<0=失败/未配 Key） */
int board_fetch(const app_config_t *cfg, board_list_t *out);

/* ---------- 雷达实时机群：把在飞航班搬上雷达 ---------- */
/* 取"进港在飞"航班清单（状态=起飞，且 45 分钟内落地）。
 * 返回条数（>=0），<0 = 失败。每次 1 次计费调用。 */
int board_fetch_inflight(const app_config_t *cfg, board_inflight_t *out, int max);

/* 取单航班最新轨迹点（variflight 自家 ADS-B，实测有国内覆盖）。
 * 输出：lat/lng 度，alt_m 米，spd_ms 米/秒，hdg 度，vs_ms 米/秒。
 * 返回 1=成功，0=无轨迹，-1=接口/网络失败。每次 1 次计费调用。 */
int board_fetch_track(const app_config_t *cfg, const char *fnum,
                      const char *dep, const char *arr, const char *date,
                      float *lat, float *lng, float *alt_m,
                      float *spd_ms, float *hdg, float *vs_ms);


#ifdef __cplusplus
}
#endif
#endif
