#pragma once

/*
 * JC3636W518 board pin map (ESP32-S3R8 + 1.85" 360x360 ST77916 + CST816S).
 * Values taken from the vendor demo pincfg.h (Demo_Arduino / Demo_Arduino_V2.0)
 * and cross-checked against schematic sheet 4_LCD.pdf.
 */

/* ---------------- Display: ST77916 over 4-line QSPI ---------------- */
#define BSP_LCD_H_RES          360
#define BSP_LCD_V_RES          360

#define BSP_LCD_BL_GPIO        15   /* backlight, LEDC PWM */
#define BSP_LCD_RST_GPIO       47
#define BSP_LCD_CS_GPIO        10
#define BSP_LCD_SCK_GPIO       9
#define BSP_LCD_D0_GPIO        11
#define BSP_LCD_D1_GPIO        12
#define BSP_LCD_D2_GPIO        13
#define BSP_LCD_D3_GPIO        14

#define BSP_LCD_SPI_HOST       SPI2_HOST
#define BSP_LCD_PIXEL_CLOCK_HZ (40 * 1000 * 1000)  /* vendor demo uses 50MHz, 40MHz is the safe start */

/* ---------------- Touch: CST816S over I2C ---------------- */
#define BSP_TOUCH_I2C_PORT     I2C_NUM_0
#define BSP_TOUCH_SDA_GPIO     7
#define BSP_TOUCH_SCL_GPIO     8
#define BSP_TOUCH_RST_GPIO     40
#define BSP_TOUCH_INT_GPIO     41

/* ---------------- microSD (4-bit SDMMC) ---------------- */
#define BSP_SD_CLK_GPIO        3
#define BSP_SD_CMD_GPIO        4
#define BSP_SD_D0_GPIO         2
#define BSP_SD_D1_GPIO         1
#define BSP_SD_D2_GPIO         6
#define BSP_SD_D3_GPIO         5
#define BSP_SD_MOUNT_POINT     "/sdcard"

/* ---------------- Misc ---------------- */
#define BSP_BUTTON_GPIO        0

/* ---------------- Audio (PCM5100A DAC + I2S mic, not wired up yet) ---------------- */
#define BSP_I2S_BCK_GPIO       18
#define BSP_I2S_WS_GPIO        16
#define BSP_I2S_DOUT_GPIO      17
#define BSP_AUDIO_MUTE_GPIO    48
#define BSP_MIC_WS_GPIO        45
#define BSP_MIC_SD_GPIO        46
#define BSP_MIC_SCK_GPIO       42
