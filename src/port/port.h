/**
 * port.h —— 硬件移植层接口（设备端实现；PC 端用 SDL 不编译本层）
 *
 * 到货对接顺序：
 *   board_init()     —— 初始化 I2C / QSPI 屏 / 触摸 / 背光（依赖微雪 pin_config.h）
 *   lvgl_port_init() —— 把屏和触摸注册成 LVGL 的 display / indev
 * 其余 UI 与业务代码全部与硬件无关，无需改动。
 */
#ifndef PORT_H
#define PORT_H

#ifdef __cplusplus
extern "C" {
#endif

void board_init(void);       /* 硬件上电初始化 */
void lvgl_port_init(void);   /* LVGL 显示/触摸对接 */

#ifdef __cplusplus
}
#endif

#endif
