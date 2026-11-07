/**
 * @file log_buffer.h
 * @brief 最近日志的环形缓冲 + 错误摘要（远程诊断用）。
 *
 * 不把日志流推 MQTT（噪声大、会撑爆 HA 历史、可能带敏感信息）。只做两件事：
 *   1. 保留最近 N 行在 RAM 里，被 request 时才发一次（非 retained）；
 *   2. 出现 W/E 级日志时记一条摘要，由上报任务限流发到 /diag（retained）。
 */
#ifndef LOG_BUFFER_H
#define LOG_BUFFER_H

#include <stdbool.h>
#include <stddef.h>

/** @brief 安装日志钩子（启动时调一次）。 */
void log_buffer_init(void);

/** @brief 取最近若干行日志，用 '\n' 连接；返回写入字节数。 */
size_t log_buffer_dump(char *out, size_t len, int lines);

/** @brief 取"自上次取走以来"的第一条 W/E 摘要（JSON），有则返回 true。 */
bool log_buffer_take_diag(char *out, size_t len);

#endif /* LOG_BUFFER_H */
