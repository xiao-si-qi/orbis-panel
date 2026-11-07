/*
 * SPDX-FileCopyrightText: 2025
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal ST77916 (360x360, 1.85" round IPS) panel driver for ESP-IDF.
 *
 * The panel is wired over a 4-line QSPI bus, which this driver speaks by
 * prefixing every command with an opcode byte:
 *
 *   bit[31:24] opcode   0x02 = write command/parameter, 0x32 = write color
 *   bit[23:8]  command  the real LCD command byte
 *   bit[7:0]   unused
 *
 * The command phase is sent on 1 data line, the parameter/color phase on all
 * 4 lines. That is exactly what esp_lcd_panel_io_spi does when it is created
 * with lcd_cmd_bits = 32 and flags.quad_mode = 1.
 *
 * The initialization sequence is the vendor (Espressif ESP32_Display_Panel
 * v0.1.4) table shipped for this panel, generated into st77916_init_cmds.h.
 */

#include <stdlib.h>
#include <string.h>
#include <sys/cdefs.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"

#include "esp_lcd_st77916.h"
#include "st77916_init_cmds.h"

#define LCD_OPCODE_WRITE_CMD    (0x02u)
#define LCD_OPCODE_READ_CMD     (0x0Bu)
#define LCD_OPCODE_WRITE_COLOR  (0x32u)

#define ST77916_CMD_SET         (0xF0)
#define ST77916_PARAM_SET       (0x00)

static const char *TAG = "st77916";

typedef struct {
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
    int reset_gpio_num;
    int x_gap;
    int y_gap;
    uint8_t madctl_val;
    struct {
        unsigned int use_qspi_interface : 1;
        unsigned int reset_level : 1;
    } flags;
} st77916_panel_t;

static esp_err_t panel_st77916_del(esp_lcd_panel_t *panel);
static esp_err_t panel_st77916_reset(esp_lcd_panel_t *panel);
static esp_err_t panel_st77916_init(esp_lcd_panel_t *panel);
static esp_err_t panel_st77916_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start,
                                           int x_end, int y_end, const void *color_data);
static esp_err_t panel_st77916_invert_color(esp_lcd_panel_t *panel, bool invert_color_data);
static esp_err_t panel_st77916_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y);
static esp_err_t panel_st77916_swap_xy(esp_lcd_panel_t *panel, bool swap_axes);
static esp_err_t panel_st77916_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap);
static esp_err_t panel_st77916_disp_on_off(esp_lcd_panel_t *panel, bool on_off);

static inline int st77916_encode_cmd(const st77916_panel_t *p, int lcd_cmd, uint8_t opcode)
{
    if (!p->flags.use_qspi_interface) {
        return lcd_cmd;
    }
    return (int)((lcd_cmd & 0xFF) << 8) | ((int)opcode << 24);
}

static esp_err_t st77916_tx_param(st77916_panel_t *p, int lcd_cmd, const void *param, size_t param_size)
{
    return esp_lcd_panel_io_tx_param(p->io, st77916_encode_cmd(p, lcd_cmd, LCD_OPCODE_WRITE_CMD), param, param_size);
}

static esp_err_t st77916_tx_color(st77916_panel_t *p, int lcd_cmd, const void *param, size_t param_size)
{
    return esp_lcd_panel_io_tx_color(p->io, st77916_encode_cmd(p, lcd_cmd, LCD_OPCODE_WRITE_COLOR), param, param_size);
}

esp_err_t esp_lcd_new_panel_st77916(const esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_st77916_config_t *config,
                                    esp_lcd_panel_handle_t *ret_panel)
{
    ESP_RETURN_ON_FALSE(io && config && ret_panel, ESP_ERR_INVALID_ARG, TAG, "invalid argument");

    st77916_panel_t *st77916 = calloc(1, sizeof(st77916_panel_t));
    ESP_RETURN_ON_FALSE(st77916, ESP_ERR_NO_MEM, TAG, "no mem for st77916 panel");

    esp_err_t ret = ESP_OK;

    if (config->reset_gpio_num >= 0) {
        gpio_config_t io_conf = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << config->reset_gpio_num,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, TAG, "configure GPIO for RST line failed");
    }

    st77916->io = io;
    st77916->reset_gpio_num = config->reset_gpio_num;
    st77916->flags.reset_level = config->reset_active_high;
    st77916->flags.use_qspi_interface = config->use_qspi;
    st77916->madctl_val = 0; /* RGB order, no mirror, no swap */

    st77916->base.del = panel_st77916_del;
    st77916->base.reset = panel_st77916_reset;
    st77916->base.init = panel_st77916_init;
    st77916->base.draw_bitmap = panel_st77916_draw_bitmap;
    st77916->base.invert_color = panel_st77916_invert_color;
    st77916->base.set_gap = panel_st77916_set_gap;
    st77916->base.mirror = panel_st77916_mirror;
    st77916->base.swap_xy = panel_st77916_swap_xy;
    st77916->base.disp_on_off = panel_st77916_disp_on_off;
    *ret_panel = &(st77916->base);

    ESP_LOGD(TAG, "new st77916 panel @%p", st77916);
    return ESP_OK;

err:
    if (st77916) {
        if (config->reset_gpio_num >= 0) {
            gpio_reset_pin(config->reset_gpio_num);
        }
        free(st77916);
    }
    return ret;
}

static esp_err_t panel_st77916_del(esp_lcd_panel_t *panel)
{
    st77916_panel_t *st77916 = __containerof(panel, st77916_panel_t, base);

    if (st77916->reset_gpio_num >= 0) {
        gpio_reset_pin(st77916->reset_gpio_num);
    }
    ESP_LOGD(TAG, "del st77916 panel @%p", st77916);
    free(st77916);
    return ESP_OK;
}

