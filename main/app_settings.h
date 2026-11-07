#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_WIFI_SSID_MAX   33
#define APP_WIFI_PASS_MAX   65
#define APP_HA_URL_MAX      128
#define APP_HA_TOKEN_MAX    256
#define APP_MQTT_HOST_MAX   64
#define APP_MQTT_USER_MAX   32
#define APP_MQTT_PASS_MAX   64
#define APP_DEVICE_ID_MAX   32
#define APP_DEVICE_NAME_MAX 32
#define APP_WEB_USER_MAX    32
#define APP_WEB_PASS_MAX    64

typedef struct {
    char wifi_ssid[APP_WIFI_SSID_MAX];
    char wifi_pass[APP_WIFI_PASS_MAX];
    char ha_url[APP_HA_URL_MAX];
    char ha_token[APP_HA_TOKEN_MAX];
    char mqtt_host[APP_MQTT_HOST_MAX];
    int mqtt_port;
    char mqtt_user[APP_MQTT_USER_MAX];
    char mqtt_pass[APP_MQTT_PASS_MAX];
    char device_id[APP_DEVICE_ID_MAX];
    char device_name[APP_DEVICE_NAME_MAX];   /* 中文显示名，与主题里的 id 分开 */
    char web_user[APP_WEB_USER_MAX];
    char web_pass[APP_WEB_PASS_MAX];
} app_settings_t;

/** 初始化 NVS 并读取配置；NVS 为空时用 secrets.h 的值播种。 */
esp_err_t app_settings_init(void);

/** 只读访问当前配置（进程内单例）。 */
const app_settings_t *app_settings_get(void);

/** 写入一条配置并落盘（字段为 NULL 表示不修改）。 */
esp_err_t app_settings_update(const char *ssid, const char *pass, const char *ha_url, const char *ha_token);

/** 追加的 MQTT / 设备身份配置（任一为 NULL 表示不修改）。 */
esp_err_t app_settings_update_name(const char *name);

esp_err_t app_settings_update_web(const char *user, const char *pass);

esp_err_t app_settings_update_mqtt(const char *host, int port, const char *user, const char *pass,
                                   const char *device_id);

/** Wi-Fi 凭据是否已配置。 */
bool app_settings_has_wifi(void);

/** HA 地址/令牌是否已配置。 */
bool app_settings_has_ha(void);

#ifdef __cplusplus
}
#endif
