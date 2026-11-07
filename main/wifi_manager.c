#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"

#include "mbedtls/base64.h"

#include "app_settings.h"
#include "wifi_manager.h"

static const char *TAG = "wifi";

#define PROV_AP_SSID "Orbis-Setup"

static volatile bool s_connected;
static volatile int s_retry;
static httpd_handle_t s_httpd;

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        if (app_settings_has_wifi()) {
            s_retry++;
            int delay_ms = (s_retry < 30) ? 500 * s_retry : 15000;
            ESP_LOGW(TAG, "disconnected, retry #%d in %d ms", s_retry, delay_ms);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&ev->ip_info.ip));
        s_connected = true;
        s_retry = 0;
    }
}

esp_err_t wifi_manager_init(void)
{
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init failed");
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(err, TAG, "event loop failed");
    }

    static bool netif_created = false;
    if (!netif_created) {
        esp_netif_create_default_wifi_sta();
        esp_netif_create_default_wifi_ap();
        netif_created = true;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init failed");

    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                            &wifi_event_handler, NULL, NULL),
                        TAG, "register wifi event failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                            &wifi_event_handler, NULL, NULL),
                        TAG, "register ip event failed");

    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "set storage failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "set mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");
    esp_wifi_set_ps(WIFI_PS_NONE);
    return ESP_OK;
}

esp_err_t wifi_manager_start(void)
{
    const app_settings_t *cfg = app_settings_get();

    if (!app_settings_has_wifi()) {
        ESP_LOGW(TAG, "no Wi-Fi credentials, starting provisioning AP");
        return wifi_manager_start_provisioning();
    }

    wifi_config_t sta = {0};
    size_t n = strlen(cfg->wifi_ssid);
    if (n > sizeof(sta.sta.ssid)) {
        n = sizeof(sta.sta.ssid);
    }
    memcpy(sta.sta.ssid, cfg->wifi_ssid, n);
    n = strlen(cfg->wifi_pass);
    if (n > sizeof(sta.sta.password)) {
        n = sizeof(sta.sta.password);
    }
    memcpy(sta.sta.password, cfg->wifi_pass, n);
    sta.sta.threshold.authmode = (cfg->wifi_pass[0] != '\0') ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta), TAG, "set sta config failed");
    ESP_LOGI(TAG, "connecting to '%s'", cfg->wifi_ssid);
    return esp_wifi_connect();
}

bool wifi_manager_is_connected(void)
{
    return s_connected;
}

bool wifi_manager_wait_connected(uint32_t timeout_ms)
{
    const TickType_t step = pdMS_TO_TICKS(100);
    for (uint32_t waited = 0; waited < timeout_ms; waited += 100) {
        if (s_connected) {
            return true;
        }
        vTaskDelay(step);
    }
    return s_connected;
}

int wifi_manager_get_rssi(void)
{
    wifi_ap_record_t info;
    if (!s_connected || esp_wifi_sta_get_ap_info(&info) != ESP_OK) {
        return 0;
    }
    return info.rssi;
}

/* ------------------------------------------------------------------------- */
/* 配网页面                                                                    */
/* ------------------------------------------------------------------------- */

/** 设备 ID 会进 MQTT 主题，只允许 a-z 0-9 - _ */
static bool device_id_ok(const char *s)
{
    if (s == NULL || s[0] == '\0') {
        return false;
    }
    size_t n = 0;
    for (const char *p = s; *p; p++, n++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) {
            return false;
        }
    }
    return n < APP_DEVICE_ID_MAX;
}

static esp_err_t send_401(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Orbis\"");
    return httpd_resp_send(req, "authentication required", HTTPD_RESP_USE_STRLEN);
}

/** 校验 Authorization: Basic base64(user:pass) */
static bool http_auth_ok(httpd_req_t *req)
{
    char hdr[192];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK) {
        return false;
    }
    const app_settings_t *cfg = app_settings_get();
    char cred[128];
    snprintf(cred, sizeof(cred), "%s:%s", cfg->web_user, cfg->web_pass);

    unsigned char b64[192];
    size_t olen = 0;
    if (mbedtls_base64_encode(b64, sizeof(b64) - 1, &olen, (const unsigned char *)cred, strlen(cred)) != 0) {
        return false;
    }
    b64[olen] = '\0';

    char expect[224];
    snprintf(expect, sizeof(expect), "Basic %s", (char *)b64);
    return strcmp(hdr, expect) == 0;
}

