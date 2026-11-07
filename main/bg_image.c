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
} bg_cfg_t;

static bg_cfg_t s_cfg;
static volatile bool s_running = false;
static volatile int s_index = 0;

static bool s_initialized = false;
static uint8_t *s_frame = NULL;          /* RGB565 输出缓冲（按图片尺寸分配） */
static size_t s_frame_len = 0;
static lv_image_dsc_t s_dsc;
static lv_obj_t *s_img = NULL;
static lv_obj_t *s_dim = NULL;
static int s_shown_w = 0;
static int s_shown_h = 0;

/* ---------------------------- 图像下载/解码 ---------------------------- */

static uint8_t *http_fetch(const char *url, size_t *out_len)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return NULL;
    }

    uint8_t *buf = NULL;
    size_t len = 0;
    if (esp_http_client_open(client, 0) == ESP_OK) {
        esp_http_client_fetch_headers(client);
        if (esp_http_client_get_status_code(client) == 200) {
            buf = heap_caps_malloc(BG_JPEG_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (buf != NULL) {
                for (;;) {
                    const int n = esp_http_client_read(client, (char *)buf + len, BG_JPEG_MAX - len);
                    if (n <= 0) {
                        break;
                    }
                    len += (size_t)n;
                    if (len >= BG_JPEG_MAX) {
                        break;
                    }
                }
            }
        } else {
            ESP_LOGW(TAG, "%s -> HTTP %d", url, esp_http_client_get_status_code(client));
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (buf == NULL || len == 0) {
        free(buf);
        return NULL;
    }
    *out_len = len;
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
        .flags = {.swap_color_bytes = 1},   /* LVGL 用 RGB565 大端序 */
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
    s_dsc.data = s_frame;
    s_dsc.data_size = out.output_len;
    return true;
}

/* ------------------------------ LVGL 侧 ------------------------------ */

/** LVGL 线程：显示当前缓冲区并淡入。 */
static void show_cb(void *arg)
{
    (void)arg;
    if (s_img == NULL) {
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
    (void)arg;
    while (s_running) {
        if (s_cfg.count <= 0) {
            break;
        }
        if (s_index >= s_cfg.count) {
            s_index = 0;
        }
        const char *url = s_cfg.urls[s_index];

        size_t jpg_len = 0;
        uint8_t *jpg = http_fetch(url, &jpg_len);
        if (jpg != NULL) {
            if (decode_to_frame(jpg, jpg_len)) {
                lv_async_call(show_cb, NULL);
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

        for (int waited = 0; waited < s_cfg.interval_s * 10 && s_running; waited++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        s_index++;
    }
    s_running = false;
    vTaskDelete(NULL);
}

/* ------------------------------- 配置解析 ------------------------------- */

void bg_image_init(void)
{
    if (s_initialized) {
        return;
    }
    s_initialized = true;
    memset(&s_dsc, 0, sizeof(s_dsc));
    s_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
}

void bg_image_bind(lv_obj_t *parent)
{
    if (parent == NULL) {
        return;
    }

    s_img = lv_image_create(parent);
    lv_obj_set_size(s_img, BG_W, BG_H);
    lv_obj_set_pos(s_img, 0, 0);
    lv_obj_move_background(s_img);
    lv_obj_remove_flag(s_img, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_img, LV_OBJ_FLAG_HIDDEN);

    s_dim = lv_obj_create(parent);
    lv_obj_remove_style_all(s_dim);
    lv_obj_set_size(s_dim, BG_W, BG_H);
    lv_obj_set_pos(s_dim, 0, 0);
    lv_obj_set_style_bg_color(s_dim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_dim, s_cfg.dim_pct > 0 ? (lv_opa_t)(s_cfg.dim_pct * 255 / 100) : LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(s_dim, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_dim, s_cfg.dim_pct > 0 ? 0 : LV_OBJ_FLAG_HIDDEN);

    /* 背景层在最底，暗化层紧贴其上，卡片会自动排在后面 */
    lv_obj_move_background(s_img);
    if (s_dim != NULL) {
        lv_obj_move_foreground(s_img);
        lv_obj_move_background(s_dim);
    }
}

void bg_image_stop(void)
{
    s_running = false;
    s_cfg.count = 0;
    if (s_img != NULL) {
        lv_obj_add_flag(s_img, LV_OBJ_FLAG_HIDDEN);
    }
}

void bg_image_apply_json(const char *json)
{
    bg_image_init();
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

    s_running = false;
    vTaskDelay(pdMS_TO_TICKS(200));      /* 让旧任务退出 */

    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.interval_s = 60;
    s_cfg.dim_pct = 35;

    const cJSON *interval = cJSON_GetObjectItemCaseSensitive(bg, "interval");
    if (cJSON_IsNumber(interval) && interval->valueint >= 5) {
        s_cfg.interval_s = interval->valueint;
    }
    const cJSON *dim = cJSON_GetObjectItemCaseSensitive(bg, "dim");
    if (cJSON_IsNumber(dim)) {
        s_cfg.dim_pct = dim->valueint < 0 ? 0 : (dim->valueint > 80 ? 80 : dim->valueint);
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

    /* 暗化层不透明度要跟着配置走 */
    if (s_dim != NULL) {
        lv_obj_set_style_bg_opa(s_dim, s_cfg.dim_pct > 0 ? (lv_opa_t)(s_cfg.dim_pct * 255 / 100) : LV_OPA_TRANSP, 0);
        if (s_cfg.dim_pct > 0) {
            lv_obj_remove_flag(s_dim, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_dim, LV_OBJ_FLAG_HIDDEN);
        }
    }

    ESP_LOGI(TAG, "background: %d image(s), %ds, dim %d%%", s_cfg.count, s_cfg.interval_s, s_cfg.dim_pct);

    s_index = 0;
    s_running = true;
    if (xTaskCreate(bg_task, "bg", 8192, NULL, 3, NULL) != pdPASS) {
        s_running = false;
        ESP_LOGE(TAG, "failed to start background task");
    }
}
