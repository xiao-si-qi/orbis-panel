#pragma once

#include "layout.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 启动页：在拿到布局之前先显示"connecting..."。 */
void ui_home_boot(void);

/** 用布局重建界面（可在任意任务调用，内部切回 LVGL 上下文）。 */
void ui_home_apply(const layout_t *layout);

/** 打开子页面（目前是灯：亮度/色温）。 */
void ui_home_open_detail(const char *entity_id);

/** 关闭子页面（带滑出动画）。 */
void ui_home_close_detail(void);

/** 子页面是否打开（滑动翻页要避让）。 */
bool ui_home_detail_open(void);

#ifdef __cplusplus
}
#endif
