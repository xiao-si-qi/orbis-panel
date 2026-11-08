#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LAYOUT_MAX_PAGES      6
#define LAYOUT_TILES_PER_PAGE 12

typedef enum {
    TILE_TOGGLE = 0, /* 点一下 = toggle */
    TILE_SCENE,      /* 点一下 = turn_on */
    TILE_SENSOR,     /* 只读，显示数值 + 单位 */
} tile_type_t;

typedef struct {
    char entity[96];
    char label[48];
    tile_type_t type;
} layout_tile_t;

typedef struct {
    char name[24];
    int tile_count;
    layout_tile_t tiles[LAYOUT_TILES_PER_PAGE];
} layout_page_t;

typedef struct {
    char title[24];
    int rotation;      /* 屏幕方向 0/90/180/270，来自布局 JSON 的 rotation 字段 */
    int page_count;
    layout_page_t pages[LAYOUT_MAX_PAGES];
} layout_t;

/** 从 `<ha_url>/local/orbis/layout.json` 拉取布局。 */
esp_err_t layout_fetch(layout_t *out);

/** 解析一段布局 JSON（MQTT 下发 / NVS 里存的那份）。 */
esp_err_t layout_parse(const char *json, layout_t *out);

/** 把布局 JSON 存进 NVS（设备重启后先用它）。 */
esp_err_t layout_save_nvs(const char *json);

/** 把 NVS 里那份布局 JSON 原样拷出来（设备回读用）。 */
esp_err_t layout_get_nvs_json(char *buf, size_t len);

/** 从 NVS 读上一次下发的布局；没有则返回 ESP_ERR_NOT_FOUND。 */
esp_err_t layout_load_nvs(layout_t *out);

/** 按 entity_id 的域推断 tile 类型。 */
tile_type_t layout_type_of(const char *entity_id);

#ifdef __cplusplus
}
#endif
