#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LAYOUT_MAX_PAGES      6
#define LAYOUT_TILES_PER_PAGE 4

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
    int page_count;
    layout_page_t pages[LAYOUT_MAX_PAGES];
} layout_t;

/** 从 `<ha_url>/local/jc3636/layout.json` 拉取布局。 */
esp_err_t layout_fetch(layout_t *out);

/** 用 ha_client 自动挑选的结果拼一个兜底布局（每页最多 4 个）。 */
void layout_fallback(layout_t *out);

/** 按 entity_id 的域推断 tile 类型。 */
tile_type_t layout_type_of(const char *entity_id);

#ifdef __cplusplus
}
#endif
