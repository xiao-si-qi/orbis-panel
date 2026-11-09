/**
 * @file xiaozhi_client.c
 * @brief 小智 AI 服务端接入（阶段③：OTA 发现 + WebSocket hello 握手，暂不传音频）。
 *
 * 线程约定：
 *   - 发现/连接在独立任务里做，不阻塞 LVGL 和 MQTT；
 *   - 状态文字是静态缓冲，语音页轮询读取即可。
 *
 * 内存约定：
 *   - 发现用一次 HTTPS（esp_http_client），完成后立刻释放；
 *   - 然后才建 WebSocket，避免两条 TLS 同时占用内部 RAM。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_websocket_client.h"

#include "bg_image.h"
#include "xiaozhi_client.h"

static const char *TAG = "xiaozhi";

#define XZ_ENDPOINT_MAX 128
#define XZ_URL_MAX      192
#define XZ_TOKEN_MAX    96
#define XZ_STATUS_MAX   72
#define XZ_NOTE_MAX     40
#define XZ_DISCOVER_MAX (24 * 1024)
#define XZ_WS_BUF       192

static bool s_enabled;
static char s_endpoint[XZ_ENDPOINT_MAX];
static char s_ws_url[XZ_URL_MAX];
static char s_token[XZ_TOKEN_MAX];
static char s_client_id[40];
static char s_device_id[24];
static char s_status[XZ_STATUS_MAX];
static char s_note[XZ_NOTE_MAX];

static volatile bool s_ready;
static volatile int s_gen;
static volatile bool s_task_alive;
static esp_websocket_client_handle_t s_ws;

/* hello 可能被分片，这里做个小累积缓冲 */
static char s_rx[XZ_WS_BUF];
static size_t s_rx_len;

/* ------------------------------ 小工具 ------------------------------ */

static void set_status(const char *text)
{
    snprintf(s_status, sizeof(s_status), "%s", text);
    ESP_LOGI(TAG, "%s", text);
}

/** 用 MAC 生成稳定的、UUID 形态的 Client-Id（小智服务端要求合法 UUID）。 */
static void init_ids(void)
{
    if (s_client_id[0] != '\0') {
        return;
    }
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_device_id, sizeof(s_device_id), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(s_client_id, sizeof(s_client_id),
             "%02x%02x%02x%02x-%02x%02x-4%02x%01x-8%02x%01x-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
             mac[0], mac[1] >> 4, mac[2], mac[3] >> 4,
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* --------------------------- 1) OTA 发现 --------------------------- */

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} http_buf_t;

static esp_err_t discover_event(esp_http_client_event_t *ev)
{
    http_buf_t *ctx = (http_buf_t *)ev->user_data;
    if (ev->event_id == HTTP_EVENT_ON_DATA && ctx != NULL && ev->data_len > 0) {
        const size_t n = (size_t)ev->data_len;
        if (ctx->len + n + 1 < ctx->cap) {
            memcpy(ctx->buf + ctx->len, ev->data, n);
            ctx->len += n;
            ctx->buf[ctx->len] = '\0';
        }
    }
    return ESP_OK;
}

