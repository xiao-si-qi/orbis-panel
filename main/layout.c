/**
 * JSON 布局：从 Home Assistant 的 `/local/orbis/layout.json` 拉取页面/卡片定义。
 *
 * 文件放到 HA 的 `config/www/orbis/layout.json`，访问地址就是
 * `<ha_url>/local/orbis/layout.json`（HA 的 /local/ 是公开静态目录，不需要令牌）。
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
#include <string.h>

#include "cJSON.h"
#include "nvs.h"
#include "esp_log.h"

#include "app_settings.h"
#include "layout.h"

static const char *TAG = "layout";



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

esp_err_t layout_parse(const char *json, layout_t *out)
{
    if (json == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    copy_str(out->title, sizeof(out->title), "Home");

    cJSON *root = cJSON_Parse(json);
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
    return (out->page_count > 0) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

#define NVS_LAYOUT_KEY "layout_json"
#define NVS_LAYOUT_MAX (12 * 1024)

esp_err_t layout_save_nvs(const char *json)
{
    if (json == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open("app", NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, NVS_LAYOUT_KEY, json);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t layout_get_nvs_json(char *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    if (nvs_open("app", NVS_READONLY, &h) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    size_t need = 0;
    esp_err_t err = nvs_get_str(h, NVS_LAYOUT_KEY, NULL, &need);
    if (err != ESP_OK || need == 0 || need > len) {
        nvs_close(h);
        return ESP_ERR_INVALID_SIZE;
    }
    err = nvs_get_str(h, NVS_LAYOUT_KEY, buf, &need);
    nvs_close(h);
    return err;
}

esp_err_t layout_load_nvs(layout_t *out)
{
    nvs_handle_t h;
    if (nvs_open("app", NVS_READONLY, &h) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    size_t len = 0;
    esp_err_t err = nvs_get_str(h, NVS_LAYOUT_KEY, NULL, &len);
    if (err != ESP_OK || len == 0 || len > NVS_LAYOUT_MAX) {
        nvs_close(h);
        return ESP_ERR_NOT_FOUND;
    }
    char *buf = malloc(len);
    if (buf == NULL) {
        nvs_close(h);
        return ESP_ERR_NO_MEM;
    }
    err = nvs_get_str(h, NVS_LAYOUT_KEY, buf, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        free(buf);
        return err;
    }
    err = layout_parse(buf, out);
    free(buf);
    return err;
}
