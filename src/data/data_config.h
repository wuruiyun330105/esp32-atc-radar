/**
 * data_config.h —— 配置读写（WiFi/Key/GPS 等），PC 与设备共用接口
 *   PC  : 存 JSON 文件 ./app_config.json
 *   设备: 存 NVS (Preferences)
 */
#ifndef DATA_CONFIG_H
#define DATA_CONFIG_H

#include "data_models.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 载入配置到 out；不存在则填入默认值并返回 false */
bool cfg_load(app_config_t *out);
/* 保存配置 */
bool cfg_save(const app_config_t *in);
bool cfg_pos_saved(void);
/* 用默认值填充 */
void cfg_defaults(app_config_t *out);

#ifdef __cplusplus
}
#endif

#endif