static bool discover_ws_url(void)
{
    char *buf = calloc(1, XZ_DISCOVER_MAX);
    if (buf == NULL) {
        set_status("内存不足");
        return false;
    }

    char body[320];
    snprintf(body, sizeof(body),
             "{\"version\":2,\"mac_address\":\"%s\",\"uuid\":\"%s\","
             "\"application\":{\"name\":\"orbis-panel\",\"version\":\"1\"},"
             "\"board\":{\"type\":\"jc3636w518\",\"name\":\"orbis\"}}",
             s_device_id, s_client_id);

    http_buf_t ctx = {.buf = buf, .len = 0, .cap = XZ_DISCOVER_MAX};
    esp_http_client_config_t cfg = {
        .url = s_endpoint,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 12000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = discover_event,
        .user_data = &ctx,
        .buffer_size = 4096,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        free(buf);
        set_status("HTTP 初始化失败");
        return false;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Device-Id", s_device_id);
    esp_http_client_set_header(client, "Client-Id", s_client_id);
    esp_http_client_set_header(client, "Activation-Version", "1");
    esp_http_client_set_header(client, "User-Agent", "orbis-panel/1");
    esp_http_client_set_post_field(client, body, (int)strlen(body));

    const esp_err_t err = esp_http_client_perform(client);
    const int http_status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || http_status >= 400 || ctx.len == 0) {
        ESP_LOGW(TAG, "发现失败: %s HTTP %d %d 字节", esp_err_to_name(err), http_status, (int)ctx.len);
        snprintf(s_note, sizeof(s_note), "发现失败 HTTP %d", http_status);
        free(buf);
        set_status("服务端不可达");
        return false;
    }

    bool ok = false;
    cJSON *root = cJSON_Parse(buf);
    if (root != NULL) {
        const cJSON *ws = cJSON_GetObjectItemCaseSensitive(root, "websocket");
        const cJSON *url = cJSON_IsObject(ws) ? cJSON_GetObjectItemCaseSensitive(ws, "url") : NULL;
        if (cJSON_IsString(url) && url->valuestring[0] != '\0') {
            snprintf(s_ws_url, sizeof(s_ws_url), "%s", url->valuestring);
            const cJSON *tok = cJSON_GetObjectItemCaseSensitive(ws, "token");
            if (cJSON_IsString(tok)) {
                snprintf(s_token, sizeof(s_token), "%s", tok->valuestring);
            }
            ok = true;
        }
        snprintf(s_note, sizeof(s_note), "%s",
                 strstr(s_ws_url, "api.tenclass.net") ? "官方服务端" : "自建服务端");
        cJSON_Delete(root);
    }
    free(buf);

    if (!ok) {
        set_status("返回里没有 websocket 地址");
        return false;
    }
    ESP_LOGI(TAG, "发现成功: %s (%s)", s_ws_url, s_note);
    return true;
}

/* --------------------------- 2) WebSocket --------------------------- */

static void handle_text_message(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        return;
    }
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (cJSON_IsString(type)) {
        if (strcmp(type->valuestring, "hello") == 0) {
            const cJSON *sid = cJSON_GetObjectItemCaseSensitive(root, "session_id");
            s_ready = true;
            if (cJSON_IsString(sid) && sid->valuestring[0] != '\0') {
                snprintf(s_status, sizeof(s_status), "已连接 %.20s", sid->valuestring);
                ESP_LOGI(TAG, "握手完成，session=%s", sid->valuestring);
            } else {
                set_status("已连接");
            }
        } else if (strcmp(type->valuestring, "tts") == 0) {
            /* 阶段④ 处理音频；现在只记一笔 */
            const cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
            if (cJSON_IsString(state)) {
                ESP_LOGI(TAG, "tts state=%s", state->valuestring);
            }
        }
    }
    cJSON_Delete(root);
}

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    esp_websocket_event_data_t *ev = (esp_websocket_event_data_t *)data;

    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED: {
        ESP_LOGI(TAG, "WebSocket 已连接，发送 hello");
        const char hello[] =
            "{\"type\":\"hello\",\"version\":1,\"transport\":\"websocket\","
            "\"features\":{\"mcp\":false,\"aec\":false},"
            "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":16000,"
            "\"channels\":1,\"frame_duration\":60}}";
        esp_websocket_client_send_text(s_ws, hello, (int)strlen(hello), pdMS_TO_TICKS(5000));
        set_status("握手 …");
        break;
    }
    case WEBSOCKET_EVENT_DATA: {
        if (ev == NULL || ev->data_len <= 0) {
            break;
        }
        /* 0x01 = 文本帧；音频帧 0x02 留到阶段④ */
        if (ev->op_code != 0x01) {
            break;
        }
        if (ev->payload_offset == 0) {
            s_rx_len = 0;
        }
        const size_t n = (size_t)ev->data_len;
        if (s_rx_len + n < sizeof(s_rx)) {
            memcpy(s_rx + s_rx_len, ev->data_ptr, n);
            s_rx_len += n;
            s_rx[s_rx_len] = '\0';
        }
        const size_t total = (size_t)ev->payload_offset + n;
        if (ev->payload_len > 0 && total >= (size_t)ev->payload_len) {
            ESP_LOGI(TAG, "收到文本: %.160s", s_rx);
            handle_text_message(s_rx);
            s_rx_len = 0;
        }
        break;
    }
    case WEBSOCKET_EVENT_DISCONNECTED:
        s_ready = false;
        set_status("连接断开");
        break;
    case WEBSOCKET_EVENT_ERROR:
        s_ready = false;
        set_status("连接出错");
        break;
    default:
        break;
    }
}

