/**
 * Home Assistant REST 客户端（通用实体关注列表）。
 *
 *  - 复用同一个 esp_http_client + keep-alive，避免每次 TLS 握手；
 *  - 启动时一次拉 /api/states，把关注列表的名字/状态/单位全部填好；
 *  - 之后按 CONFIG_APP_POLL_INTERVAL_S 逐个刷新；
 *  - 不做完整 JSON 解析：只扫描 entity_id / state / friendly_name / unit 字段。
 */

#include <string.h>
#include <time.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdio.h>
#include <math.h>
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

#include "cJSON.h"
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

/* 历史请求临时把响应写到 PSRAM 大缓冲里（复用同一条 TLS 连接，避免再开一条） */
static char *s_sink;
static size_t s_sink_len;
static size_t s_sink_cap;

static esp_err_t http_event_handler(esp_http_client_event_t *ev)
{
    if (ev->event_id == HTTP_EVENT_ON_DATA && s_sink != NULL && ev->data_len > 0) {
        const size_t n = (size_t)ev->data_len;
        if (s_sink_len + n < s_sink_cap) {
            memcpy(s_sink + s_sink_len, ev->data, n);
            s_sink_len += n;
            s_sink[s_sink_len] = '\0';
        }
        return ESP_OK;
    }

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

static int http_post_service_ext(const char *domain, const char *service, const char *entity_id,
                                 const char *attrs_json)
{
    if (client_create() != ESP_OK) {
        return -1;
    }
    char url[APP_HA_URL_MAX + 64];
    snprintf(url, sizeof(url), "%s/api/services/%s/%s", s_base, domain, service);
    char body[320];
    if (attrs_json != NULL && attrs_json[0] == '{') {
        /* attrs_json 形如 {"brightness_pct":60}：跳过开头的 '{'，它自带结尾的 '}' */
        snprintf(body, sizeof(body), "{\"entity_id\":\"%.95s\",", entity_id);
        strncat(body, attrs_json + 1, sizeof(body) - strlen(body) - 1);
    } else if (attrs_json != NULL && attrs_json[0] != '\0') {
        snprintf(body, sizeof(body), "{\"entity_id\":\"%.95s\",%.200s}", entity_id, attrs_json + 1);
    } else {
        snprintf(body, sizeof(body), "{\"entity_id\":\"%.95s\"}", entity_id);
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    set_auth_header();
    esp_http_client_set_header(s_client, "Content-Type", "application/json");
    esp_http_client_set_method(s_client, HTTP_METHOD_POST);
    esp_http_client_set_url(s_client, url);
    esp_http_client_set_post_field(s_client, body, (int)strlen(body));
    ESP_LOGD(TAG, "POST body(%d): %s", (int)strlen(body), body);

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

static int http_post_service(const char *domain, const char *service, const char *entity_id)
{
    return http_post_service_ext(domain, service, entity_id, NULL);
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

/** 在 [start,end) 范围内找子串 needle。 */
static bool range_contains(const char *start, const char *end, const char *needle)
{
    if (start == NULL) {
        return false;
    }
    if (end == NULL) {
        end = start + strlen(start);
    }
    const size_t nlen = strlen(needle);
    for (const char *p = start; p + nlen <= end; p++) {
        if (memcmp(p, needle, nlen) == 0) {
            return true;
        }
    }
    return false;
}

/** 在 [start,end) 里找 "key": number，返回是否找到。 */
static bool json_int_in(const char *start, const char *end, const char *key, int *out)
{
    if (start == NULL) {
        return false;
    }
    if (end == NULL) {
        end = start + strlen(start);
    }
    char pattern[40];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *p = strstr(start, pattern);
    if (p == NULL || p >= end) {
        return false;
    }
    p += strlen(pattern);
    while (p < end && (*p == ' ' || *p == '\t')) {
        p++;
    }
    char *endp = NULL;
    const long v = strtol(p, &endp, 10);
    if (endp == p) {
        return false;
    }
    *out = (int)v;
    return true;
}

static bool json_str_in(const char *start, const char *end, const char *key, char *out, size_t out_len)
{
    if (start == NULL) {
        return false;
    }
    if (end == NULL) {
        end = start + strlen(start);
    }
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

    /* 灯的属性（没有就是 0 / false） */
    int v = 0;
    const bool is_light = (strncmp(e->entity_id, "light.", 6) == 0);
    const bool has_br_attr = json_int_in(obj_start, obj_end, "brightness", &v);
    if (has_br_attr && v > 0) {
        e->brightness = v;
        e->seen_brightness = true;
    } else if (is_light && !e->seen_brightness) {
        e->brightness = 255;      /* 从没拿到过真实值，先按满亮度显示 */
    }
    /* 灯关着时 HA 不返回 brightness：沿用上一次的值，别重置成 100% */
    e->has_brightness = has_br_attr || is_light;

    const bool has_ct_attr = json_int_in(obj_start, obj_end, "color_temp_kelvin", &v);
    if (has_ct_attr) {
        e->color_temp_k = v;
        e->seen_tint = true;
    }
    /* 灯关着时 color_temp_kelvin 也会缺失，但 supported_color_modes 能说明它支持色温 */
    e->has_tint = has_ct_attr ||
                  range_contains(obj_start, obj_end, "\"color_temp\"") ||
                  range_contains(obj_start, obj_end, "min_color_temp_kelvin");
    if (json_int_in(obj_start, obj_end, "min_color_temp_kelvin", &v)) {
        e->min_k = v;
    }
    if (json_int_in(obj_start, obj_end, "max_color_temp_kelvin", &v)) {
        e->max_k = v;
    }
    if (e->min_k == 0) {
        e->min_k = 2700;
    }
    if (e->max_k == 0) {
        e->max_k = 6500;
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

esp_err_t ha_client_action_attrs(const char *entity_id, const char *service, const char *attrs_json)
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
    const int status = http_post_service_ext(domain, service, entity_id, attrs_json);
    ESP_LOGI(TAG, "%s/%s %s %s -> %d", domain, service, entity_id,
             attrs_json ? attrs_json : "", status);
    return (status >= 200 && status < 300) ? ESP_OK : ESP_FAIL;
}

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

/* ---------------- 历史数据（折线图） ---------------- */

#define HIST_MAX_BYTES   (32 * 1024)

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} hist_ctx_t;

static esp_err_t hist_event(esp_http_client_event_t *ev)
{
    hist_ctx_t *ctx = (hist_ctx_t *)ev->user_data;
    if (ev->event_id == HTTP_EVENT_ON_DATA && ctx != NULL && ev->data_len > 0) {
        const size_t n = (size_t)ev->data_len;
        if (ctx->len + n < ctx->cap) {
            memcpy(ctx->buf + ctx->len, ev->data, n);
            ctx->len += n;
            ctx->buf[ctx->len] = '\0';
        }
    }
    return ESP_OK;
}

esp_err_t ha_client_history(const char *entity_id, int hours, float *values, int max_points,
                            int *out_count, float *out_min, float *out_max, float *out_avg)
{
    if (entity_id == NULL || values == NULL || max_points <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_base[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    char *buf = heap_caps_malloc(HIST_MAX_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    buf[0] = '\0';

    const time_t now = time(NULL);
    const time_t start = now - (time_t)hours * 3600;
    struct tm tm_start;
    gmtime_r(&start, &tm_start);
    char iso[40];
    snprintf(iso, sizeof(iso), "%04d-%02d-%02dT%02d:%02d:%02dZ",
             tm_start.tm_year + 1900, tm_start.tm_mon + 1, tm_start.tm_mday,
             tm_start.tm_hour, tm_start.tm_min, tm_start.tm_sec);

    char url[APP_HA_URL_MAX + 320];
    snprintf(url, sizeof(url),
             "%s/api/history/period/%s?filter_entity_id=%.95s&minimal_response&no_attributes"
             "&significant_changes_only",
             s_base, iso, entity_id);

    /* 复用常驻客户端（同一条 TLS 连接），只把响应写进 PSRAM 大缓冲：
       再开一条 TLS 会多占 16KB 内部 RAM，之前就是这里失败的 */
    if (client_create() != ESP_OK) {
        free(buf);
        return ESP_FAIL;
    }
    esp_err_t herr = ESP_FAIL;
    int hstatus = -1;
    for (int attempt = 0; attempt < 2; attempt++) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        set_auth_header();
        esp_http_client_set_method(s_client, HTTP_METHOD_GET);
        esp_http_client_set_url(s_client, url);
        s_sink = buf;
        s_sink_len = 0;
        s_sink_cap = HIST_MAX_BYTES - 1;
        buf[0] = '\0';
        herr = esp_http_client_perform(s_client);
        hstatus = esp_http_client_get_status_code(s_client);
        s_sink = NULL;
        if (herr != ESP_OK) {
            esp_http_client_close(s_client);   /* 长连接坏了：关掉重来 */
        }
        xSemaphoreGive(s_lock);
        if (herr == ESP_OK && hstatus == 200) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    if (herr != ESP_OK || hstatus != 200) {
        ESP_LOGW(TAG, "history %s -> %s, HTTP %d", entity_id, esp_err_to_name(herr), hstatus);
        if (herr != ESP_OK) {
            esp_http_client_close(s_client);
        }
        free(buf);
        return ESP_FAIL;
    }

    /* 不解析成对象树：直接在文本里扫 "state":"..."，避免 cJSON 把内部 RAM 撑爆 */
    float *raw = heap_caps_malloc(sizeof(float) * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (raw == NULL) {
        free(buf);
        return ESP_ERR_NO_MEM;
    }
    int raw_n = 0;
    const char *p = buf;
    while (raw_n < 1024) {
        p = strstr(p, "\"state\":\"");
        if (p == NULL) {
            break;
        }
        p += strlen("\"state\":\"");
        char *endp = NULL;
        const double v = strtod(p, &endp);
        const char *quote = strchr(p, '"');
        if (endp != p && quote != NULL && endp <= quote && isfinite(v) &&
            v > -1.0e6 && v < 1.0e6) {
            raw[raw_n++] = (float)v;      /* 只收数字型状态，unknown/on/off 自动跳过 */
        }
        p = quote ? quote : p;
    }

    int out_n = 0;
    float vmin = 0, vmax = 0, vsum = 0;
    if (raw_n > 0) {
        const int buckets = (raw_n < max_points) ? raw_n : max_points;
        for (int b = 0; b < buckets; b++) {
            const int from = (int)((long)b * raw_n / buckets);
            const int to = (int)((long)(b + 1) * raw_n / buckets);
            float acc = 0;
            int cnt = 0;
            for (int i = from; i < to; i++) {
                acc += raw[i];
                cnt++;
            }
            if (cnt == 0) {
                continue;
            }
            const float avg = acc / cnt;
            values[out_n++] = avg;
            vsum += avg;
            if (out_n == 1 || avg < vmin) vmin = avg;
            if (out_n == 1 || avg > vmax) vmax = avg;
        }
    }
    free(raw);
    free(buf);

    *out_count = out_n;
    if (out_n > 0) {
        *out_min = vmin;
        *out_max = vmax;
        *out_avg = vsum / out_n;
    }
    ESP_LOGI(TAG, "history %s: %d 点 (原始 %d)", entity_id, out_n, raw_n);
    return out_n > 0 ? ESP_OK : ESP_FAIL;
}

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
