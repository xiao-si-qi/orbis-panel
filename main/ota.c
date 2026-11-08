/**
 * 固件 OTA：从 Home Assistant 的 /local/orbis/ 拉 manifest 和 bin。
 *
 * manifest（firmware.json）由 HA 插件在“上传固件”时生成：
 *   {"version":"0.2.0","file":"firmware.bin","size":2680000,"sha256":"..."}
 *
 * 版本号用字符串比较（PROJECT_VER 与 manifest.version 不同即视为有新版本），
 * 简单、可预期：想强制下发就把 manifest 的 version 改一下。
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "cJSON.h"

#include "app_settings.h"
#include "mqtt_link.h"
#include "ota.h"

static const char *TAG = "ota";

#define MANIFEST_MAX 1024

static volatile bool s_busy;
static volatile int s_percent = -1;
static char s_status[96] = "";   /* 空 = 空闲，UI 不显示 */
static char s_manifest[MANIFEST_MAX];
static size_t s_manifest_len;

const char *ota_current_version(void)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    return desc ? desc->version : "?";
}

bool ota_busy(void)
{
    return s_busy;
}

int ota_percent(void)
{
    return s_busy ? s_percent : -1;
}

const char *ota_status_text(void)
{
    return s_status;
}

typedef struct {
    char url[256];
    char version[24];
    bool use_manifest;
} ota_req_t;

static esp_err_t http_event(esp_http_client_event_t *ev)
{
    if (ev->event_id == HTTP_EVENT_ON_DATA && ev->data && ev->data_len > 0) {
        if (s_manifest_len + (size_t)ev->data_len < MANIFEST_MAX) {
            memcpy(s_manifest + s_manifest_len, ev->data, ev->data_len);
            s_manifest_len += ev->data_len;
            s_manifest[s_manifest_len] = '\0';
        }
    }
    return ESP_OK;
}

/** 拉 firmware.json，成功时把 version / file 写入 out 缓冲。 */
static esp_err_t fetch_manifest(char *version, size_t vlen, char *file, size_t flen)
{
    const app_settings_t *cfg = app_settings_get();
    if (cfg->ha_url[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    char url[APP_HA_URL_MAX + 64];
    snprintf(url, sizeof(url), "%s/local/switchyard/%s/firmware.json", cfg->ha_url, mqtt_link_device_id());

    esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = 8000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 1024,
        .event_handler = http_event,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_manifest_len = 0;
    s_manifest[0] = '\0';

    esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200 || s_manifest_len == 0) {
        ESP_LOGW(TAG, "manifest %s -> %d (%s)", url, status, esp_err_to_name(err));
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(s_manifest);
    if (root == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const cJSON *jv = cJSON_GetObjectItemCaseSensitive(root, "version");
    const cJSON *jf = cJSON_GetObjectItemCaseSensitive(root, "file");
    bool ok = cJSON_IsString(jv) && cJSON_IsString(jf);
    if (ok) {
        snprintf(version, vlen, "%s", jv->valuestring);
        snprintf(file, flen, "%s", jf->valuestring);
    }
    cJSON_Delete(root);
    return ok ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static void ota_task(void *arg)
{
    ota_req_t *req = (ota_req_t *)arg;
    char version[24] = {0};
    char file[48] = {0};
    char url[320];

    if (req != NULL && req->use_manifest) {
        snprintf(s_status, sizeof(s_status), "检查中…");
        if (fetch_manifest(version, sizeof(version), file, sizeof(file)) != ESP_OK) {
            snprintf(s_status, sizeof(s_status), "没有固件信息");
            goto done;
        }
        const app_settings_t *cfg = app_settings_get();
        snprintf(url, sizeof(url), "%s/local/switchyard/%s/%s", cfg->ha_url, mqtt_link_device_id(), file);
    } else if (req != NULL && req->url[0] != '\0') {
        snprintf(url, sizeof(url), "%s", req->url);
        if (req->version[0]) {
            snprintf(version, sizeof(version), "%s", req->version);
        }
    } else {
        snprintf(s_status, sizeof(s_status), "没有可用版本");
        goto done;
    }

    const char *current = ota_current_version();
    ESP_LOGI(TAG, "current=%s remote=%s", current, version);
    if (version[0] && strcmp(version, current) == 0) {
        snprintf(s_status, sizeof(s_status), "已是最新 %s", current);
        goto done;
    }

    snprintf(s_status, sizeof(s_status), "正在下载 %s", version);
    s_percent = 0;

    esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = 20000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota begin failed: %s", esp_err_to_name(err));
        snprintf(s_status, sizeof(s_status), "开始失败");
        goto done;
    }

    const int total = esp_https_ota_get_image_size(handle);
    while (true) {
        err = esp_https_ota_perform(handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
        if (total > 0) {
            s_percent = (int)((esp_https_ota_get_image_len_read(handle) * 100) / total);
            if (s_percent > 99) {
                s_percent = 99;
            }
        }
    }

    if (err == ESP_OK && esp_https_ota_is_complete_data_received(handle)) {
        s_percent = 100;
        snprintf(s_status, sizeof(s_status), "即将重启…");
        esp_err_t fin = esp_https_ota_finish(handle);
        if (fin == ESP_OK) {
            ESP_LOGI(TAG, "OTA done, restarting");
            vTaskDelay(pdMS_TO_TICKS(800));
            esp_restart();
        }
        ESP_LOGE(TAG, "ota finish failed: %s", esp_err_to_name(fin));
        snprintf(s_status, sizeof(s_status), "写入失败");
    } else {
        ESP_LOGE(TAG, "ota failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(handle);
        snprintf(s_status, sizeof(s_status), "下载失败");
    }

done:
    s_busy = false;
    if (req != NULL) {
        free(req);
    }
    vTaskDelete(NULL);
}

static void ota_launch(ota_req_t *req)
{
    if (s_busy) {
        free(req);
        return;
    }
    s_busy = true;
    s_percent = -1;
    snprintf(s_status, sizeof(s_status), "检查中…");
    xTaskCreate(ota_task, "ota", 8192, req, 5, NULL);
}

void ota_start_check(void)
{
    ota_req_t *req = calloc(1, sizeof(ota_req_t));
    if (req == NULL) {
        return;
    }
    req->use_manifest = true;
    ota_launch(req);
}

void ota_start_url(const char *url, const char *version)
{
    if (url == NULL || url[0] == '\0') {
        return;
    }
    ota_req_t *req = calloc(1, sizeof(ota_req_t));
    if (req == NULL) {
        return;
    }
    req->use_manifest = false;
    snprintf(req->url, sizeof(req->url), "%s", url);
    if (version) {
        snprintf(req->version, sizeof(req->version), "%s", version);
    }
    ota_launch(req);
}