/* ----------------------------- 工作线程 ----------------------------- */

static void xz_task(void *arg)
{
    const int my_gen = (int)(intptr_t)arg;
    s_task_alive = true;

    if (my_gen != s_gen) {
        goto out;
    }
    /* TLS 证书校验需要正确的系统时间：开机初期 SNTP 还没同步，先等一会儿 */
    for (int i = 0; i < 40; i++) {
        if (time(NULL) > 1700000000) {
            break;
        }
        if (my_gen != s_gen) {
            goto out;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    /* 语音要长期占一条 TLS：先把背景图下载停掉，避免两条 TLS 抢内部 RAM */
    bg_image_set_paused(true);

    set_status("发现服务端 …");
    if (!discover_ws_url() || my_gen != s_gen) {
        goto out;
    }

    set_status("连接服务端 …");
    esp_websocket_client_config_t ws_cfg = {
        .uri = s_ws_url,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048,
        .task_stack = 4096,
    };
    s_ws = esp_websocket_client_init(&ws_cfg);
    if (s_ws == NULL) {
        set_status("WebSocket 初始化失败");
        goto out;
    }
    if (s_token[0] != '\0') {
        esp_websocket_client_append_header(s_ws, "Authorization", s_token);
    }
    esp_websocket_client_append_header(s_ws, "Protocol-Version", "1");
    esp_websocket_client_append_header(s_ws, "Device-Id", s_device_id);
    esp_websocket_client_append_header(s_ws, "Client-Id", s_client_id);

    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event, NULL);
    if (esp_websocket_client_start(s_ws) != ESP_OK) {
        set_status("WebSocket 启动失败");
        goto out;
    }

    while (my_gen == s_gen) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

out:
    if (s_ws != NULL) {
        esp_websocket_client_stop(s_ws);
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
    }
    s_ready = false;
    if (my_gen == s_gen) {
        set_status("未连接");
    }
    bg_image_set_paused(false);      /* 退出时把背景图还回去 */
    s_task_alive = false;
    vTaskDelete(NULL);
}

/* ------------------------------ 对外接口 ------------------------------ */

void xiaozhi_client_configure(bool enabled, const char *endpoint)
{
    init_ids();

    const bool want_on = enabled && endpoint != NULL && endpoint[0] != '\0';
    if (!want_on) {
        if (!s_enabled) {
            return;
        }
        s_enabled = false;
        s_gen++;                 /* 让旧任务退出 */
        s_ready = false;
        snprintf(s_status, sizeof(s_status), "未启用");
        return;
    }

    if (s_enabled && strcmp(endpoint, s_endpoint) == 0) {
        return;                  /* 配置没变 */
    }

    s_enabled = true;
    snprintf(s_endpoint, sizeof(s_endpoint), "%s", endpoint);
    s_gen++;
    const int gen = s_gen;

    /* 等旧任务退出，避免两个 WebSocket 同时在线 */
    for (int i = 0; i < 30 && s_task_alive; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    snprintf(s_status, sizeof(s_status), "准备连接 …");
    if (xTaskCreate(xz_task, "xiaozhi", 7168, (void *)(intptr_t)gen, 4, NULL) != pdPASS) {
        s_enabled = false;
        snprintf(s_status, sizeof(s_status), "任务创建失败");
    }
}

const char *xiaozhi_client_status_text(void)
{
    return s_status[0] != '\0' ? s_status : "未启用";
}

bool xiaozhi_client_ready(void)
{
    return s_ready;
}

const char *xiaozhi_client_server_note(void)
{
    return s_note;
}
