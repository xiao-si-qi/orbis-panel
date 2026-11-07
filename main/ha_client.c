/**
 * Home Assistant REST 客户端（通用实体关注列表）。
 *
 *  - 复用同一个 esp_http_client + keep-alive，避免每次 TLS 握手；
 *  - 启动时一次拉 /api/states，把关注列表的名字/状态/单位全部填好；
 *  - 之后按 CONFIG_APP_POLL_INTERVAL_S 逐个刷新；
 *  - 不做完整 JSON 解析：只扫描 entity_id / state / friendly_name / unit 字段。
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "app_settings.h"
#include "ha_client.h"

static const char *TAG = "ha";

#define HA_HTTP_TIMEOUT_MS  10000
#define HA_BODY_MAX  (8 * 1024)

static esp_http_client_handle_t s_client;
static SemaphoreHandle_t s_lock;
static char s_base[APP_HA_URL_MAX];
static char s_token[APP_HA_TOKEN_MAX];
static char *s_buf;
static size_t s_buf_len;

static ha_entity_t s_entities[HA_MAX_ENTITIES];
static volatile bool s_online;
static int s_fail_count;
static int s_count;

/* ------------------------------------------------------------------------- */
/* HTTP                                                                       */
/* ------------------------------------------------------------------------- */

static esp_err_t http_event_handler(esp_http_client_event_t *ev)
{
    if (ev->event_id == HTTP_EVENT_ON_DATA && ev->data && ev->data_len > 0) {
        size_t room = HA_BODY_MAX - 1 - s_buf_len;
        size_t n = (size_t)ev->data_len < room ? (size_t)ev->data_len : room;
        if (n > 0) {
            memcpy(s_buf + s_buf_len, ev->data, n);
            s_buf_len += n;
            s_buf[s_buf_len] = '\0';
        }
    }
    return ESP_OK;
}

static esp_err_t client_create(void)
{
    if (s_client) {
        return ESP_OK;
    }
    const esp_http_client_config_t cfg = {
        .url = s_base,
        .timeout_ms = HA_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048,
        .buffer_size_tx = 512,
        .keep_alive_enable = true,
        .event_handler = http_event_handler,
    };
    s_client = esp_http_client_init(&cfg);
    return s_client ? ESP_OK : ESP_ERR_NO_MEM;
}

static void set_auth_header(void)
{
    static char auth[APP_HA_TOKEN_MAX + 16];
    snprintf(auth, sizeof(auth), "Bearer %s", s_token);
    esp_http_client_set_header(s_client, "Authorization", auth);
    esp_http_client_set_header(s_client, "Accept", "application/json");
}

