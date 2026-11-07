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

typedef struct {
    char wifi_ssid[APP_WIFI_SSID_MAX];
    char wifi_pass[APP_WIFI_PASS_MAX];
    char ha_url[APP_HA_URL_MAX];
    char ha_token[APP_HA_TOKEN_MAX];
} app_settings_t;

/** 初始化 NVS 并读取配置；NVS 为空时用 secrets.h 的值播种。 */
esp_err_t app_settings_init(void);

/** 只读访问当前配置（进程内单例）。 */
const app_settings_t *app_settings_get(void);

/** 写入一条配置并落盘（字段为 NULL 表示不修改）。 */
esp_err_t app_settings_update(const char *ssid, const char *pass, const char *ha_url, const char *ha_token);

/** Wi-Fi 凭据是否已配置。 */
bool app_settings_has_wifi(void);

/** HA 地址/令牌是否已配置。 */
bool app_settings_has_ha(void);

#ifdef __cplusplus
}
#endif
