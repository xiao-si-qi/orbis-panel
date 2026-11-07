/**
 * MQTT 接入（switchyard 设备协议 v0.1）
 *
 *  - 上线：availability=online（retained）+ state（retained）+ LWT=offline
 *  - 周期：每 30s 上报一次 state
 *  - 下行：订阅 orbis/<id>/cmd，带 seq 去重（最后执行的 seq 存 NVS）
 *          cmd: ota(url,version) / reboot / set(brightness)
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "mqtt_client.h"
#include "nvs.h"

#include "app_settings.h"
#include "bsp_display.h"
#include "ha_client.h"
#include "layout.h"
#include "mqtt_link.h"
#include "ui_home.h"
#include "ota.h"
#include "wifi_manager.h"

static const char *TAG = "mqtt";

#define STATE_PERIOD_MS 30000
#define SEQ_KEY         "cmd_seq"

static esp_mqtt_client_handle_t s_client;
static bool s_connected;
static char s_topic_state[96];
static char s_topic_avail[96];
static char s_topic_cmd[96];
static char s_topic_event[96];
static char s_topic_cfg[96];
static char s_client_id[64];
static uint32_t s_last_seq;

static uint32_t load_seq(void)
{
    nvs_handle_t h;
    uint32_t v = 0;
    if (nvs_open("app", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, SEQ_KEY, &v);
        nvs_close(h);
    }
    return v;
}

static void save_seq(uint32_t seq)
{
    nvs_handle_t h;
    if (nvs_open("app", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, SEQ_KEY, seq);
        nvs_commit(h);
        nvs_close(h);
    }
}

const char *mqtt_link_device_id(void)
{
    const app_settings_t *cfg = app_settings_get();
    return cfg->device_id[0] ? cfg->device_id : "hall-screen";
}

bool mqtt_link_connected(void)
{
    return s_connected;
}

static void publish_retained(const char *topic, const char *payload)
{
    if (s_client && s_connected) {
        esp_mqtt_client_publish(s_client, topic, payload, 0, 0, 1);
    }
}

void mqtt_link_publish_state(void)
{
    if (!s_connected) {
        return;
    }
    char ip[24] = "-";
    wifi_manager_get_ip(ip, sizeof(ip));

    const int64_t up_s = esp_timer_get_time() / 1000000;
    char json[320];
    snprintf(json, sizeof(json),
             "{\"id\":\"%s\",\"type\":\"screen\",\"fw\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,"
             "\"uptime\":%lld,\"heap\":%d,\"ts\":%lld}",
             mqtt_link_device_id(), ota_current_version(), ip, wifi_manager_get_rssi(),
             (long long)up_s, (int)(esp_get_free_heap_size() / 1024), (long long)time(NULL));
    publish_retained(s_topic_state, json);
}

void mqtt_link_publish_event(const char *json)
{
    publish_retained(s_topic_event, json);
}

static void apply_layout_json(const char *json)
{
    layout_t *layout = calloc(1, sizeof(layout_t));
    if (layout == NULL) {
        return;
    }
    if (layout_parse(json, layout) != ESP_OK) {
        ESP_LOGW(TAG, "cfg: layout parse failed");
        free(layout);
        return;
    }
    layout_save_nvs(json);
    for (int p = 0; p < layout->page_count; p++) {
        for (int t = 0; t < layout->pages[p].tile_count; t++) {
            const layout_tile_t *tile = &layout->pages[p].tiles[t];
            ha_client_register(tile->entity, tile->label[0] ? tile->label : NULL);
        }
    }
    ha_client_fetch_all();
    ui_home_apply(layout);
    free(layout);
    mqtt_link_publish_event("{\"ev\":\"cfg_applied\"}");
    ESP_LOGI(TAG, "cfg applied: %d page(s)", layout->page_count);
}

static void handle_cmd(const char *payload)
{
    cJSON *root = cJSON_Parse(payload);
    if (root == NULL) {
        ESP_LOGW(TAG, "bad cmd json: %s", payload);
        return;
    }
    const cJSON *jseq = cJSON_GetObjectItemCaseSensitive(root, "seq");
    const cJSON *jcmd = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    const uint32_t seq = cJSON_IsNumber(jseq) ? (uint32_t)jseq->valuedouble : 0;
    const char *cmd = cJSON_IsString(jcmd) ? jcmd->valuestring : NULL;

    if (seq != 0 && seq <= s_last_seq) {
        ESP_LOGI(TAG, "cmd seq %u already executed (last %u)", seq, s_last_seq);
        cJSON_Delete(root);
        return;
    }
    if (cmd == NULL) {
        cJSON_Delete(root);
        return;
    }
    ESP_LOGI(TAG, "cmd '%s' seq=%u", cmd, seq);

    if (strcmp(cmd, "reboot") == 0) {
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
    } else if (strcmp(cmd, "ota") == 0) {
        const cJSON *jurl = cJSON_GetObjectItemCaseSensitive(root, "url");
        const cJSON *jver = cJSON_GetObjectItemCaseSensitive(root, "version");
        if (cJSON_IsString(jurl)) {
            ota_start_url(jurl->valuestring, cJSON_IsString(jver) ? jver->valuestring : NULL);
        }
    } else if (strcmp(cmd, "set") == 0) {
        const cJSON *jkey = cJSON_GetObjectItemCaseSensitive(root, "key");
        const cJSON *jval = cJSON_GetObjectItemCaseSensitive(root, "value");
        if (cJSON_IsString(jkey) && cJSON_IsNumber(jval) && strcmp(jkey->valuestring, "brightness") == 0) {
            bsp_display_set_backlight((uint8_t)jval->valuedouble);
        }
    }

    if (seq != 0) {
        s_last_seq = seq;
        save_seq(seq);
    }
    cJSON_Delete(root);
}

static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)base;
    esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        ESP_LOGI(TAG, "connected to broker");
        publish_retained(s_topic_avail, "online");
        mqtt_link_publish_state();
        esp_mqtt_client_subscribe(s_client, s_topic_cmd, 0);
        esp_mqtt_client_subscribe(s_client, s_topic_cfg, 0);
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "disconnected");
        break;
    case MQTT_EVENT_DATA: {
        /* 主题可能是分片的；只有 cmd 主题且 data_offset==0 时处理整包 */
        char topic[96] = {0};
        snprintf(topic, sizeof(topic), "%.*s", ev->topic_len, ev->topic);
        if (strcmp(topic, s_topic_cfg) == 0) {
            static char cfg_buf[8192];
            static int cfg_len;
            if (ev->current_data_offset == 0) {
                cfg_len = 0;
            }
            int n = ev->data_len;
            if (cfg_len + n < (int)sizeof(cfg_buf) - 1) {
                memcpy(cfg_buf + cfg_len, ev->data, n);
                cfg_len += n;
                cfg_buf[cfg_len] = '\0';
            }
            if (cfg_len + n >= (int)sizeof(cfg_buf) - 1 || ev->current_data_offset + n >= ev->total_data_len) {
                apply_layout_json(cfg_buf);
                cfg_len = 0;
            }
            break;
        }
        if (strcmp(topic, s_topic_cmd) == 0 && ev->current_data_offset == 0 && ev->data_len == ev->total_data_len) {
            char payload[512];
            size_t n = ev->data_len < (int)sizeof(payload) - 1 ? (size_t)ev->data_len : sizeof(payload) - 1;
            memcpy(payload, ev->data, n);
            payload[n] = '\0';
            handle_cmd(payload);
        }
        break;
    }
    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "mqtt error");
        break;
    default:
        break;
    }
}

