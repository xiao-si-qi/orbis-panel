#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 每次 DMA 传输完成回调（LVGL 用它调用 lv_display_flush_ready）。 */
typedef void (*bsp_display_flush_done_cb_t)(void *user_ctx);

/**
 * @brief 初始化 QSPI + ST77916 面板 + 背光 PWM
 *
 * @param[out] ret_panel  返回 esp_lcd 面板句柄
 * @param[in]  done_cb    颜色数据传输完成回调，可为 NULL
 * @param[in]  done_ctx   回调上下文
 */
esp_err_t bsp_display_init(esp_lcd_panel_handle_t *ret_panel,
                           bsp_display_flush_done_cb_t done_cb, void *done_ctx);

/** 背光亮度 0..100。 */
void bsp_display_set_backlight(uint8_t percent);

/** 当前背光亮度（0..100），状态上报用。 */
uint8_t bsp_display_get_brightness(void);

/** 设置/更换 DMA 传输完成回调（LVGL 移植层用，必须在 init 之后、开始刷屏之前调用）。 */
void bsp_display_set_flush_done_hook(bsp_display_flush_done_cb_t cb, void *user_ctx);

/** 开关屏幕（只关背光，面板仍保持内容）。 */
void bsp_display_screen_on(bool on);

esp_lcd_panel_handle_t bsp_display_panel(void);

/** 屏幕方向（0/90/180/270），走面板 MADCTL 硬件旋转。 */
void bsp_display_set_rotation(int deg);

#ifdef __cplusplus
}
#endif
