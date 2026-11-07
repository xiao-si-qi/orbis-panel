#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cst816s_dev_t *cst816s_handle_t;

/**
 * @brief Add a CST816S touch controller to an existing I2C master bus
 *
 * @param bus       I2C master bus handle
 * @param rst_gpio  Reset pin (set to -1 if not connected)
 * @param int_gpio  Interrupt pin (set to -1 if not connected, polling still works)
 * @param ret       Returned device handle
 */
esp_err_t cst816s_init(i2c_master_bus_handle_t bus, int rst_gpio, int int_gpio, cst816s_handle_t *ret);

/**
 * @brief Read one touch point
 *
 * @param[out] x,y   Coordinates in panel pixels
 * @param[out] pressed true when at least one finger is down
 * @return ESP_OK on a successful transaction (pressed may be false)
 */
esp_err_t cst816s_read(cst816s_handle_t dev, uint16_t *x, uint16_t *y, bool *pressed);

/** @brief Read the chip id register (0xB5 expected for CST816S) */
uint8_t cst816s_get_chip_id(cst816s_handle_t dev);

#ifdef __cplusplus
}
#endif
