/**
 * @file bg_cache.h
 * @brief 背景图缓存（写在 flash 的 assets FAT 分区，不依赖 TF 卡）。
 *
 * 缓存策略：按"URL 列表里的索引"存两份文件
 *   /assets/bg_<i>.jpg   JPEG 原始字节
 *   /assets/bg_<i>.url   该索引对应的 URL（用于判断缓存是否还有效）
 *
 * 开机时若 URL 未变，直接读缓存解码，不再走网络；换图后 URL 变，自动重新下载。
 */
#ifndef BG_CACHE_H
#define BG_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief 挂载 assets 分区（失败不影响功能，只是没有缓存）。 */
void bg_cache_init(void);

/** @brief 缓存是否可用（分区挂载成功）。 */
bool bg_cache_ready(void);

/**
 * @brief 读取缓存。
 *
 * @param index 图片索引（0 起）
 * @param url   期望的 URL；与缓存记录不一致则视为未命中
 * @param out   成功时返回 malloc 出来的缓冲（调用方 free）
 * @param len   返回长度
 */
bool bg_cache_get(int index, const char *url, uint8_t **out, size_t *len);

/** @brief 写入/更新缓存。失败只打日志，不影响显示。 */
void bg_cache_put(int index, const char *url, const uint8_t *data, size_t len);

/** @brief 删除索引 i 的缓存（换图、清空时用）。 */
void bg_cache_drop(int index);

#endif /* BG_CACHE_H */