static int http_get(const char *url)
{
    if (client_create() != ESP_OK) {
        return -1;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    set_auth_header();
    esp_http_client_set_method(s_client, HTTP_METHOD_GET);
    esp_http_client_set_url(s_client, url);
    s_buf_len = 0;
    s_buf[0] = '\0';

    int status = -1;
    esp_err_t err = esp_http_client_perform(s_client);
    if (err == ESP_OK) {
        status = esp_http_client_get_status_code(s_client);
    } else {
        ESP_LOGW(TAG, "GET %s failed: %s (internal heap %d KB)", url, esp_err_to_name(err),
                 (int)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
        /* 长连接被服务端/NAT 掐断后复用会一直失败：关掉，下次重新握手 */
        esp_http_client_close(s_client);
    }
    xSemaphoreGive(s_lock);
    if (status > 0) {
        s_online = true;
        s_fail_count = 0;
    } else if (++s_fail_count >= 5) {
        s_online = false;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "GET %s -> %d", url, status);
    }
    return status;
}

static int http_post_service(const char *domain, const char *service, const char *entity_id)
{
    if (client_create() != ESP_OK) {
        return -1;
    }
    char url[APP_HA_URL_MAX + 64];
    snprintf(url, sizeof(url), "%s/api/services/%s/%s", s_base, domain, service);
    char body[128];
    snprintf(body, sizeof(body), "{\"entity_id\":\"%.95s\"}", entity_id);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    set_auth_header();
    esp_http_client_set_header(s_client, "Content-Type", "application/json");
    esp_http_client_set_method(s_client, HTTP_METHOD_POST);
    esp_http_client_set_url(s_client, url);
    esp_http_client_set_post_field(s_client, body, (int)strlen(body));

    int status = -1;
    esp_err_t err = esp_http_client_perform(s_client);
    if (err == ESP_OK) {
        status = esp_http_client_get_status_code(s_client);
    } else {
        ESP_LOGW(TAG, "GET %s failed: %s (internal heap %d KB)", url, esp_err_to_name(err),
                 (int)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    }
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "%s/%s %s -> %d", domain, service, entity_id, status);
    return status;
}

/* ------------------------------------------------------------------------- */
/* 解析工具                                                                    */
/* ------------------------------------------------------------------------- */

static void str_copy(char *dst, size_t dst_len, const char *src)
{
    size_t n = strlen(src);
    if (n >= dst_len) {
        n = dst_len - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static bool json_str_in(const char *start, const char *end, const char *key, char *out, size_t out_len)
{
    char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char *p = strstr(start, pattern);
    if (p == NULL || (end != NULL && p >= end)) {
        out[0] = '\0';
        return false;
    }
    p += strlen(pattern);
    size_t n = 0;
    while (*p != '\0' && *p != '"' && (end == NULL || p < end) && n + 1 < out_len) {
        out[n++] = *p++;
    }
    out[n] = '\0';
    return n > 0;
}

/* ------------------------------------------------------------------------- */
/* 关注列表                                                                    */
/* ------------------------------------------------------------------------- */

esp_err_t ha_client_register(const char *entity_id, const char *label)
{
    if (entity_id == NULL || entity_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_entities[i].entity_id, entity_id) == 0) {
            if (label && label[0] && !s_entities[i].name[0]) {
                str_copy(s_entities[i].name, sizeof(s_entities[i].name), label);
            }
            return ESP_OK;
        }
    }
    if (s_count >= HA_MAX_ENTITIES) {
        ESP_LOGW(TAG, "entity list full (%d)", HA_MAX_ENTITIES);
        return ESP_ERR_NO_MEM;
    }

    ha_entity_t *e = &s_entities[s_count];
    memset(e, 0, sizeof(*e));
    str_copy(e->entity_id, sizeof(e->entity_id), entity_id);
    if (label && label[0] != '\0') {
        str_copy(e->name, sizeof(e->name), label);
    }
    e->valid = true;
    e->fresh = false;
    strcpy(e->state, "?");
    s_count++;
    return ESP_OK;
}

int ha_client_count(void)
{
    return s_count;
}

const ha_entity_t *ha_client_at(int idx)
{
    if (idx < 0 || idx >= s_count) {
        return NULL;
    }
    return &s_entities[idx];
}

const ha_entity_t *ha_client_find(const char *entity_id)
{
    if (entity_id == NULL) {
        return NULL;
    }
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_entities[i].entity_id, entity_id) == 0) {
            return &s_entities[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* 初始化                                                                     */
/* ------------------------------------------------------------------------- */

esp_err_t ha_client_init(void)
{
    const app_settings_t *cfg = app_settings_get();
    if (!app_settings_has_ha()) {
        ESP_LOGE(TAG, "HA url/token missing");
        return ESP_ERR_INVALID_STATE;
    }
    snprintf(s_base, sizeof(s_base), "%s", cfg->ha_url);
    snprintf(s_token, sizeof(s_token), "%s", cfg->ha_token);

    if (s_buf == NULL) {
        s_buf = malloc(HA_BODY_MAX);
        if (s_buf == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    ESP_LOGI(TAG, "HA client ready: %s", s_base);
    return client_create();
}

/* ------------------------------------------------------------------------- */
/* 全量刷新                                                                    */
/* ------------------------------------------------------------------------- */

static void parse_entity_object(ha_entity_t *e, const char *obj_start, const char *obj_end)
{
    char state[24] = {0};
    if (json_str_in(obj_start, obj_end, "state", state, sizeof(state))) {
        str_copy(e->state, sizeof(e->state), state);
        e->on = (strcmp(state, "on") == 0);
        e->fresh = true;
    }
    if (e->name[0] == '\0') {
        char fn[48] = {0};
        if (json_str_in(obj_start, obj_end, "friendly_name", fn, sizeof(fn))) {
            str_copy(e->name, sizeof(e->name), fn);
        } else {
            const char *dot = strchr(e->entity_id, '.');
            const char *obj = dot ? dot + 1 : e->entity_id;
            str_copy(e->name, sizeof(e->name), obj);
        }
    }
    char unit[12] = {0};
    if (json_str_in(obj_start, obj_end, "unit_of_measurement", unit, sizeof(unit))) {
        str_copy(e->unit, sizeof(e->unit), unit);
    }
}

esp_err_t ha_client_fetch_all(void)
{
    /* 只取关注的那几个实体：/api/states 在实体多的 HA 上可能有近 1MB，
       没必要为 5 个实体拉一整包（以前就是这么被截断才导致 HA 红点）。 */
    int ok = 0;
    for (int i = 0; i < s_count; i++) {
        if (ha_client_refresh_at(i) == ESP_OK) {
            ok++;
        }
    }
    ESP_LOGI(TAG, "refreshed %d/%d entity(ies)", ok, s_count);
    return (s_count == 0 || ok > 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t ha_client_refresh_at(int idx)
{
    if (idx < 0 || idx >= s_count) {
        return ESP_ERR_INVALID_ARG;
    }
    ha_entity_t *e = &s_entities[idx];

    char url[APP_HA_URL_MAX + 160];
    snprintf(url, sizeof(url), "%s/api/states/%.95s", s_base, e->entity_id);
    if (http_get(url) != 200) {
        e->fresh = false;
        return ESP_FAIL;
    }
    parse_entity_object(e, s_buf, NULL);
    return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/* 动作                                                                       */
/* ------------------------------------------------------------------------- */

esp_err_t ha_client_action(const char *entity_id, const char *service)
{
    if (entity_id == NULL || service == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    char domain[24] = {0};
    const char *dot = strchr(entity_id, '.');
    size_t dlen = dot ? (size_t)(dot - entity_id) : 0;
    if (dlen == 0 || dlen >= sizeof(domain)) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(domain, entity_id, dlen);

    if (http_post_service(domain, service, entity_id) >= 300) {
        return ESP_FAIL;
    }

    const ha_entity_t *e = ha_client_find(entity_id);
    if (e) {
        ha_entity_t *m = (ha_entity_t *)e;
        if (strcmp(service, "toggle") == 0) {
            m->on = !m->on;
            str_copy(m->state, sizeof(m->state), m->on ? "on" : "off");
        } else if (strcmp(service, "turn_on") == 0) {
            m->on = true;
            str_copy(m->state, sizeof(m->state), "on");
        } else if (strcmp(service, "turn_off") == 0) {
            m->on = false;
            str_copy(m->state, sizeof(m->state), "off");
        }
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/* 轮询                                                                       */
/* ------------------------------------------------------------------------- */

bool ha_client_online(void)
{
    return s_online;
}

static void poll_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));
    while (true) {
        if (s_count == 0) {
            /* 还没收到布局，没有实体可刷：只探测 HA 是否可达 */
            char url[APP_HA_URL_MAX + 16];
            snprintf(url, sizeof(url), "%s/api/", s_base);
            int st = http_get(url);
            ESP_LOGI(TAG, "HA probe -> %d (internal heap %d KB)", st,
                     (int)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
            vTaskDelay(pdMS_TO_TICKS(30000));
            continue;
        }
        for (int i = 0; i < s_count; i++) {
            ha_client_refresh_at(i);
        }
        vTaskDelay(pdMS_TO_TICKS(CONFIG_APP_POLL_INTERVAL_S * 1000));
    }
}

void ha_client_start_poll(void)
{
    xTaskCreate(poll_task, "ha_poll", 6144, NULL, 4, NULL);
}