static void state_task(void *arg)
{
    (void)arg;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(STATE_PERIOD_MS));
        mqtt_link_publish_state();
    }
}

esp_err_t mqtt_link_start(void)
{
    const app_settings_t *cfg = app_settings_get();
    if (cfg->mqtt_host[0] == '\0') {
        ESP_LOGW(TAG, "mqtt_host not configured, MQTT disabled");
        return ESP_ERR_INVALID_STATE;
    }

    snprintf(s_topic_state, sizeof(s_topic_state), "orbis/%s/state", mqtt_link_device_id());
    snprintf(s_topic_avail, sizeof(s_topic_avail), "orbis/%s/availability", mqtt_link_device_id());
    snprintf(s_topic_cmd, sizeof(s_topic_cmd), "orbis/%s/cmd", mqtt_link_device_id());
    snprintf(s_topic_event, sizeof(s_topic_event), "orbis/%s/event", mqtt_link_device_id());
    snprintf(s_topic_cfg, sizeof(s_topic_cfg), "orbis/%s/cfg", mqtt_link_device_id());
    snprintf(s_client_id, sizeof(s_client_id), "orbis-%s", mqtt_link_device_id());
    s_last_seq = load_seq();

    char uri[128];
    snprintf(uri, sizeof(uri), "mqtt://%s:%d", cfg->mqtt_host, cfg->mqtt_port);

    char lwt_topic[96];
    snprintf(lwt_topic, sizeof(lwt_topic), "orbis/%s/availability", mqtt_link_device_id());

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = uri,
        .credentials.client_id = s_client_id,
        .credentials.username = cfg->mqtt_user[0] ? cfg->mqtt_user : NULL,
        .credentials.authentication.password = cfg->mqtt_pass[0] ? cfg->mqtt_pass : NULL,
        .session.last_will.topic = lwt_topic,
        .session.last_will.msg = "offline",
        .session.last_will.msg_len = 7,
        .session.last_will.qos = 0,
        .session.last_will.retain = 1,
        .session.keepalive = 30,
        .buffer.size = 8192,
        .buffer.out_size = 8192,
    };

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_client == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_err_t err = esp_mqtt_client_start(s_client);
    ESP_LOGI(TAG, "mqtt start: %s (%s), device id '%s'", uri, esp_err_to_name(err), mqtt_link_device_id());

    xTaskCreate(state_task, "mq_state", 4096, NULL, 3, NULL);
    return err;
}