static const char *PROV_HTML_FMT =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Orbis 控制屏配网</title><style>"
    "body{font-family:system-ui,sans-serif;margin:0;padding:24px;background:#111;color:#eee}"
    "h1{font-size:20px;margin:0 0 16px}p{color:#aaa;font-size:13px}"
    "label{display:block;margin:14px 0 6px;font-size:13px;color:#aaa}"
    "input{width:100%%;padding:10px;border-radius:8px;border:1px solid #333;background:#1b1b1b;color:#eee;box-sizing:border-box}"
    "button{margin-top:22px;width:100%%;padding:12px;border:0;border-radius:8px;background:#3b82f6;color:#fff;font-size:16px}"
    "</style></head><body><h1>Orbis 控制屏</h1>"
    "<p>填写家里 Wi-Fi 和 Home Assistant 连接信息，保存后设备自动重启。</p>"
    "<form method='post' action='/save'>"
    "<label>Wi-Fi 名称 (SSID)</label><input name='ssid' value='%s'>"
    "<label>Wi-Fi 密码</label><input name='pass' type='password' placeholder='已保存，留空表示不修改'>"
    "<label>HA 地址</label><input name='url' value='%s' placeholder='https://ha.example.com'>"
    "<label>HA 长期访问令牌</label><input name='token' type='password' placeholder='已保存，留空表示不修改'>"
    "<label>MQTT 服务器（留空 = 不启用）</label><input name='mqtt_host' value='%s' placeholder='192.168.1.10'>"
    "<label>MQTT 端口</label><input name='mqtt_port' value='%d'>"
    "<label>MQTT 用户名</label><input name='mqtt_user' value='%s'>"
    "<label>MQTT 密码</label><input name='mqtt_pass' type='password' placeholder='已保存，留空表示不修改'>"
    "<label>设备 ID（主题里的名字，a-z 0-9 - _）</label><input name='device_id' value='%s'>"
    "<label>中文显示名（HA 里显示的名字）</label><input name='device_name' value='%s'>"
    "<label>设置页用户名（HTTP Basic Auth）</label><input name='web_user' value='%s'>"
    "<label>设置页密码</label><input name='web_pass' type='password' placeholder='留空表示不修改'>"
    "<button type='submit'>保存并重启</button></form></body></html>";

static int hex2int(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(char *s)
{
    char *out = s;
    for (char *in = s; *in; in++) {
        if (*in == '+') {
            *out++ = ' ';
        } else if (*in == '%' && hex2int(in[1]) >= 0 && hex2int(in[2]) >= 0) {
            *out++ = (char)(hex2int(in[1]) * 16 + hex2int(in[2]));
            in += 2;
        } else {
            *out++ = *in;
        }
    }
    *out = '\0';
}

/* 从 urlencoded body 中取一个字段：先把该字段原样拷到临时缓冲，再解码、再截断。
   两个坑都踩过：
     1. 不能先按 out_len 截断再解码 —— 中文一个字编码成 9 字节，会切成半个字符；
     2. 不能就地改 body（把 '&' 改成 '\0'）—— 后面的字段就再也找不到了。 */
static bool form_get(char *body, const char *key, char *out, size_t out_len)
{
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "%s=", key);
    char *p = strstr(body, pattern);
    if (p == NULL) {
        out[0] = '\0';
        return false;
    }
    p += strlen(pattern);
    const char *end = strchr(p, '&');
    size_t len = end ? (size_t)(end - p) : strlen(p);

    char *scratch = malloc(len + 1);
    if (scratch == NULL) {
        out[0] = '\0';
        return false;
    }
    memcpy(scratch, p, len);
    scratch[len] = '\0';
    url_decode(scratch);          /* 解码后只会变短 */

    size_t decoded = strlen(scratch);
    if (decoded >= out_len) {
        decoded = out_len - 1;
    }
    memcpy(out, scratch, decoded);
    out[decoded] = '\0';
    free(scratch);
    return true;
}

static esp_err_t prov_root_get(httpd_req_t *req)
{
    if (!http_auth_ok(req)) {
        return send_401(req);
    }
    const app_settings_t *cfg = app_settings_get();
    char *page = malloc(4096);
    if (page == NULL) {
        return httpd_resp_send_500(req);
    }
    snprintf(page, 4096, PROV_HTML_FMT, cfg->wifi_ssid, cfg->ha_url, cfg->mqtt_host, cfg->mqtt_port,
             cfg->mqtt_user, cfg->device_id, cfg->device_name, cfg->web_user);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    esp_err_t err = httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
    free(page);
    return err;
}

