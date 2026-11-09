#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

#include "bsp_board.h"
#include "bsp_display.h"
#include "bsp_lvgl.h"
#include "bsp_touch.h"

static const char *TAG = "bsp_lvgl";

#define BUF_LINES   10  /* 40 x 360 x 2B = 28.8KB per buffer, two buffers */

static esp_lcd_panel_handle_t s_panel;
static lv_display_t *s_disp;

static uint32_t tick_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* Called from the SPI ISR when the DMA transfer of one area has finished. */
static void flush_done_cb(void *ctx)
{
    lv_display_flush_ready((lv_display_t *)ctx);
}

static void disp_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    const uint32_t w = (uint32_t)(area->x2 - area->x1 + 1);
    const uint32_t h = (uint32_t)(area->y2 - area->y1 + 1);

    /* LVGL renders RGB565 little-endian; the ST77916 expects big-endian bytes */
    for (uint32_t i = 0; i < w * h * 2; i += 2) {
        uint8_t t = px_map[i];
        px_map[i] = px_map[i + 1];
        px_map[i + 1] = t;
    }

    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "draw_bitmap failed: %s", esp_err_to_name(err));
        lv_display_flush_ready(disp);
    }
    /* readiness is signalled from flush_done_cb (SPI transfer complete) */
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    uint16_t x = 0;
    uint16_t y = 0;
    if (bsp_touch_read(&x, &y)) {
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = x;
        data->point.y = y;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

esp_err_t bsp_lvgl_init(esp_lcd_panel_handle_t panel)
{
    s_panel = panel;

    lv_init();
    lv_tick_set_cb(tick_cb);

    s_disp = lv_display_create(BSP_LCD_H_RES, BSP_LCD_V_RES);
    if (s_disp == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const size_t buf_bytes = BSP_LCD_H_RES * BUF_LINES * sizeof(uint16_t);

    /* 必须放内部 DMA RAM：这个面板驱动用 PSRAM 指针会报
       recycle spi transactions failed / send CASET failed。
       省内存靠减少行数（20 -> 10，每块 7.2KB）。 */
    void *buf1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    void *buf2 = heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    const bool in_psram = false;
    if (buf1 == NULL || buf2 == NULL) {
        ESP_LOGE(TAG, "LVGL buffer alloc failed");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "LVGL 绘制缓冲在 %s（%u KB × 2）", in_psram ? "PSRAM" : "内部 RAM",
             (unsigned)(buf_bytes / 1024));

    lv_display_set_buffers(s_disp, buf1, buf2, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_disp, disp_flush_cb);
    lv_display_set_user_data(s_disp, NULL);

    /* 告诉显示层：DMA 传完就把 flush 标记置位，否则 LVGL 会一直等 */
    bsp_display_set_flush_done_hook(flush_done_cb, s_disp);

    lv_indev_t *indev = lv_indev_create();
    if (indev == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);
    lv_indev_set_display(indev, s_disp);

    ESP_LOGI(TAG, "LVGL %d.%d.%d ready, buffers %u KB", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR,
             LVGL_VERSION_PATCH, (unsigned)(buf_bytes * 2 / 1024));
    return ESP_OK;
}

static void lvgl_task(void *arg)
{
    (void)arg;
    while (true) {
        uint32_t delay_ms = lv_timer_handler();

        if (delay_ms > 10) {
            delay_ms = 10;
        }
        if (delay_ms == 0) {
            delay_ms = 1;
        }
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

/** 屏幕方向：0 / 90 / 180 / 270 度。LVGL 会同时旋转画面和触摸坐标。 */
void bsp_lvgl_set_rotation(int deg)
{
    if (s_disp == NULL) {
        return;
    }
    /* LVGL 的 lv_display_set_rotation 只改逻辑坐标系、不旋转像素（需要驱动自己转），
       这里改成面板硬件旋转 + 触摸坐标换算，零 CPU 开销 */
    bsp_display_set_rotation(deg);
    bsp_touch_set_rotation(deg);
    ESP_LOGI(TAG, "屏幕方向设置为 %d 度", deg);
}

void bsp_lvgl_start(void)
{
    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 8192, NULL, 5, NULL, 1);
}
