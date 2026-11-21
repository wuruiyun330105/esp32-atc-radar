/**
 * ui_radar.h —— 塔台 PPI 磷光绿雷达页
 */
#ifndef UI_RADAR_H
#define UI_RADAR_H

#include "lvgl.h"
#include "data_models.h"
#ifdef __cplusplus
extern "C" {
#endif


/* 创建雷达页（返回 screen 对象） */
lv_obj_t *ui_radar_create(void);

/* 每帧刷新：扫描、余晖、目标、数据块 */
void ui_radar_tick(void);

/* 外部喂入航班数据（不调用则用 data_flight 的列表） */
void ui_radar_set_flights(const flight_list_t *fl);

/* 底部航班条：显示 2 条机场动态（原航班动态页并入雷达页） */
void ui_radar_set_board(const board_list_t *b);

/* 无线电状态（真塔台同款）：tx=1 显示 TX 并在底部滚动播报字幕 */
void ui_radar_set_radio(int tx, const char *subtitle);

/* 设置底部状态栏文字（WiFi/UTC 等） */
void ui_radar_set_status(const char *left_txt, const char *utc_txt);

/* 量程显示（顶部右侧 RNG xxNM） */
void ui_radar_set_range(int range_nm);

/* 是否有详情卡打开（切页/手势前可判断） */
int  ui_radar_card_open(void);

/* 关闭详情卡 */
void ui_radar_card_close(void);

/* 调试/自动化：直接选中第 i 个目标并弹卡 */
void ui_radar_select_index(int i);


#ifdef __cplusplus
}
#endif
#endif
