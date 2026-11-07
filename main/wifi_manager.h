#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化 netif / event loop / Wi-Fi 驱动（APSTA 模式，幂等）。 */
esp_err_t wifi_manager_init(void);

/** 用 NVS 里的凭据连接；没有凭据时自动进入配网模式。 */
esp_err_t wifi_manager_start(void);

/** 阻塞等待拿到 IP。 */
bool wifi_manager_wait_connected(uint32_t timeout_ms);

bool wifi_manager_is_connected(void);

/** 启动配网热点 "JC3636-Setup" + 配置页面（192.168.4.1）。 */
esp_err_t wifi_manager_start_provisioning(void);

/** 当前 RSSI（dBm），未连接返回 0。 */
int wifi_manager_get_rssi(void);

/** 取当前 STA 的 IPv4 字符串，未连接时写入 "-"。 */
void wifi_manager_get_ip(char *buf, size_t len);

#ifdef __cplusplus
}
#endif
