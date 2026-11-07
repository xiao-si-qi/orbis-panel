/**
 * @file bg_image.h
 * @brief 全屏背景图 + 轮播（JPEG 由设备自己解码）。
 *
 * 布局 JSON 里的可选字段：
 *
 *   "bg": {
 *     "images": ["https://.../1.jpg", "https://.../2.jpg"],
 *     "interval": 60,        // 每张停留秒数，默认 60
 *     "dim": 35              // 背景上叠一层黑色，0-80，默认 35；0 = 不叠
 *   }
 *
 * 图片由 switchyard 转好（建议 360×360），设备下载后解码成 RGB565 直接贴图，
 * 不需要在设备上做缩放/裁剪（尺寸不符时用 LVGL 的缩放兜底）。
 * 缓冲放 PSRAM，一帧 360×360×2 ≈ 259 KB。
 */
#ifndef BG_IMAGE_H
#define BG_IMAGE_H

#include "lvgl.h"

/** @brief 初始化（创建互斥量等），在 UI 起来之前调用一次。 */
void bg_image_init(void);

/**
 * @brief 把背景层挂到某个页面容器里（放在最底层）。
 *
 * 每次重建 UI 都会调用；内部会复用已解码的缓冲。
 */
void bg_image_bind(lv_obj_t *parent);

/**
 * @brief 应用布局 JSON 里的 bg 配置（传整个 layout 也行，内部只找 "bg"）。
 *
 * 传 NULL 或没有 bg 字段 → 清掉背景、停止轮播。
 */
void bg_image_apply_json(const char *json);

/** @brief 停止轮播并释放当前图像。 */
void bg_image_stop(void);

#endif /* BG_IMAGE_H */
