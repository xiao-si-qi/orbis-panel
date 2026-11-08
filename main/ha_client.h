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
    /* 灯类实体的属性（其它实体会是 0） */
    int brightness;        /* 0-255 */
    int color_temp_k;      /* 当前色温（开尔文） */
    int min_k, max_k;      /* 该实体支持的范围 */
    bool has_brightness;
    bool has_tint;         /* 支持色温调节 */
} ha_entity_t;

/** 读取 settings 里的 HA 地址/令牌，创建 HTTP 客户端。 */
esp_err_t ha_client_init(void);

/** 加入关注列表（去重）。label 为 NULL 时用 friendly_name / object_id 兜底。 */
esp_err_t ha_client_register(const char *entity_id, const char *label);

/** 一次拉全量 /api/states，把关注列表里的名字/状态填好（启动时用，最快）。 */
esp_err_t ha_client_fetch_all(void);

int ha_client_count(void);
const ha_entity_t *ha_client_at(int idx);
const ha_entity_t *ha_client_find(const char *entity_id);

/** 单独刷新一个实体。 */
esp_err_t ha_client_refresh_at(int idx);

/** 执行动作：service 传 "toggle" / "turn_on" / "turn_off"。 */
esp_err_t ha_client_action(const char *entity_id, const char *service);

/** 带参数的动作用：attrs_json 是额外字段（不含 entity_id），例如 {"brightness_pct":60}。 */
esp_err_t ha_client_action_attrs(const char *entity_id, const char *service, const char *attrs_json);

/**
 * @brief 拉某个实体最近 hours 小时的历史，降采样到 max_points 个点。
 *
 * @param values     输出数组（float，长度至少 max_points）
 * @param out_count  实际点数
 * @param out_min    最小值
 * @param out_max    最大值
 * @param out_avg    平均值
 */
esp_err_t ha_client_history(const char *entity_id, int hours, float *values, int max_points,
                            int *out_count, float *out_min, float *out_max, float *out_avg);

/** HA REST 是否可达（最近一次请求成功）。界面上的 HA 圆点用它。 */
bool ha_client_online(void);

/** 启动后台轮询。 */
void ha_client_start_poll(void);

#ifdef __cplusplus
}
#endif
