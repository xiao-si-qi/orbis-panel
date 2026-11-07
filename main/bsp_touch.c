#include "esp_check.h"
#include "esp_log.h"
#include "driver/i2c_master.h"

#include "bsp_board.h"
#include "bsp_touch.h"
#include "cst816s.h"

static const char *TAG = "bsp_touch";

static cst816s_handle_t s_touch;
static i2c_master_bus_handle_t s_bus;

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
    return pressed;
}
