/**
 * @file bg_image.c
 * @brief 背景图轮播实现：HTTP 下载 JPEG → esp_jpeg 解码成 RGB565 → LVGL 贴图。
 *
 * 线程约定：
 *   - 下载/解码在独立任务里做（会阻塞几百毫秒）；
 *   - 碰 LVGL 对象的动作通过 lv_async_call 切回 LVGL 线程。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "jpeg_decoder.h"

#include "app_settings.h"
#include "bg_cache.h"
#include "bg_image.h"

static const char *TAG = "bg";

#define BG_MAX_IMAGES   8
#define BG_URL_MAX      256
#define BG_W            360
#define BG_H            360
#define BG_JPEG_MAX     (512 * 1024)

typedef struct {
    char urls[BG_MAX_IMAGES][BG_URL_MAX];
    int count;
    int interval_s;
    int dim_pct;
    int card_pct;      /* 卡片黑色蒙版（0-95） */
} bg_cfg_t;

static bg_cfg_t s_cfg;
static volatile bool s_running = false;
static volatile int s_gen;          /* 配置代际：旧任务看到代际变了就退出 */
static volatile bool s_task_alive;
static volatile int s_index = 0;

static bool s_initialized = false;
static uint8_t *s_frame = NULL;          /* RGB565 输出缓冲（按图片尺寸分配） */
static size_t s_frame_len = 0;
static lv_image_dsc_t s_dsc;
static lv_obj_t *s_img = NULL;
static lv_obj_t *s_dim = NULL;
static volatile bool s_frame_ready = false;
static int s_shown_w = 0;
static int s_shown_h = 0;

/* ---------------------------- 图像下载/解码 ---------------------------- */

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t len;
    bool overflow;
} fetch_ctx_t;

static esp_err_t fetch_event(esp_http_client_event_t *ev)
{
    fetch_ctx_t *ctx = (fetch_ctx_t *)ev->user_data;
    if (ev->event_id == HTTP_EVENT_ON_DATA && ctx != NULL) {
        const size_t n = (size_t)ev->data_len;
        if (ctx->len + n > ctx->cap) {
            ctx->overflow = true;
            return ESP_OK;
        }
        memcpy(ctx->buf + ctx->len, ev->data, n);
        ctx->len += n;
    }
    return ESP_OK;
}

/** 下载到 PSRAM 缓冲（走 perform，TLS/重定向都交给 esp_http_client 处理）。 */
static uint8_t *http_fetch(const char *url, size_t *out_len)
{
    uint8_t *buf = heap_caps_malloc(BG_JPEG_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        ESP_LOGE(TAG, "no PSRAM for jpeg buffer");
        return NULL;
    }

    fetch_ctx_t ctx = {.buf = buf, .cap = BG_JPEG_MAX, .len = 0, .overflow = false};
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
        .event_handler = fetch_event,
        .user_data = &ctx,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        free(buf);
        return NULL;
    }

    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200 || ctx.len == 0 || ctx.overflow) {
        ESP_LOGW(TAG, "%s -> %s, HTTP %d, %d bytes%s", url, esp_err_to_name(err), status,
                 (int)ctx.len, ctx.overflow ? " (too large)" : "");
        free(buf);
        return NULL;
    }
    *out_len = ctx.len;
    return buf;
}

