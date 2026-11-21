/**
 * net/web_config.h —— 配网门户（设备端）：开热点 + 手机浏览器配置
 */
#ifndef WEB_CONFIG_H
#define WEB_CONFIG_H

#include "data/data_models.h"
#ifdef __cplusplus
extern "C" {
#endif


/* 启动 AP 配网门户（HGH-RADAR, 192.168.4.1）。保存后自动重启。 */
void web_config_start(void);
/* 在 loop() 中周期调用以处理网页请求 */
void web_config_loop(void);
/* 是否处于配网模式 */
bool web_config_active(void);
/* 从网页保存后置位，loop 中检测到就重启 */
bool web_config_should_reboot(void);

/* 已连接 WiFi 后取本机 IP 供显示（设备端） */
void web_config_status(char *ssid_out, char *ip_out);


#ifdef __cplusplus
}
#endif
#endif
