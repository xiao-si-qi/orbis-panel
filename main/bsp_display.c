#include "esp_check.h"
#include "esp_log.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"

#include "bsp_board.h"
#include "bsp_display.h"
#include "esp_lcd_st77916.h"

static const char *TAG = "bsp_display";

#define LCD_LEDC_TIMER      LEDC_TIMER_0
#define LCD_LEDC_CHANNEL    LEDC_CHANNEL_0
#define LCD_LEDC_MODE       LEDC_LOW_SPEED_MODE
#define LCD_LEDC_RES        LEDC_TIMER_10_BIT
#define LCD_LEDC_DUTY_MAX   ((1 << 10) - 1)

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
static bsp_display_flush_done_cb_t s_done_cb;
static void *s_done_ctx;
static uint8_t s_backlight = 0;   /* 0-100，最近一次设定的亮度 */

static bool on_color_trans_done(esp_lcd_panel_io_handle_t panel_io,
                                esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    if (s_done_cb) {
        s_done_cb(s_done_ctx);
    }
    return false;
}

static esp_err_t backlight_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = LCD_LEDC_MODE,
        .duty_resolution = LCD_LEDC_RES,
        .timer_num = LCD_LEDC_TIMER,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "ledc timer failed");

    ledc_channel_config_t ch = {
        .gpio_num = BSP_LCD_BL_GPIO,
        .speed_mode = LCD_LEDC_MODE,
        .channel = LCD_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LCD_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    return ledc_channel_config(&ch);
}

void bsp_display_set_backlight(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    s_backlight = percent;
    uint32_t duty = (uint32_t)percent * LCD_LEDC_DUTY_MAX / 100;
    ledc_set_duty(LCD_LEDC_MODE, LCD_LEDC_CHANNEL, duty);
    ledc_update_duty(LCD_LEDC_MODE, LCD_LEDC_CHANNEL);
}

/** 当前亮度（0-100），给状态上报用。 */
uint8_t bsp_display_get_brightness(void)
{
    return s_backlight;
}

void bsp_display_screen_on(bool on)
{
    bsp_display_set_backlight(on ? (s_backlight ? s_backlight : 80) : 0);
}

void bsp_display_set_flush_done_hook(bsp_display_flush_done_cb_t cb, void *user_ctx)
{
    s_done_cb = cb;
    s_done_ctx = user_ctx;
}

esp_lcd_panel_handle_t bsp_display_panel(void)
{
    return s_panel;
}

esp_err_t bsp_display_init(esp_lcd_panel_handle_t *ret_panel,
                           bsp_display_flush_done_cb_t done_cb, void *done_ctx)
{
    s_done_cb = done_cb;
    s_done_ctx = done_ctx;

    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "backlight init failed");
    bsp_display_set_backlight(0);

    /* ---- QSPI bus: SCK on 1 line, D0..D3 for data ---- */
    spi_bus_config_t buscfg = {
        .sclk_io_num = BSP_LCD_SCK_GPIO,
        .data0_io_num = BSP_LCD_D0_GPIO,
        .data1_io_num = BSP_LCD_D1_GPIO,
        .data2_io_num = BSP_LCD_D2_GPIO,
        .data3_io_num = BSP_LCD_D3_GPIO,
        .max_transfer_sz = BSP_LCD_H_RES * 80 * sizeof(uint16_t),
        .flags = SPICOMMON_BUSFLAG_MASTER,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BSP_LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO),
                        TAG, "spi bus init failed");

    /* 32-bit command field carries the QSPI opcode; no D/C line on this panel */
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = BSP_LCD_CS_GPIO,
        .dc_gpio_num = -1,
        .spi_mode = 0,
        .pclk_hz = BSP_LCD_PIXEL_CLOCK_HZ,
        .trans_queue_depth = 10,
        .on_color_trans_done = on_color_trans_done,
        .user_ctx = NULL,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .flags = {
            .quad_mode = 1,
        },
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_HOST, &io_cfg, &s_io),
                        TAG, "panel io create failed");

    esp_lcd_st77916_config_t panel_cfg = {
        .reset_gpio_num = BSP_LCD_RST_GPIO,
        .reset_active_high = false,
        .use_qspi = true,
        .init_cmds = NULL,
        .init_cmds_size = 0,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st77916(s_io, &panel_cfg, &s_panel),
                        TAG, "panel create failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true), TAG, "invert failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "disp on failed");

    bsp_display_set_backlight(80);

    if (ret_panel) {
        *ret_panel = s_panel;
    }
    ESP_LOGI(TAG, "ST77916 ready: %dx%d QSPI @%d Hz", BSP_LCD_H_RES, BSP_LCD_V_RES, BSP_LCD_PIXEL_CLOCK_HZ);
    return ESP_OK;
}