/** 解码到 s_frame（按图片实际尺寸/缩放分配缓冲）；成功返回 true。 */
static bool decode_to_frame(const uint8_t *jpg, size_t jpg_len)
{
    esp_jpeg_image_cfg_t info_cfg = {
        .indata = (uint8_t *)jpg,
        .indata_size = (uint32_t)jpg_len,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_0,
    };
    esp_jpeg_image_output_t info = {0};
    if (esp_jpeg_get_image_info(&info_cfg, &info) != ESP_OK || info.width == 0 || info.height == 0) {
        ESP_LOGW(TAG, "jpeg info failed");
        return false;
    }

    /* 大图让解码器做整数缩小，避免缓冲过大 / 内存碎片：
       目标是解码后宽度 >= 360（后面交给 LVGL 缩放到屏幕），但不超过 720。 */
    esp_jpeg_image_scale_t scale = JPEG_IMAGE_SCALE_0;
    int divisor = 1;
    if (info.width > 720 && info.width / 2 >= 360) {
        scale = JPEG_IMAGE_SCALE_1_2;
        divisor = 2;
    }
    if (info.width / divisor > 1080 && info.width / 4 >= 360) {
        scale = JPEG_IMAGE_SCALE_1_4;
        divisor = 4;
    }

    const size_t need = (size_t)(info.width / divisor) * (size_t)(info.height / divisor) * 2;
    if (need == 0) {
        return false;
    }
    if (s_frame_len < need) {
        uint8_t *buf = heap_caps_realloc(s_frame, need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (buf == NULL) {
            ESP_LOGE(TAG, "no PSRAM for %u bytes", (unsigned)need);
            return false;
        }
        s_frame = buf;
        s_frame_len = need;
    }

    esp_jpeg_image_cfg_t cfg = {
        .indata = (uint8_t *)jpg,
        .indata_size = (uint32_t)jpg_len,
        .outbuf = s_frame,
        .outbuf_size = (uint32_t)need,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = scale,
        .flags = {.swap_color_bytes = 0},   /* tjpgd 直接写 uint16_t，已是 LVGL 需要的小端 */
    };
    esp_jpeg_image_output_t out = {0};
    const esp_err_t err = esp_jpeg_decode(&cfg, &out);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "jpeg decode failed: %s (%dx%d, scale 1/%d)", esp_err_to_name(err),
                 info.width, info.height, divisor);
        return false;
    }
    s_shown_w = out.width;
    s_shown_h = out.height;
    s_frame_ready = true;
    s_dsc.data = s_frame;
    s_dsc.data_size = out.output_len;
    return true;
}

/* ------------------------------ LVGL 侧 ------------------------------ */

/** LVGL 线程：显示当前缓冲区并淡入。 */
/** 目标视觉状态：由任何线程设置，实际改 LVGL 对象必须在 LVGL 线程做。 */
static volatile bool s_want_dim_visible;
static volatile bool s_want_image_hidden;

