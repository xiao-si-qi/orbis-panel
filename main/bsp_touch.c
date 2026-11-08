#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "driver/i2c_master.h"

#include "bsp_board.h"
#include "bsp_touch.h"
#include "cst816s.h"

static const char *TAG = "bsp_touch";

static cst816s_handle_t s_touch;
static i2c_master_bus_handle_t s_bus;
static int s_rotation;      /* 0/90/180/270：触摸坐标要和屏幕一起转 */

void bsp_touch_set_rotation(int deg)
{
    s_rotation = deg;
}

/** 扫描 I2C 总线（临时诊断：看看除了触摸还有没有别的器件，比如加速度计）。 */
void bsp_touch_scan_bus(void)
{
    if (s_bus == NULL) {
        return;
    }
    char found[160] = {0};
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(s_bus, addr, 50) == ESP_OK) {
            const size_t n = strlen(found);
            snprintf(found + n, sizeof(found) - n, " 0x%02X", addr);
        }
    }
    ESP_LOGI(TAG, "I2C 扫描:%s", found[0] ? found : " (无)");
}

esp_err_t bsp_touch_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = BSP_TOUCH_I2C_PORT,
        .sda_io_num = BSP_TOUCH_SDA_GPIO,
        .scl_io_num = BSP_TOUCH_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {
            .enable_internal_pullup = true,
        },
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus), TAG, "i2c bus init failed");

    esp_err_t err = cst816s_init(s_bus, BSP_TOUCH_RST_GPIO, BSP_TOUCH_INT_GPIO, &s_touch);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CST816S init failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "CST816S ready (SDA=%d SCL=%d)", BSP_TOUCH_SDA_GPIO, BSP_TOUCH_SCL_GPIO);
    return ESP_OK;
}

bool bsp_touch_read(uint16_t *x, uint16_t *y)
{
    if (s_touch == NULL) {
        return false;
    }
    bool pressed = false;
    if (cst816s_read(s_touch, x, y, &pressed) != ESP_OK) {
        return false;
    }
    if (*x >= BSP_LCD_H_RES) {
        *x = BSP_LCD_H_RES - 1;
    }
    if (*y >= BSP_LCD_V_RES) {
        *y = BSP_LCD_V_RES - 1;
    }

    /* 触摸芯片是固定方向的；屏幕硬件旋转后，这里换算成逻辑坐标 */
    const uint16_t px = *x;
    const uint16_t py = *y;
    switch (s_rotation) {
    case 90:
        *x = (uint16_t)(BSP_LCD_H_RES - 1 - py);
        *y = px;
        break;
    case 180:
        *x = (uint16_t)(BSP_LCD_H_RES - 1 - px);
        *y = (uint16_t)(BSP_LCD_V_RES - 1 - py);
        break;
    case 270:
        *x = (uint16_t)(BSP_LCD_H_RES - 1 - py);   /* 270 度：X 镜像、Y 直接取 */
        *y = px;
        break;
    default:
        break;
    }
    return pressed;
}
