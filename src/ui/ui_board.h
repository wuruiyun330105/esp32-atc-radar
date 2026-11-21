/**
 * ui_board.h —— 航班动态页（飞常准机场大屏）
 */
#ifndef UI_BOARD_H
#define UI_BOARD_H

#include "lvgl.h"
#include "data/data_models.h"
#ifdef __cplusplus
extern "C" {
#endif


lv_obj_t *ui_board_create(void);

/* 注入最新航班动态（空列表显示空态提示） */
void ui_board_set(const board_list_t *b);


#ifdef __cplusplus
}
#endif
#endif
