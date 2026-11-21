/**
 * ip_locate.h —— IP 级自动定位（城市中心，误差 1~10km）
 *
 * 设备没 GPS 芯片时的"开箱即定位"方案：
 *   出口 IP → ip-api.com（免 Key、免费、国内可达）→ 城市经纬度。
 * 精度对 120NM 雷达完全够用；需要精确位置走配网页手机 GPS 或外接卫星模块。
 */
#ifndef IP_LOCATE_H
#define IP_LOCATE_H
#ifdef __cplusplus
extern "C" {
#endif

/* 阻塞式（约 2~4 秒，需 WiFi 已连）。成功返回 1 并填 lat/lon/city */
int ip_locate(double *lat, double *lon, char *city, int cap);

/* 最近一次定位到的城市名（未定位过 = "上海"） */
const char *ip_locate_city(void);

#ifdef __cplusplus
}
#endif
#endif
