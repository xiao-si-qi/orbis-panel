#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief ST77916 panel configuration (QSPI 4-line data bus)
 */
typedef struct {
    int reset_gpio_num;        /*!< GPIO for panel RESET, -1 if tied to board reset */
    bool reset_active_high;    /*!< Reset active level, 0 for most panels */
    bool use_qspi;             /*!< true: 32-bit opcode+cmd on 1 line, data on 4 lines */
    const void *init_cmds;     /*!< Optional custom init sequence, NULL = built-in vendor table */
    uint16_t init_cmds_size;   /*!< Number of entries in init_cmds */
} esp_lcd_st77916_config_t;

/**
 * @brief Init command entry, same layout as esp_lcd_panel_vendor_init_cmd_t
 */
typedef struct {
    uint8_t cmd;
    const void *data;
    size_t data_bytes;
    uint8_t delay_ms;
} st77916_init_cmd_t;

/**
 * @brief Create an ST77916 panel instance on top of an esp_lcd panel IO handle
 *
 * @param io       Panel IO handle (must be created with lcd_cmd_bits = 32 and
 *                 flags.quad_mode = 1 for QSPI)
 * @param config   Panel configuration
 * @param ret_panel Returned panel handle
 */
esp_err_t esp_lcd_new_panel_st77916(const esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_st77916_config_t *config,
                                    esp_lcd_panel_handle_t *ret_panel);

#ifdef __cplusplus
}
#endif
