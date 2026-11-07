#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HA_MAX_ENTITIES 24

typedef struct {
    char entity_id[96];
    char name[48];
    char state[24];
    char unit[12];
    bool on;
    bool valid;
    bool fresh;
} ha_entity_t;

/** 读取 settings 里的 HA 地址/令牌，创建 HTTP 客户端。 */
esp_err_t ha_client_init(void);

/** 加入关注列表（去重）。label 为 NULL 时用 friendly_name / object_id 兜底。 */
esp_err_t ha_client_register(const char *entity_id, const char *label);

/** 没有 JSON 布局时的兜底：自动挑 灯/开关 → 场景 → 传感器，最多 4+4+4 个。 */
esp_err_t ha_client_autofill(void);

/** 一次拉全量 /api/states，把关注列表里的名字/状态填好（启动时用，最快）。 */
esp_err_t ha_client_fetch_all(void);

int ha_client_count(void);
const ha_entity_t *ha_client_at(int idx);
const ha_entity_t *ha_client_find(const char *entity_id);

/** 单独刷新一个实体。 */
esp_err_t ha_client_refresh_at(int idx);

/** 执行动作：service 传 "toggle" / "turn_on" / "turn_off"。 */
esp_err_t ha_client_action(const char *entity_id, const char *service);

/** 启动后台轮询。 */
void ha_client_start_poll(void);

#ifdef __cplusplus
}
#endif