/** 在 LVGL 线程里应用目标状态（lv_async_call 回调）。 */
static void apply_visual_cb(void *arg)
{
    (void)arg;
    if (s_dim != NULL) {
        lv_obj_set_style_bg_opa(s_dim,
                                s_want_dim_visible ? (lv_opa_t)(s_cfg.dim_pct * 255 / 100) : LV_OPA_TRANSP, 0);
        if (s_want_dim_visible) {
            lv_obj_remove_flag(s_dim, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_dim, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_img != NULL && s_want_image_hidden) {
        lv_obj_add_flag(s_img, LV_OBJ_FLAG_HIDDEN);
    }
}

/** 请求在 LVGL 线程里刷新视觉状态（可以从任意任务调用）。 */
static void request_visual_update(bool dim_visible, bool image_hidden)
{
    s_want_dim_visible = dim_visible;
    s_want_image_hidden = image_hidden;
    lv_lock();
    lv_async_call(apply_visual_cb, NULL);
    lv_unlock();
}

static void show_cb(void *arg)
{
    (void)arg;
    if (s_img == NULL) {
        /* UI 还没建好（或刚被重建）：留个标记，等 bg_image_bind 时补显示 */
        return;
    }
    s_dsc.header.w = s_shown_w;
    s_dsc.header.h = s_shown_h;
    lv_image_set_src(s_img, &s_dsc);

    /* 非 360×360 的图按比例缩放（面板会先裁好，这里只是兜底） */
    if (s_shown_w > 0 && s_shown_w != BG_W) {
        lv_image_set_scale(s_img, (uint32_t)(BG_W * 256 / s_shown_w));
    } else {
        lv_image_set_scale(s_img, 256);
    }

    lv_obj_remove_flag(s_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(s_img, LV_OPA_TRANSP, 0);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_img);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_time(&a, 500);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_style_opa);
    lv_anim_start(&a);
}

static void bg_task(void *arg)
{
    const int my_gen = (int)(intptr_t)arg;
    s_task_alive = true;

    /* 开机阶段 HA 实体拉取也在跑 TLS，两条握手同时开会把内部 RAM 顶爆
       （表现为 esp-tls 读错误 -0x7F00）。先等网络/首轮请求稳定下来。 */
    vTaskDelay(pdMS_TO_TICKS(4000));

    while (s_running && my_gen == s_gen) {
        if (s_cfg.count <= 0) {
            break;
        }
        if (s_index >= s_cfg.count) {
            s_index = 0;
        }
        const char *url = s_cfg.urls[s_index];

        size_t jpg_len = 0;
        uint8_t *jpg = NULL;
        if (bg_cache_get(s_index, url, &jpg, &jpg_len)) {
            ESP_LOGI(TAG, "background %d/%d 来自缓存", s_index + 1, s_cfg.count);
        } else {
            for (int attempt = 0; attempt < 3 && s_running; attempt++) {
                jpg = http_fetch(url, &jpg_len);
                if (jpg != NULL) {
                    bg_cache_put(s_index, url, jpg, jpg_len);
                    break;
                }
                ESP_LOGW(TAG, "下载失败，%d 秒后重试 (%d/3)", 3, attempt + 1);
                vTaskDelay(pdMS_TO_TICKS(3000));
            }
        }
        if (jpg != NULL) {
            if (decode_to_frame(jpg, jpg_len)) {
                lv_lock();
                lv_async_call(show_cb, NULL);
                lv_unlock();
                ESP_LOGI(TAG, "background %d/%d shown (%dx%d)", s_index + 1, s_cfg.count, s_shown_w, s_shown_h);
            }
            free(jpg);
        }

        /* 单张图不用轮播，停在原地等下一次配置 */
        if (s_cfg.count == 1) {
            while (s_running && s_cfg.count == 1) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            continue;
        }

        for (int waited = 0; waited < s_cfg.interval_s * 10 && s_running && my_gen == s_gen; waited++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        s_index++;
    }
    if (my_gen == s_gen) {
        s_running = false;
    }
    s_task_alive = false;
    vTaskDelete(NULL);
}

/* ------------------------------- 配置解析 ------------------------------- */

void bg_image_init(void)
{
    if (s_initialized) {
        return;
    }
    s_initialized = true;
    s_cfg.interval_s = 60;
    s_cfg.dim_pct = 35;
    s_cfg.card_pct = 60;
    memset(&s_dsc, 0, sizeof(s_dsc));
    s_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
}

void bg_image_bind(lv_obj_t *parent)
{
    if (parent == NULL) {
        return;
    }

    /* 层级（从底到顶）：图片 → 暗化层 → 卡片。
       顺序必须是"先建暗化层放到底，再建图片放到底"，
       这样图片落在索引 0、暗化层紧随其后。 */
    s_dim = lv_obj_create(parent);
    lv_obj_remove_style_all(s_dim);
    lv_obj_set_size(s_dim, BG_W, BG_H);
    lv_obj_set_pos(s_dim, 0, 0);
    lv_obj_set_style_bg_color(s_dim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_dim, s_cfg.dim_pct > 0 ? (lv_opa_t)(s_cfg.dim_pct * 255 / 100) : LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(s_dim, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_dim, s_cfg.dim_pct > 0 ? 0 : LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_background(s_dim);

    s_img = lv_image_create(parent);
    lv_obj_set_size(s_img, BG_W, BG_H);
    lv_obj_set_pos(s_img, 0, 0);
    lv_obj_remove_flag(s_img, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_background(s_img);       /* 图片沉到最底，暗化层在它上面 */

    s_want_dim_visible = s_cfg.dim_pct > 0;
    s_want_image_hidden = !s_frame_ready;
    apply_visual_cb(NULL);      /* bind 本身就在 LVGL 线程 */

    ESP_LOGI(TAG, "bind: dim %d%% (opa %d), image behind", s_cfg.dim_pct,
             (int)(s_cfg.dim_pct * 255 / 100));

    /* 如果下载早就完成了，绑定之后立刻补显示 */
    if (s_frame_ready) {
        show_cb(NULL);
    }
}

uint8_t bg_image_card_opa_pct(void)
{
    return (uint8_t)s_cfg.card_pct;
}

void bg_image_stop(void)
{
    s_running = false;
    for (int i = 0; i < s_cfg.count; i++) {
        bg_cache_drop(i);
    }
    s_cfg.count = 0;
    request_visual_update(false, true);   /* 隐藏图片也交给 LVGL 线程 */
}

void bg_image_apply_json(const char *json)
{
    bg_image_init();
    bg_cache_init();
    if (json == NULL) {
        bg_image_stop();
        return;
    }

    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        return;
    }
    const cJSON *bg = cJSON_GetObjectItemCaseSensitive(root, "bg");
    if (!cJSON_IsObject(bg)) {
        cJSON_Delete(root);
        bg_image_stop();
        return;
    }

    /* 关键：必须等旧任务真的退出，否则每次重连都会多出一个任务在跑 TLS */
    s_running = false;
    s_gen++;
    for (int i = 0; i < 30 && s_task_alive; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (s_task_alive) {
        ESP_LOGW(TAG, "上一个背景任务 3 秒内没退出，继续");
    }

    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.interval_s = 60;
    s_cfg.dim_pct = 35;
    s_cfg.card_pct = 60;

    const cJSON *interval = cJSON_GetObjectItemCaseSensitive(bg, "interval");
    if (cJSON_IsNumber(interval) && interval->valueint >= 5) {
        s_cfg.interval_s = interval->valueint;
    }
    const cJSON *dim = cJSON_GetObjectItemCaseSensitive(bg, "dim");
    if (cJSON_IsNumber(dim)) {
        s_cfg.dim_pct = dim->valueint < 0 ? 0 : (dim->valueint > 80 ? 80 : dim->valueint);
    }

    const cJSON *card = cJSON_GetObjectItemCaseSensitive(bg, "card");
    if (cJSON_IsNumber(card)) {
        s_cfg.card_pct = card->valueint < 0 ? 0 : (card->valueint > 95 ? 95 : card->valueint);
    }

    const cJSON *images = cJSON_GetObjectItemCaseSensitive(bg, "images");
    if (cJSON_IsArray(images)) {
        const cJSON *item = NULL;
        cJSON_ArrayForEach(item, images)
        {
            if (!cJSON_IsString(item) || s_cfg.count >= BG_MAX_IMAGES) {
                continue;
            }
            snprintf(s_cfg.urls[s_cfg.count], BG_URL_MAX, "%s", item->valuestring);
            s_cfg.count++;
        }
    }
    cJSON_Delete(root);

    if (s_cfg.count == 0) {
        bg_image_stop();
        return;
    }

    /* 注意：这里跑在 MQTT 任务里，**不能**直接改 LVGL 对象，必须切回 LVGL 线程 */
    request_visual_update(s_cfg.dim_pct > 0, false);

    for (int i = s_cfg.count; i < BG_MAX_IMAGES; i++) {
        bg_cache_drop(i);      /* 数量变少时清掉多余缓存 */
    }
    ESP_LOGI(TAG, "background: %d image(s), %ds, dim %d%%", s_cfg.count, s_cfg.interval_s, s_cfg.dim_pct);

    s_index = 0;
    s_running = true;
    ESP_LOGI(TAG, "starting bg task, internal heap %d KB (largest block %d KB)",
             (int)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (int)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
    if (xTaskCreate(bg_task, "bg", 10240, (void *)(intptr_t)s_gen, 3, NULL) != pdPASS) {
        s_running = false;
        ESP_LOGE(TAG, "failed to start background task");
    }
}
