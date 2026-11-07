#pragma once

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 LVGL 9：显示驱动、触摸输入、tick
 *
 * 必须在 bsp_display_init() 和 bsp_touch_init() 之后调用。
 */
esp_err_t bsp_lvgl_init(esp_lcd_panel_handle_t panel);

/** 启动 LVGL 任务（内部 5~10ms 周期调用 lv_timer_handler）。 */
void bsp_lvgl_start(void);

#ifdef __cplusplus
}
#endif
