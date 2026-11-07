#include <string.h>
#include <stdio.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
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
#define KEY_MQTT_HOST "mqtt_host"
#define KEY_MQTT_PORT "mqtt_port"
#define KEY_MQTT_USER "mqtt_user"
#define KEY_MQTT_PASS "mqtt_pass"
#define KEY_DEVICE_ID "device_id"
#define KEY_DEVICE_NAME "device_name"
#define KEY_WEB_USER  "web_user"
#define KEY_WEB_PASS  "web_pass"

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
    err |= nvs_set_str(h, KEY_MQTT_HOST, s_cfg.mqtt_host);
    err |= nvs_set_u16(h, KEY_MQTT_PORT, (uint16_t)s_cfg.mqtt_port);
    err |= nvs_set_str(h, KEY_MQTT_USER, s_cfg.mqtt_user);
    err |= nvs_set_str(h, KEY_MQTT_PASS, s_cfg.mqtt_pass);
    err |= nvs_set_str(h, KEY_DEVICE_ID, s_cfg.device_id);
    err |= nvs_set_str(h, KEY_DEVICE_NAME, s_cfg.device_name);
    err |= nvs_set_str(h, KEY_WEB_USER, s_cfg.web_user);
    err |= nvs_set_str(h, KEY_WEB_PASS, s_cfg.web_pass);
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
    nvs_read_str(h, KEY_MQTT_HOST, s_cfg.mqtt_host, sizeof(s_cfg.mqtt_host));
    nvs_read_str(h, KEY_MQTT_USER, s_cfg.mqtt_user, sizeof(s_cfg.mqtt_user));
    nvs_read_str(h, KEY_MQTT_PASS, s_cfg.mqtt_pass, sizeof(s_cfg.mqtt_pass));
    nvs_read_str(h, KEY_DEVICE_ID, s_cfg.device_id, sizeof(s_cfg.device_id));
    nvs_read_str(h, KEY_DEVICE_NAME, s_cfg.device_name, sizeof(s_cfg.device_name));
    nvs_read_str(h, KEY_WEB_USER, s_cfg.web_user, sizeof(s_cfg.web_user));
    nvs_read_str(h, KEY_WEB_PASS, s_cfg.web_pass, sizeof(s_cfg.web_pass));
    uint16_t port = 0;
    if (nvs_get_u16(h, KEY_MQTT_PORT, &port) == ESP_OK) {
        s_cfg.mqtt_port = port;
    }
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
    if (s_cfg.mqtt_host[0] == '\0' && APP_SECRET_MQTT_HOST[0] != '\0') {
        copy_str(s_cfg.mqtt_host, sizeof(s_cfg.mqtt_host), APP_SECRET_MQTT_HOST);
        s_cfg.mqtt_port = APP_SECRET_MQTT_PORT;
        copy_str(s_cfg.mqtt_user, sizeof(s_cfg.mqtt_user), APP_SECRET_MQTT_USER);
        copy_str(s_cfg.mqtt_pass, sizeof(s_cfg.mqtt_pass), APP_SECRET_MQTT_PASS);
        seeded = true;
    }
    if (s_cfg.device_name[0] == '\0') {
        copy_str(s_cfg.device_name, sizeof(s_cfg.device_name), APP_SECRET_DEVICE_NAME);
        seeded = true;
    }
    if (s_cfg.web_user[0] == '\0') {
        copy_str(s_cfg.web_user, sizeof(s_cfg.web_user), APP_SECRET_WEB_USER);
        copy_str(s_cfg.web_pass, sizeof(s_cfg.web_pass), APP_SECRET_WEB_PASS);
        ESP_LOGW(TAG, "settings page auth: %s / %s (change me)", s_cfg.web_user, s_cfg.web_pass);
        seeded = true;
    }
    if (s_cfg.device_id[0] == '\0') {
        copy_str(s_cfg.device_id, sizeof(s_cfg.device_id), APP_SECRET_DEVICE_ID);
        seeded = true;
    }
    if (s_cfg.mqtt_port == 0) {
        s_cfg.mqtt_port = 1883;
    }
    if (s_cfg.device_name[0] == '\0') {
        copy_str(s_cfg.device_name, sizeof(s_cfg.device_name), APP_SECRET_DEVICE_NAME);
        seeded = true;
    }
    if (s_cfg.web_user[0] == '\0') {
        copy_str(s_cfg.web_user, sizeof(s_cfg.web_user), APP_SECRET_WEB_USER);
        copy_str(s_cfg.web_pass, sizeof(s_cfg.web_pass), APP_SECRET_WEB_PASS);
        ESP_LOGW(TAG, "settings page auth: %s / %s (change me)", s_cfg.web_user, s_cfg.web_pass);
        seeded = true;
    }
    if (s_cfg.device_id[0] == '\0') {
        uint8_t mac[6] = {0};
        if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
            snprintf(s_cfg.device_id, sizeof(s_cfg.device_id), "orbis-%02x%02x", mac[4], mac[5]);
        } else {
            copy_str(s_cfg.device_id, sizeof(s_cfg.device_id), "orbis-0000");
        }
        ESP_LOGI(TAG, "generated device id: %s", s_cfg.device_id);
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

esp_err_t app_settings_update_name(const char *name)
{
    if (name != NULL && name[0] != '\0') {
        copy_str(s_cfg.device_name, sizeof(s_cfg.device_name), name);
    }
    return config_save();
}

esp_err_t app_settings_update_web(const char *user, const char *pass)
{
    if (user) {
        copy_str(s_cfg.web_user, sizeof(s_cfg.web_user), user);
    }
    if (pass) {
        copy_str(s_cfg.web_pass, sizeof(s_cfg.web_pass), pass);
    }
    return config_save();
}

esp_err_t app_settings_update_mqtt(const char *host, int port, const char *user, const char *pass,
                                   const char *device_id)
{
    if (host) {
        copy_str(s_cfg.mqtt_host, sizeof(s_cfg.mqtt_host), host);
    }
    if (port > 0) {
        s_cfg.mqtt_port = port;
    }
    if (user) {
        copy_str(s_cfg.mqtt_user, sizeof(s_cfg.mqtt_user), user);
    }
    if (pass) {
        copy_str(s_cfg.mqtt_pass, sizeof(s_cfg.mqtt_pass), pass);
    }
    if (device_id) {
        copy_str(s_cfg.device_id, sizeof(s_cfg.device_id), device_id);
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
