/**
 * JSON 布局：从 Home Assistant 的 `/local/jc3636/layout.json` 拉取页面/卡片定义。
 *
 * 文件放到 HA 的 `config/www/jc3636/layout.json`，访问地址就是
 * `<ha_url>/local/jc3636/layout.json`（HA 的 /local/ 是公开静态目录，不需要令牌）。
 *
 * 格式：
 * {
 *   "title": "Home",
 *   "pages": [
 *     {
 *       "name": "Devices",
 *       "tiles": [
 *         {"entity": "light.xie_ju_deng", "label": "鞋柜灯", "type": "toggle"},
 *         {"entity": "sensor.water_temp", "label": "水温", "type": "sensor"}
 *       ]
 *     }
 *   ]
 * }
 *
 * type 可省略，按域名自动推断；label 省略时用 HA 的 friendly_name。
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "app_settings.h"
#include "ha_client.h"
#include "layout.h"

static const char *TAG = "layout";

#define LAYOUT_URL_SUFFIX "/local/jc3636/layout.json"
#define LAYOUT_BODY_MAX   (16 * 1024)

static char s_body[LAYOUT_BODY_MAX];
static int s_body_len;

static esp_err_t http_event(esp_http_client_event_t *ev)
{
    if (ev->event_id == HTTP_EVENT_ON_DATA && ev->data && ev->data_len > 0) {
        if (s_body_len + ev->data_len < LAYOUT_BODY_MAX) {
            memcpy(s_body + s_body_len, ev->data, ev->data_len);
            s_body_len += ev->data_len;
            s_body[s_body_len] = '\0';
        }
    }
    return ESP_OK;
}

tile_type_t layout_type_of(const char *entity_id)
{
    if (entity_id == NULL) {
        return TILE_TOGGLE;
    }
    if (strncmp(entity_id, "sensor.", 7) == 0 || strncmp(entity_id, "binary_sensor.", 14) == 0) {
        return TILE_SENSOR;
    }
    if (strncmp(entity_id, "script.", 7) == 0 || strncmp(entity_id, "scene.", 6) == 0 ||
        strncmp(entity_id, "automation.", 11) == 0) {
        return TILE_SCENE;
    }
    return TILE_TOGGLE;
}

static const char *json_string(const cJSON *obj, const char *key)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(it) ? it->valuestring : NULL;
}

static void copy_str(char *dst, size_t dst_len, const char *src)
{
    size_t n = strlen(src);
    if (n >= dst_len) {
        n = dst_len - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

esp_err_t layout_fetch(layout_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    copy_str(out->title, sizeof(out->title), "Home");

    const app_settings_t *cfg = app_settings_get();
    if (cfg->ha_url[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    char url[APP_HA_URL_MAX + 64];
    snprintf(url, sizeof(url), "%s%s", cfg->ha_url, LAYOUT_URL_SUFFIX);

    esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = 8000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048,
        .event_handler = http_event,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_body_len = 0;
    s_body[0] = '\0';
    esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200 || s_body_len == 0) {
        ESP_LOGW(TAG, "layout fetch failed (%s, HTTP %d, %d bytes)", esp_err_to_name(err), status, s_body_len);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "layout downloaded: %d bytes", s_body_len);

    cJSON *root = cJSON_Parse(s_body);
    if (root == NULL) {
        ESP_LOGE(TAG, "layout JSON parse failed");
        return ESP_ERR_INVALID_ARG;
    }

    const char *title = json_string(root, "title");
    if (title) {
        copy_str(out->title, sizeof(out->title), title);
    }

    const cJSON *pages = cJSON_GetObjectItemCaseSensitive(root, "pages");
    if (!cJSON_IsArray(pages)) {
        ESP_LOGE(TAG, "layout has no pages[]");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    const cJSON *page = NULL;
    cJSON_ArrayForEach(page, pages)
    {
        if (out->page_count >= LAYOUT_MAX_PAGES) {
            break;
        }
        layout_page_t *lp = &out->pages[out->page_count];
        const char *pname = json_string(page, "name");
        snprintf(lp->name, sizeof(lp->name), "%s", pname ? pname : "Page");

        const cJSON *tiles = cJSON_GetObjectItemCaseSensitive(page, "tiles");
        if (!cJSON_IsArray(tiles)) {
            continue;
        }
        const cJSON *tile = NULL;
        cJSON_ArrayForEach(tile, tiles)
        {
            if (lp->tile_count >= LAYOUT_TILES_PER_PAGE) {
                break;
            }
            const char *entity = json_string(tile, "entity");
            if (entity == NULL || entity[0] == '\0') {
                continue;
            }
            layout_tile_t *lt = &lp->tiles[lp->tile_count];
            copy_str(lt->entity, sizeof(lt->entity), entity);

            const char *label = json_string(tile, "label");
            if (label == NULL) {
                label = json_string(tile, "name");
            }
            if (label) {
                copy_str(lt->label, sizeof(lt->label), label);
            }

            const char *type = json_string(tile, "type");
            if (type == NULL) {
                lt->type = layout_type_of(entity);
            } else if (strcmp(type, "sensor") == 0) {
                lt->type = TILE_SENSOR;
            } else if (strcmp(type, "scene") == 0 || strcmp(type, "script") == 0) {
                lt->type = TILE_SCENE;
            } else {
                lt->type = TILE_TOGGLE;
            }
            lp->tile_count++;
        }
        if (lp->tile_count > 0) {
            out->page_count++;
        }
    }

    cJSON_Delete(root);

    if (out->page_count == 0) {
        ESP_LOGE(TAG, "layout has no usable page");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "layout: '%s', %d page(s)", out->title, out->page_count);
    return ESP_OK;
}

void layout_fallback(layout_t *out)
{
    memset(out, 0, sizeof(*out));
    copy_str(out->title, sizeof(out->title), "Home");

    const int total = ha_client_count();
    for (int i = 0; i < total; i++) {
        const ha_entity_t *e = ha_client_at(i);
        if (e == NULL) {
            continue;
        }
        if (out->page_count == 0 || out->pages[out->page_count - 1].tile_count >= LAYOUT_TILES_PER_PAGE) {
            if (out->page_count >= LAYOUT_MAX_PAGES) {
                break;
            }
            layout_page_t *np = &out->pages[out->page_count];
            const tile_type_t t = layout_type_of(e->entity_id);
            snprintf(np->name, sizeof(np->name), "%s",
                     t == TILE_SENSOR ? "Sensors" : (t == TILE_SCENE ? "Scenes" : "Devices"));
            out->page_count++;
        }
        layout_page_t *lp = &out->pages[out->page_count - 1];
        layout_tile_t *lt = &lp->tiles[lp->tile_count++];
        copy_str(lt->entity, sizeof(lt->entity), e->entity_id);
        copy_str(lt->label, sizeof(lt->label), e->name);
        lt->type = layout_type_of(e->entity_id);
    }
    ESP_LOGI(TAG, "fallback layout: %d page(s)", out->page_count);
}