static esp_err_t panel_st77916_reset(esp_lcd_panel_t *panel)
{
    st77916_panel_t *st77916 = __containerof(panel, st77916_panel_t, base);

    if (st77916->reset_gpio_num >= 0) {
        gpio_set_level(st77916->reset_gpio_num, st77916->flags.reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(st77916->reset_gpio_num, !st77916->flags.reset_level);
        vTaskDelay(pdMS_TO_TICKS(120));
    } else {
        ESP_RETURN_ON_ERROR(st77916_tx_param(st77916, LCD_CMD_SWRESET, NULL, 0), TAG, "software reset failed");
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    return ESP_OK;
}

static esp_err_t panel_st77916_init(esp_lcd_panel_t *panel)
{
    st77916_panel_t *st77916 = __containerof(panel, st77916_panel_t, base);
    bool is_cmd_set = true;

    /* MADCTL + COLMOD (RGB565) before the vendor unlock sequence */
    ESP_RETURN_ON_ERROR(st77916_tx_param(st77916, LCD_CMD_MADCTL, (uint8_t[]) {
        st77916->madctl_val,
    }, 1), TAG, "send MADCTL failed");
    ESP_RETURN_ON_ERROR(st77916_tx_param(st77916, LCD_CMD_COLMOD, (uint8_t[]) {
        0x55, /* RGB565 */
    }, 1), TAG, "send COLMOD failed");

    for (size_t i = 0; i < ST77916_VENDOR_INIT_CMDS_SIZE; i++) {
        const st77916_init_cmd_t *c = &st77916_vendor_init_cmds[i];

        ESP_RETURN_ON_ERROR(st77916_tx_param(st77916, c->cmd, c->data, c->data_bytes),
                            TAG, "send init cmd 0x%02X failed", c->cmd);
        if (c->delay_ms) {
            vTaskDelay(pdMS_TO_TICKS(c->delay_ms));
        }

        /* Track the 0xF0 "command set" state, some commands are only valid inside a set */
        if (c->cmd == ST77916_CMD_SET) {
            is_cmd_set = (((const uint8_t *)c->data)[0] == ST77916_PARAM_SET);
        }
    }
    ESP_LOGD(TAG, "init sequence done (last set state: %d)", is_cmd_set);

    return ESP_OK;
}

static esp_err_t panel_st77916_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start,
                                           int x_end, int y_end, const void *color_data)
{
    st77916_panel_t *st77916 = __containerof(panel, st77916_panel_t, base);
    assert((x_start < x_end) && (y_start < y_end) && "start position must be smaller than end position");

    x_start += st77916->x_gap;
    x_end += st77916->x_gap;
    y_start += st77916->y_gap;
    y_end += st77916->y_gap;

    ESP_RETURN_ON_ERROR(st77916_tx_param(st77916, LCD_CMD_CASET, (uint8_t[]) {
        (x_start >> 8) & 0xFF,
        x_start & 0xFF,
        ((x_end - 1) >> 8) & 0xFF,
        (x_end - 1) & 0xFF,
    }, 4), TAG, "send CASET failed");

    ESP_RETURN_ON_ERROR(st77916_tx_param(st77916, LCD_CMD_RASET, (uint8_t[]) {
        (y_start >> 8) & 0xFF,
        y_start & 0xFF,
        ((y_end - 1) >> 8) & 0xFF,
        (y_end - 1) & 0xFF,
    }, 4), TAG, "send RASET failed");

    size_t len = (size_t)(x_end - x_start) * (size_t)(y_end - y_start) * 2;
    return st77916_tx_color(st77916, LCD_CMD_RAMWR, color_data, len);
}

static esp_err_t panel_st77916_invert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
    st77916_panel_t *st77916 = __containerof(panel, st77916_panel_t, base);
    return st77916_tx_param(st77916, invert_color_data ? LCD_CMD_INVON : LCD_CMD_INVOFF, NULL, 0);
}

static esp_err_t panel_st77916_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    st77916_panel_t *st77916 = __containerof(panel, st77916_panel_t, base);

    if (mirror_x) {
        st77916->madctl_val |= LCD_CMD_MX_BIT;
    } else {
        st77916->madctl_val &= ~LCD_CMD_MX_BIT;
    }
    if (mirror_y) {
        st77916->madctl_val |= LCD_CMD_MY_BIT;
    } else {
        st77916->madctl_val &= ~LCD_CMD_MY_BIT;
    }
    return st77916_tx_param(st77916, LCD_CMD_MADCTL, (uint8_t[]) {
        st77916->madctl_val
    }, 1);
}

static esp_err_t panel_st77916_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    st77916_panel_t *st77916 = __containerof(panel, st77916_panel_t, base);

    if (swap_axes) {
        st77916->madctl_val |= LCD_CMD_MV_BIT;
    } else {
        st77916->madctl_val &= ~LCD_CMD_MV_BIT;
    }
    return st77916_tx_param(st77916, LCD_CMD_MADCTL, (uint8_t[]) {
        st77916->madctl_val
    }, 1);
}

static esp_err_t panel_st77916_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    st77916_panel_t *st77916 = __containerof(panel, st77916_panel_t, base);
    st77916->x_gap = x_gap;
    st77916->y_gap = y_gap;
    return ESP_OK;
}

static esp_err_t panel_st77916_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    st77916_panel_t *st77916 = __containerof(panel, st77916_panel_t, base);
    return st77916_tx_param(st77916, on_off ? LCD_CMD_DISPON : LCD_CMD_DISPOFF, NULL, 0);
}
