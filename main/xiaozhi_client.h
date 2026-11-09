/**
 * @file xiaozhi_client.h
 * @brief 小智 AI 服务端接入（阶段③：OTA 发现 + WebSocket 握手，暂不传音频）。
 *
 * 流程（协议见 xiaozhi-esp32/docs/websocket.md）：
 *   1. POST <endpoint>（OTA/发现接口）→ 拿到 {"websocket":{"url":"wss://..."}}
 *   2. 连 WebSocket，带 Authorization / Protocol-Version / Device-Id / Client-Id
 *   3. 发 hello（transport=websocket, audio_params=opus/16000/60ms）
 *   4. 收服务器 hello（含 session_id）→ 标记已连接
 */
#ifndef XIAOZHI_CLIENT_H
#define XIAOZHI_CLIENT_H

#include <stdbool.h>

/** 按布局配置启动/停止。endpoint 为空或 enabled=false 时停止。 */
void xiaozhi_client_configure(bool enabled, const char *endpoint);

/** 给语音页显示的一行状态文字（永远非 NULL）。 */
const char *xiaozhi_client_status_text(void);

/** 是否已连上服务端（hello 完成）。 */
bool xiaozhi_client_ready(void);

/** 服务端类型：官方 / 自建（用于页面显示）。 */
const char *xiaozhi_client_server_note(void);

#endif /* XIAOZHI_CLIENT_H */
