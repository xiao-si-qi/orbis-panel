#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "app_settings.h"
#include "secrets.h"

static const char *TAG = "settings";

#define NVS_NAMESPACE "app"
#define KEY_SSID      "ssid"
#define KEY_PASS      "pass"
#define KEY_URL       "url"
#define KEY_TOKEN     "token"

static app_settings_t s_cfg;

static void copy_str(char *dst, size_t dst_len, const char *src)
{
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, dst_len - 1);
    dst[dst_len - 1] = '\0';
}

static esp_err_t nvs_read_str(nvs_handle_t h, const char *key, char *out, size_t out_len)
{
    size_t len = out_len;
    esp_err_t err = nvs_get_str(h, key, out, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        out[0] = '\0';
        return ESP_OK;
    }
    return err;
}

static esp_err_t config_save(void)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h), TAG, "nvs open failed");

    esp_err_t err = ESP_OK;
    err |= nvs_set_str(h, KEY_SSID, s_cfg.wifi_ssid);
    err |= nvs_set_str(h, KEY_PASS, s_cfg.wifi_pass);
    err |= nvs_set_str(h, KEY_URL, s_cfg.ha_url);
    err |= nvs_set_str(h, KEY_TOKEN, s_cfg.ha_token);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t app_settings_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "erasing NVS (%s)", esp_err_to_name(err));
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs erase failed");
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs init failed");

    memset(&s_cfg, 0, sizeof(s_cfg));

    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h), TAG, "nvs open failed");
    nvs_read_str(h, KEY_SSID, s_cfg.wifi_ssid, sizeof(s_cfg.wifi_ssid));
    nvs_read_str(h, KEY_PASS, s_cfg.wifi_pass, sizeof(s_cfg.wifi_pass));
    nvs_read_str(h, KEY_URL, s_cfg.ha_url, sizeof(s_cfg.ha_url));
    nvs_read_str(h, KEY_TOKEN, s_cfg.ha_token, sizeof(s_cfg.ha_token));
    nvs_close(h);

    /* First boot: seed from secrets.h so a freshly flashed board just works */
    bool seeded = false;
    if (s_cfg.wifi_ssid[0] == '\0' && APP_SECRET_WIFI_SSID[0] != '\0') {
        copy_str(s_cfg.wifi_ssid, sizeof(s_cfg.wifi_ssid), APP_SECRET_WIFI_SSID);
        copy_str(s_cfg.wifi_pass, sizeof(s_cfg.wifi_pass), APP_SECRET_WIFI_PASSWORD);
        seeded = true;
    }
    if (s_cfg.ha_url[0] == '\0' && APP_SECRET_HA_URL[0] != '\0') {
        copy_str(s_cfg.ha_url, sizeof(s_cfg.ha_url), APP_SECRET_HA_URL);
        copy_str(s_cfg.ha_token, sizeof(s_cfg.ha_token), APP_SECRET_HA_TOKEN);
        seeded = true;
    }
    if (seeded) {
        ESP_LOGI(TAG, "seeding settings from secrets.h");
        ESP_RETURN_ON_ERROR(config_save(), TAG, "seed failed");
    }

    ESP_LOGI(TAG, "wifi ssid: '%s' (%s), ha: '%s' (%s)",
             s_cfg.wifi_ssid, s_cfg.wifi_ssid[0] ? "set" : "empty",
             s_cfg.ha_url, s_cfg.ha_token[0] ? "token set" : "no token");
    return ESP_OK;
}

const app_settings_t *app_settings_get(void)
{
    return &s_cfg;
}

esp_err_t app_settings_update(const char *ssid, const char *pass, const char *ha_url, const char *ha_token)
{
    if (ssid) {
        copy_str(s_cfg.wifi_ssid, sizeof(s_cfg.wifi_ssid), ssid);
    }
    if (pass) {
        copy_str(s_cfg.wifi_pass, sizeof(s_cfg.wifi_pass), pass);
    }
    if (ha_url) {
        copy_str(s_cfg.ha_url, sizeof(s_cfg.ha_url), ha_url);
        /* tolerate a trailing slash */
        size_t n = strlen(s_cfg.ha_url);
        while (n > 0 && s_cfg.ha_url[n - 1] == '/') {
            s_cfg.ha_url[--n] = '\0';
        }
    }
    if (ha_token) {
        copy_str(s_cfg.ha_token, sizeof(s_cfg.ha_token), ha_token);
    }
    return config_save();
}

bool app_settings_has_wifi(void)
{
    return s_cfg.wifi_ssid[0] != '\0';
}

bool app_settings_has_ha(void)
{
    return s_cfg.ha_url[0] != '\0' && s_cfg.ha_token[0] != '\0';
}
