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

/** 显示传感器折线图（由后台任务取完历史后调用，必须在 LVGL 线程）。 */
void ui_home_show_chart(const char *entity_id, const float *vals, int n,
                        float vmin, float vmax, float vavg);

/** 关闭折线图。 */
void ui_home_close_chart(void);

/** 折线图是否打开。 */
bool ui_home_chart_open(void);

#ifdef __cplusplus
}
#endif