static esp_err_t prov_save_post(httpd_req_t *req)
{
    if (!http_auth_ok(req)) {
        return send_401(req);
    }
    char body[1024] = {0};
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_FAIL;
    }
    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, body + received, total - received);
        if (r <= 0) {
            return ESP_FAIL;
        }
        received += r;
    }
    body[received] = '\0';

    char ssid[APP_WIFI_SSID_MAX] = {0};
    char pass[APP_WIFI_PASS_MAX] = {0};
    char url[APP_HA_URL_MAX] = {0};
    char token[APP_HA_TOKEN_MAX] = {0};
    char mqtt_host[APP_MQTT_HOST_MAX] = {0};
    char mqtt_port[8] = {0};
    char mqtt_user[APP_MQTT_USER_MAX] = {0};
    char mqtt_pass[APP_MQTT_PASS_MAX] = {0};
    char device_id[APP_DEVICE_ID_MAX] = {0};
    char device_name[APP_DEVICE_NAME_MAX] = {0};
    char web_user[APP_WEB_USER_MAX] = {0};
    char web_pass[APP_WEB_PASS_MAX] = {0};
    form_get(body, "ssid", ssid, sizeof(ssid));
    form_get(body, "pass", pass, sizeof(pass));
    form_get(body, "url", url, sizeof(url));
    form_get(body, "token", token, sizeof(token));
    form_get(body, "mqtt_host", mqtt_host, sizeof(mqtt_host));
    form_get(body, "mqtt_port", mqtt_port, sizeof(mqtt_port));
    form_get(body, "mqtt_user", mqtt_user, sizeof(mqtt_user));
    form_get(body, "mqtt_pass", mqtt_pass, sizeof(mqtt_pass));
    form_get(body, "device_id", device_id, sizeof(device_id));
    form_get(body, "device_name", device_name, sizeof(device_name));
    form_get(body, "web_user", web_user, sizeof(web_user));
    form_get(body, "web_pass", web_pass, sizeof(web_pass));

    if (ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
        return ESP_FAIL;
    }

    if (device_id[0] != '\0' && !device_id_ok(device_id)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "device id: only a-z 0-9 - _ (max 31 chars)");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "saving settings: ssid='%s' ha='%s'", ssid, url);
    /* 空值 = 保持原值（密码/令牌不回填，页面上留空就表示不改） */
    app_settings_update(ssid,
                        pass[0] ? pass : NULL,
                        url[0] ? url : NULL,
                        token[0] ? token : NULL);
    app_settings_update_name(device_name[0] ? device_name : NULL);
    app_settings_update_web(web_user[0] ? web_user : NULL, web_pass[0] ? web_pass : NULL);
    app_settings_update_mqtt(mqtt_host[0] ? mqtt_host : NULL,
                             atoi(mqtt_port),
                             mqtt_user[0] ? mqtt_user : NULL,
                             mqtt_pass[0] ? mqtt_pass : NULL,
                             device_id[0] ? device_id : NULL);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, "<meta charset='utf-8'><h3>已保存，正在重启...</h3>");

    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
    return ESP_OK;
}

esp_err_t wifi_manager_web_start(void)
{
    if (s_httpd == NULL) {
        httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    /* 配置页 handler 里有 1KB 请求体 + 十余个配置缓冲，默认栈不够（曾导致
       "stack overflow in task httpd" 直接重启）。 */
    cfg.stack_size = 8192;
        cfg.lru_purge_enable = true;
        ESP_RETURN_ON_ERROR(httpd_start(&s_httpd, &cfg), TAG, "httpd start failed");

        httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = prov_root_get};
        httpd_uri_t save = {.uri = "/save", .method = HTTP_POST, .handler = prov_save_post};
        httpd_register_uri_handler(s_httpd, &root);
        httpd_register_uri_handler(s_httpd, &save);
    }
    ESP_LOGI(TAG, "settings page: http://<device-ip>/");
    return ESP_OK;
}

esp_err_t wifi_manager_start_provisioning(void)
{
    wifi_config_t ap = {0};
    strncpy((char *)ap.ap.ssid, PROV_AP_SSID, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(PROV_AP_SSID);
    ap.ap.channel = 1;
    ap.ap.max_connection = 2;
    ap.ap.authmode = WIFI_AUTH_OPEN;

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), TAG, "set ap config failed");

    if (s_httpd == NULL) {
        httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    /* 配置页 handler 里有 1KB 请求体 + 十余个配置缓冲，默认栈不够（曾导致
       "stack overflow in task httpd" 直接重启）。 */
    cfg.stack_size = 8192;
        cfg.lru_purge_enable = true;
        ESP_RETURN_ON_ERROR(httpd_start(&s_httpd, &cfg), TAG, "httpd start failed");

        httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = prov_root_get};
        httpd_uri_t save = {.uri = "/save", .method = HTTP_POST, .handler = prov_save_post};
        httpd_register_uri_handler(s_httpd, &root);
        httpd_register_uri_handler(s_httpd, &save);
    }

    ESP_LOGW(TAG, "provisioning AP '%s' up, open http://192.168.4.1", PROV_AP_SSID);
    return ESP_OK;
}

void wifi_manager_get_ip(char *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return;
    }
    snprintf(buf, len, "-");
    if (!s_connected) {
        return;
    }
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    if (netif != NULL && esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
        snprintf(buf, len, IPSTR, IP2STR(&ip.ip));
    }
}
