/*
 * SPDX-FileCopyrightText: 2025
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal CST816S / CST816T single-point capacitive touch driver.
 *
 * Register map used here:
 *   0x01 gesture  0x02 finger num  0x03..0x06 X/Y (12 bit, high nibble first)
 *   0xA7 chip id (0xB5 for CST816S)
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"

#include "cst816s.h"

#define CST816S_ADDR        (0x15)
#define CST816S_REG_GESTURE (0x01)
#define CST816S_REG_STATUS  (0x02)
#define CST816S_REG_CHIP_ID (0xA7)

static const char *TAG = "cst816s";

struct cst816s_dev_t {
    i2c_master_dev_handle_t i2c;
    int rst_gpio;
    int int_gpio;
};

static esp_err_t cst816s_write_reg(cst816s_handle_t dev, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(dev->i2c, buf, sizeof(buf), 100);
}

static esp_err_t cst816s_read_regs(cst816s_handle_t dev, uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(dev->i2c, &reg, 1, buf, len, 100);
}

esp_err_t cst816s_init(i2c_master_bus_handle_t bus, int rst_gpio, int int_gpio, cst816s_handle_t *out_dev)
{
    ESP_RETURN_ON_FALSE(bus && out_dev, ESP_ERR_INVALID_ARG, TAG, "invalid argument");

    struct cst816s_dev_t *dev = calloc(1, sizeof(struct cst816s_dev_t));
    ESP_RETURN_ON_FALSE(dev, ESP_ERR_NO_MEM, TAG, "no mem");

    esp_err_t ret = ESP_OK;
    dev->rst_gpio = rst_gpio;
    dev->int_gpio = int_gpio;

    if (rst_gpio >= 0) {
        gpio_config_t io_conf = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << rst_gpio,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, TAG, "reset gpio config failed");
        gpio_set_level(rst_gpio, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(rst_gpio, 1);
        vTaskDelay(pdMS_TO_TICKS(60));
    }

    if (int_gpio >= 0) {
        gpio_config_t io_conf = {
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pin_bit_mask = 1ULL << int_gpio,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, TAG, "int gpio config failed");
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CST816S_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_GOTO_ON_ERROR(i2c_master_bus_add_device(bus, &dev_cfg, &dev->i2c), err, TAG, "add i2c device failed");

    uint8_t id = 0;
    if (cst816s_read_regs(dev, CST816S_REG_CHIP_ID, &id, 1) == ESP_OK) {
        ESP_LOGI(TAG, "chip id 0x%02X%s", id, id == 0xB5 ? " (CST816S)" : "");
    } else {
        ESP_LOGW(TAG, "chip id read failed");
    }

    /* Disable auto-sleep so the panel stays responsive (reg 0xFA, 1 = never sleep) */
    cst816s_write_reg(dev, 0xFA, 0x01);
    /* Enable IRQ on touch down (reg 0xFA bit? use motion mask) - keep polling default */
    cst816s_write_reg(dev, 0xEC, 0x01);

    *out_dev = dev;
    return ret;

err:
    if (dev->i2c) {
        i2c_master_bus_rm_device(dev->i2c);
    }
    free(dev);
    return ret;
}

uint8_t cst816s_get_chip_id(cst816s_handle_t dev)
{
    uint8_t id = 0;
    if (dev) {
        cst816s_read_regs(dev, CST816S_REG_CHIP_ID, &id, 1);
    }
    return id;
}

esp_err_t cst816s_read(cst816s_handle_t dev, uint16_t *x, uint16_t *y, bool *pressed)
{
    ESP_RETURN_ON_FALSE(dev && x && y && pressed, ESP_ERR_INVALID_ARG, TAG, "invalid argument");

    uint8_t buf[5] = {0};
    ESP_RETURN_ON_ERROR(cst816s_read_regs(dev, CST816S_REG_STATUS, buf, sizeof(buf)), TAG, "read point failed");

    const uint8_t fingers = buf[0] & 0x0F;
    *pressed = fingers > 0;
    *x = (uint16_t)(((buf[1] & 0x0F) << 8) | buf[2]);
    *y = (uint16_t)(((buf[3] & 0x0F) << 8) | buf[4]);
    return ESP_OK;
}
