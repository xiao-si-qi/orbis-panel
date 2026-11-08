/*
 * Orbis Panel · JC3636W518 圆屏 Home Assistant 控制屏
 *
 * 启动顺序：
 *   1. NVS / 配置
 *   2. 屏幕 + 触摸 + LVGL（先出启动页，避免网络阻塞时黑屏）
 *   3. 后台任务：连 Wi-Fi → 拉 JSON 布局 → 注册实体 → 一次全量拉状态 → 建界面
 *      （没有 JSON 布局时退回自动挑选 灯/场景/传感器）
 *   4. 后台轮询实体状态
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_lcd_panel_ops.h"

#include "app_settings.h"
#include "bg_cache.h"
#include "bg_image.h"
#include "bsp_display.h"
#include "bsp_lvgl.h"
#include "bsp_touch.h"
#include "ha_client.h"
#include "esp_sntp.h"
#include "layout.h"
#include "mqtt_link.h"
#include "ui_home.h"
#include "wifi_manager.h"

static const char *TAG = "app";

static void register_layout_entities(const layout_t *layout)
{
    for (int p = 0; p < layout->page_count; p++) {
        for (int t = 0; t < layout->pages[p].tile_count; t++) {
            const layout_tile_t *tile = &layout->pages[p].tiles[t];
            ha_client_register(tile->entity, tile->label[0] ? tile->label : NULL);
        }
    }
    ESP_LOGI(TAG, "registered %d entity(ies) from layout", ha_client_count());
}

static void network_task(void *arg)
{
    (void)arg;

    ESP_ERROR_CHECK(wifi_manager_init());
    wifi_manager_start();

    if (!wifi_manager_wait_connected(30000)) {
        ESP_LOGW(TAG, "Wi-Fi not connected yet, starting provisioning AP as a fallback");
        wifi_manager_start_provisioning();
        while (!wifi_manager_is_connected()) {
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
    ESP_LOGI(TAG, "Wi-Fi connected, rssi=%d dBm", wifi_manager_get_rssi());

    /* 时间同步（MQTT state 里的 ts 需要真实时间） */
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    if (ha_client_init() != ESP_OK) {
        ESP_LOGE(TAG, "HA client init failed (check provisioning page)");
        vTaskDelete(NULL);
        return;
    }

    /* 先把配置页和 MQTT 起起来：它们不依赖 HA 的 REST，
       不会被后面较慢的 /api/states 请求堵住 */
    wifi_manager_web_start();

    if (mqtt_link_start() == ESP_OK) {
        ESP_LOGI(TAG, "MQTT enabled, device id '%s'", mqtt_link_device_id());
    }

    /* 布局只走 MQTT：NVS 里是上一次下发的，没收到过就只显示 System 页 */
    /* layout_t 现在约 11KB（6 页 × 12 卡片），本任务栈只有 8KB：必须放堆上 */
    layout_t *layout = calloc(1, sizeof(layout_t));
    if (layout != NULL && layout_load_nvs(layout) == ESP_OK) {
        /* 开机离线时也要用配置里的方向 */
        lv_lock();
        bsp_lvgl_set_rotation(layout->rotation);
        lv_unlock();
        register_layout_entities(layout);
        ha_client_fetch_all();
        ui_home_apply(layout);
        ESP_LOGI(TAG, "layout restored from NVS (%d page(s))", layout->page_count);
        free(layout);
    } else {
        ESP_LOGW(TAG, "no layout yet - waiting for switchyard/%s/cfg", mqtt_link_device_id());
        ui_home_apply(NULL);
    }

    ha_client_start_poll();
    ESP_LOGI(TAG, "polling %d entity(ies)", ha_client_count());
    vTaskDelete(NULL);
}

void app_main(void)
{
#define MEMSTEP(tag_name) \
    ESP_LOGI(TAG, "MEMSTEP %-16s internal=%dKB largest=%dKB psram=%dKB", tag_name, \
             (int)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024), \
             (int)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024), \
             (int)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024))

    ESP_LOGI(TAG, "Orbis Panel booting (JC3636W518), internal heap %d KB, psram %d KB",
             (int)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (int)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

    /* 关键：让 >=2KB 的普通 malloc 走 PSRAM（8MB），别再挤内部 RAM。
       DMA/IRAM 强约束的分配不受影响（它们显式指定了 caps）。 */
    heap_caps_malloc_extmem_enable(2048);

    MEMSTEP("boot");
    ESP_ERROR_CHECK(app_settings_init());
    MEMSTEP("settings");

    /* 背景/卡片默认值要在第一次建 UI 之前就位，否则首屏会闪一下透明卡片 */
    bg_image_init();
    bg_cache_init();

    esp_lcd_panel_handle_t panel = NULL;
    ESP_ERROR_CHECK(bsp_display_init(&panel, NULL, NULL));
    MEMSTEP("display");
    ESP_ERROR_CHECK(bsp_touch_init());
    MEMSTEP("touch");
    ESP_ERROR_CHECK(bsp_lvgl_init(panel));
    MEMSTEP("lvgl");

    ui_home_boot();
    bsp_lvgl_start();
    MEMSTEP("boot-ui");

    /* 新固件跑起来（LVGL 已出画面）才算有效：配合 APP_ROLLBACK_ENABLE，
       如果新固件在到达这一步之前就崩了/断电，下次启动会自动回滚到旧固件 */
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (running && esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
        ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
            ESP_LOGI(TAG, "firmware marked valid (rollback cancelled)");
        }
    }

    MEMSTEP("pre-net");
    xTaskCreate(network_task, "net", 8192, NULL, 4, NULL);
}
