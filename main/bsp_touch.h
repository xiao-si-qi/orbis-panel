#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化 I2C 总线 + CST816S。 */
esp_err_t bsp_touch_init(void);

/** 扫描 I2C 总线并打印发现的地址（诊断用）。 */
void bsp_touch_scan_bus(void);

/** 读一次触摸点，返回是否按下。 */
bool bsp_touch_read(uint16_t *x, uint16_t *y);

#ifdef __cplusplus
}
#endif
