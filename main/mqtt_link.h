#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 设备 id（MQTT 主题里的 <id>），来自配置，默认 "hall-screen"。 */
const char *mqtt_link_device_id(void);

/** 启动 MQTT（未配置 broker 时直接返回 ESP_ERR_INVALID_STATE）。 */
esp_err_t mqtt_link_start(void);

bool mqtt_link_connected(void);

/** 立刻上报一次 state（连接后也会自动上报，之后每 30s 一次）。 */
void mqtt_link_publish_state(void);

/** 上报能力声明（retained，switchyard/<id>/meta）。 */
void mqtt_link_publish_meta(void);

/** 上报一个事件，payload 是 JSON 对象字符串。 */
void mqtt_link_publish_event(const char *json);

#ifdef __cplusplus
}
#endif
