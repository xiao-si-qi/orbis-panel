#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 当前运行的固件版本（PROJECT_VER）。 */
const char *ota_current_version(void);

/** 后台任务：拉 firmware.json → 版本不同则下载并烧写 → 重启。重复调用安全。 */
void ota_start_check(void);

/** 直接用给定 URL 升级（MQTT cmd:ota 用）。version 可为 NULL。 */
void ota_start_url(const char *url, const char *version);

bool ota_busy(void);
int ota_percent(void);          /* 0..100，未知时 -1 */
const char *ota_status_text(void);

#ifdef __cplusplus
}
#endif
