/**
 * 家庭控制屏界面：JSON 布局驱动 + 一个固件内置的「System」页。
 *
 * 页 0..N-1 来自 HA 的 /local/orbis/layout.json（每页最多 4 个 tile）；
 * 最后一页是固件自己画的系统页：固件版本 / Wi-Fi / IP / 运行时长 / 内存，
 * 以及「检查更新」按钮（拉 HA 上的 firmware.json，有新版本就下载并重启）。
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "lvgl.h"

#include "app_settings.h"
#include "bsp_board.h"
#include "bsp_display.h"
#include "ha_client.h"
#include "layout.h"
#include "lv_font_noto_18_cjk.h"
#include "mqtt_link.h"
#include "ota.h"
#include "bg_image.h"
#include "ui_home.h"
#include "wifi_manager.h"

static const char *TAG = "ui";

#define MAX_PAGES (LAYOUT_MAX_PAGES + 1) /* JSON 页 + System 页 */

#define CARD_W    140
#define CARD_H    90
#define CARD_GAP  12
#define GRID_TOP  84
#define GRID_LEFT 34

#define COLOR_BG        lv_color_hex(0x000000)
#define COLOR_CARD      lv_color_hex(0x1C1C1E)
#define COLOR_CARD_ON   lv_color_hex(0x1D4ED8)
#define COLOR_BORDER    lv_color_hex(0x2C2C2E)
#define COLOR_TEXT      lv_color_hex(0xF2F2F7)
#define COLOR_MUTED     lv_color_hex(0x8E8E93)
#define COLOR_OK        lv_color_hex(0x30D158)
#define COLOR_BAD       lv_color_hex(0xFF453A)
#define COLOR_ACCENT    lv_color_hex(0x2563EB)

typedef struct {
    lv_obj_t *card;
    lv_obj_t *label;
    lv_obj_t *value;
    lv_obj_t *dot;
    char entity[96];
    tile_type_t type;
} tile_widget_t;

static layout_t s_layout;
static bool s_layout_valid;
static int s_json_pages;

static tile_widget_t s_tiles[LAYOUT_MAX_PAGES][LAYOUT_TILES_PER_PAGE];
static int s_page_tiles[MAX_PAGES];

static lv_obj_t *s_pages[MAX_PAGES];
static lv_obj_t *s_dots[MAX_PAGES];
static int s_page_count;

static lv_obj_t *s_title;
static lv_obj_t *s_wifi_icon;
static lv_obj_t *s_wifi_label;
static lv_color_t s_wifi_color;
static int s_wifi_level;      /* 0..4 格 */
static lv_obj_t *s_ha_dot;
static lv_obj_t *s_hint;
static lv_timer_t *s_refresh_timer;

/* System 页的控件 */
static lv_obj_t *s_sys_lines[5];
static lv_obj_t *s_ota_btn;
static lv_obj_t *s_ota_btn_label;
static lv_obj_t *s_ota_bar;
static lv_obj_t *s_ota_status;

static int s_page;
static bool s_press_on_slider;   /* 本次按压是否落在滑条上 */

/* ---- 灯 / 传感器 的子页面 ---- */
static lv_obj_t *s_detail;                       /* 全屏子页容器 */
static lv_obj_t *s_detail_title;
static lv_obj_t *s_back_label;
static lv_obj_t *s_br_lab;
static lv_obj_t *s_ct_lab;
static lv_obj_t *s_detail_power_sw;
static lv_obj_t *s_detail_br_row, *s_detail_br_slider, *s_detail_br_value;
static lv_obj_t *s_detail_ct_row, *s_detail_ct_slider, *s_detail_ct_value;
static bool s_detail_visible;

/* ---- 传感器折线图页 ---- */
static lv_obj_t *s_chart_page;
static lv_obj_t *s_chart_title;
static lv_obj_t *s_chart_span;
static lv_obj_t *s_chart;
static float s_chart_pts[32];
static int s_chart_pts_n;
static float s_chart_lo, s_chart_hi;
static lv_obj_t *s_chart_min;
static lv_obj_t *s_chart_max;
static lv_obj_t *s_chart_avg;
static lv_obj_t *s_chart_back_label;
static bool s_chart_visible;

/* ---- 空调页 ---- */
static lv_obj_t *s_clim_page;
static lv_obj_t *s_clim_title;
static lv_obj_t *s_clim_temp;     /* 当前/目标大号文字 */
static lv_obj_t *s_clim_span;     /* 当前模式说明 */
static lv_obj_t *s_clim_slider;
static lv_obj_t *s_clim_modes_row;
static lv_obj_t *s_clim_fans_row;
static bool s_clim_visible;
static char s_clim_entity[96];
static volatile bool s_chart_wanted;
static volatile int s_chart_gen;
static char s_chart_entity[96];
/* 最近一次成功的折线图数据：60 秒内同一实体直接复用，避免每次点击都走网络 */
static char s_chart_cache_entity[96];
static float s_chart_cache_vals[32];
static int s_chart_cache_n;
static float s_chart_cache_min, s_chart_cache_max, s_chart_cache_avg;
static uint32_t s_chart_cache_at;
static char s_detail_entity[96];
/* 用户刚拖过滑条时，短时间内以本地值为准（HA 的轮询会慢半拍，灯关着时甚至没有 brightness） */
static bool s_sw_override_on;
static uint32_t s_sw_override_until;
static int s_br_override;
static uint32_t s_br_override_until;
static int s_ct_override;
static uint32_t s_ct_override_until;
#define OVERRIDE_MS 6000

/* 子页函数的前置声明（refresh_timer_cb 里会用到） */
static void detail_apply_entity(void);
static void detail_apply_foreground(bool dark_text);
static uint32_t s_last_gesture_ms;   /* 最近一次手势时间，用来吃掉误触 */
static QueueHandle_t s_cmd_q;

typedef struct {
    char entity[96];
    char service[24];
    char attrs[64];      /* 非空则带参数调用（亮度/色温/空调） */
    int gen;             /* 折线图请求代际：过期结果直接丢掉 */
} cmd_t;

/* ------------------------------------------------------------------------- */
/* 翻页                                                                       */
/* ------------------------------------------------------------------------- */

/* 标题用哪套字体：含非 ASCII（中文）就用内置 CJK 字体，纯英文用 Montserrat */
static const lv_font_t *title_font_for(const char *text)
{
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (*p >= 0x80) {
            return &lv_font_noto_18_cjk;
        }
    }
    return &lv_font_montserrat_24;
}

static bool is_status_page(int page)
{
    return s_layout_valid && page == s_json_pages;
}

static const char *page_title(int page)
{
    if (is_status_page(page)) {
        return "System";
    }
    return s_layout.pages[page].name;
}

/* ---------------- 翻页动画 ---------------- */

#define PAGE_ANIM_MS 220

static bool s_animating;

static void anim_x_cb(void *var, int32_t v)
{
    lv_obj_set_x((lv_obj_t *)var, (lv_coord_t)v);
}

/* 位置固定、只动渲染偏移：不会被布局刷新覆盖，滑动效果也更平滑 */
static void anim_translate_cb(void *var, int32_t v)
{
    lv_obj_set_style_translate_x((lv_obj_t *)var, (lv_coord_t)v, 0);
}

static void anim_done_cb(lv_anim_t *a)
{
    lv_obj_t *out = (lv_obj_t *)a->user_data;
    if (out != NULL && a->var != out) {
        lv_obj_add_flag(out, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_x(out, 0);
    }
    s_animating = false;
}

/** 直接把所有页的可见性/位置/圆点/标题刷成"只有 s_page 可见"。构建首帧用。 */
static void pages_apply_visibility(void)
{
    for (int i = 0; i < s_page_count; i++) {
        lv_obj_set_x(s_pages[i], 0);
        if (i == s_page) {
            lv_obj_remove_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
        }
        if (s_dots[i]) {
            lv_obj_set_style_bg_color(s_dots[i], (i == s_page) ? COLOR_TEXT : lv_color_hex(0x3A3A3C), 0);
        }
    }
    if (s_title) {
        const char *title = page_title(s_page);
        lv_label_set_text(s_title, title);
        lv_obj_set_style_text_font(s_title, title_font_for(title), 0);
        lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 18);
    }
    int visible = 0;
    for (int i = 0; i < s_page_count; i++) {
        if (!lv_obj_has_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN)) {
            visible++;
        }
    }
    ESP_LOGI(TAG, "showing page %d/%d (visible=%d)", s_page, s_page_count, visible);
}

/** 切换页面：dir > 0 表示"下一页"（新页从右侧滑入），< 0 反之。 */
static void show_page_dir(int page, int dir)
{
    if (s_page_count <= 0) {
        return;
    }
    if (page < 0) {
        page = s_page_count - 1;
    }
    page %= s_page_count;

    const int prev = s_page;
    if (page == prev) {
        return;
    }
    if (s_animating) {
        return;   /* 动画中忽略，避免页面叠在一起 */
    }
    s_page = page;

    /* 圆点 + 标题立刻更新，不跟着动画走 */
    for (int i = 0; i < s_page_count; i++) {
        if (s_dots[i]) {
            lv_obj_set_style_bg_color(s_dots[i], (i == page) ? COLOR_TEXT : lv_color_hex(0x3A3A3C), 0);
        }
    }
    if (s_title) {
        const char *title = page_title(page);
        lv_label_set_text(s_title, title);
        lv_obj_set_style_text_font(s_title, title_font_for(title), 0);
        lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 18);
    }

    lv_obj_t *in = s_pages[page];
    lv_obj_t *out = s_pages[prev];
    const lv_coord_t dist = (dir >= 0) ? 360 : -360;

    /* 新页先放到屏幕外，再滑到 0 */
    lv_obj_remove_flag(in, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_x(in, dist);

    s_animating = true;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, in);
    lv_anim_set_values(&a, dist, 0);
    lv_anim_set_time(&a, PAGE_ANIM_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, anim_x_cb);
    lv_anim_start(&a);

    if (out != NULL && out != in) {
        lv_anim_t b;
        lv_anim_init(&b);
        lv_anim_set_var(&b, out);
        lv_anim_set_values(&b, 0, -dist);
        lv_anim_set_time(&b, PAGE_ANIM_MS);
        lv_anim_set_path_cb(&b, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&b, anim_x_cb);
        lv_anim_set_completed_cb(&b, anim_done_cb);
        lv_anim_set_user_data(&b, out);
        lv_anim_start(&b);
    } else {
        s_animating = false;
    }
}

static void show_page(int page)
{
    /* 没有方向信息时按索引大小推断（点圆点翻页走这里） */
    const int dir = (page >= s_page) ? 1 : -1;
    show_page_dir(page, dir);
}

static void dot_clicked_cb(lv_event_t *e)
{
    show_page((int)(intptr_t)lv_event_get_user_data(e));
}

/* 让手势从任意子对象冒泡到页面/屏幕（否则滑在卡片上就没反应） */
static void ui_enable_gesture_bubble(lv_obj_t *obj)
{
    lv_obj_add_flag(obj, LV_OBJ_FLAG_GESTURE_BUBBLE | LV_OBJ_FLAG_EVENT_BUBBLE);
    const uint32_t n = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < n; i++) {
        ui_enable_gesture_bubble(lv_obj_get_child(obj, i));
    }
}

/* 原生手势在触摸驱动上不稳定，这里直接按"按下 → 抬起"的位移判断 */
static void swipe_cb(lv_event_t *e)
{
    static lv_point_t start;
    static bool pressed;

    const lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) {
        return;
    }

    lv_point_t p;
    lv_indev_get_point(indev, &p);

    if (code == LV_EVENT_PRESSED) {
        start = p;
        pressed = true;
        /* 记下起手点是否落在滑条上（拖滑条不能被当成滑动） */
        lv_obj_t *target = lv_event_get_target(e);
        s_press_on_slider = (target != NULL && lv_obj_check_type(target, &lv_slider_class));
        return;
    }
    if (code != LV_EVENT_RELEASED || !pressed) {
        return;
    }
    pressed = false;
    if (s_press_on_slider) {
        s_press_on_slider = false;
        return;                 /* 这一下是在滑条上，不算翻页/返回 */
    }

    const int dx = p.x - start.x;
    const int dy = p.y - start.y;
    ESP_LOGD(TAG, "swipe dx=%d dy=%d", dx, dy);

    if (abs(dx) < 40 || abs(dx) < abs(dy)) {
        return;
    }
    if (ui_home_climate_open()) {
        if (dx > 40 && start.x < 70) {
            ui_home_close_climate();
        }
        return;
    }
    if (ui_home_chart_open()) {
        if (dx > 40 && start.x < 70) {
            ui_home_close_chart();
        }
        return;
    }
    if (ui_home_detail_open()) {
        /* 只有从屏幕左边缘起手的右滑才当"返回"，否则拖滑条会被误判 */
        if (dx > 40 && start.x < 70) {
            ui_home_close_detail();
        }
        return;
    }
    const uint32_t now = lv_tick_get();
    if (now - s_last_gesture_ms < 400) {
        return;
    }
    s_last_gesture_ms = now;
    ESP_LOGI(TAG, "swipe dx=%d -> page %d", dx, (dx < 0 ? s_page + 1 : s_page - 1));
    if (dx < 0) {
        show_page_dir(s_page + 1, 1);
    } else {
        show_page_dir(s_page - 1, -1);
    }
}

static void gesture_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) {
        return;
    }
    /* 去抖：一次滑动可能冒泡出多个 GESTURE，避免连翻两页 */
    const uint32_t now = lv_tick_get();
    if (now - s_last_gesture_ms < 400) {
        return;
    }
    s_last_gesture_ms = now;
    switch (lv_indev_get_gesture_dir(indev)) {
    case LV_DIR_LEFT:
        show_page_dir(s_page + 1, 1);
        break;
    case LV_DIR_RIGHT:
        show_page_dir(s_page - 1, -1);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------------- */
/* tile                                                                       */
/* ------------------------------------------------------------------------- */

static void tile_clicked_cb(lv_event_t *e)
{
    tile_widget_t *w = (tile_widget_t *)lv_event_get_user_data(e);
    if (w == NULL || s_cmd_q == NULL) {
        return;
    }
    if (lv_tick_get() - s_last_gesture_ms < 400) {
        return;   /* 刚滑动过，忽略这次"点击" */
    }
    ESP_LOGI(TAG, "tile 点击: %s type=%d (detail=%p)", w->entity, (int)w->type, (void *)s_detail);

    /* 传感器：点开折线图（历史数据由后台任务拉取） */
    if (w->type == TILE_SENSOR) {
        /* 已经在看同一张图：不重复请求 */
        if (s_chart_visible && strcmp(s_chart_entity, w->entity) == 0) {
            return;
        }
        /* 缓存命中：直接画，不发请求 */
        if (strcmp(s_chart_cache_entity, w->entity) == 0 && s_chart_cache_n > 0 &&
            (lv_tick_get() - s_chart_cache_at) < 60000) {
            ui_home_show_chart(w->entity, s_chart_cache_vals, s_chart_cache_n,
                               s_chart_cache_min, s_chart_cache_max, s_chart_cache_avg);
            ESP_LOGI(TAG, "折线图命中缓存: %s", w->entity);
            return;
        }

        cmd_t cmd;
        memset(&cmd, 0, sizeof(cmd));
        snprintf(cmd.entity, sizeof(cmd.entity), "%s", w->entity);
        snprintf(cmd.service, sizeof(cmd.service), "chart");
        s_chart_gen++;
        cmd.gen = s_chart_gen;
        s_chart_wanted = true;
        bg_image_set_paused(true);      /* 让出 TLS/内存给历史请求 */
        xQueueSend(s_cmd_q, &cmd, 0);
        ESP_LOGI(TAG, "请求折线图: %s (gen %d)", w->entity, cmd.gen);
        if (s_chart_page != NULL && !s_chart_visible) {
            snprintf(s_chart_entity, sizeof(s_chart_entity), "%s", w->entity);
            const ha_entity_t *ent = ha_client_find(w->entity);
            lv_label_set_text(s_chart_title, ent ? ent->name : w->entity);
            lv_label_set_text(s_chart_span, "加载中…");
            lv_label_set_text(s_chart_min, "");
            lv_label_set_text(s_chart_avg, "");
            lv_label_set_text(s_chart_max, "");
            s_chart_pts_n = 0;               /* 清掉上一张图的线 */
            lv_obj_invalidate(s_chart);
            s_chart_visible = true;
            lv_obj_set_pos(s_chart_page, 0, 0);
            lv_obj_set_style_translate_x(s_chart_page, 360, 0);
            lv_obj_remove_flag(s_chart_page, LV_OBJ_FLAG_HIDDEN);

            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, s_chart_page);
            lv_anim_set_values(&a, 360, 0);
            lv_anim_set_time(&a, 220);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
            lv_anim_set_exec_cb(&a, anim_translate_cb);
            lv_anim_start(&a);
        }
        return;
    }

    /* 空调：点开专属页（温度/模式/风速） */
    if (strncmp(w->entity, "climate.", 8) == 0) {
        ui_home_open_climate(w->entity);
        return;
    }

    /* 灯：点开详情页（亮度/色温）；其它实体保持直接动作 */
    if (strncmp(w->entity, "light.", 6) == 0) {
        ui_home_open_detail(w->entity);
        return;
    }

    cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    snprintf(cmd.entity, sizeof(cmd.entity), "%s", w->entity);
    snprintf(cmd.service, sizeof(cmd.service), "%s", (w->type == TILE_SCENE) ? "turn_on" : "toggle");
    xQueueSend(s_cmd_q, &cmd, 0);
}

static void make_tile(lv_obj_t *parent, int page, int slot, int col, int row)
{
    const layout_tile_t *lt = &s_layout.pages[page].tiles[slot];
    tile_widget_t *w = &s_tiles[page][slot];

    memset(w, 0, sizeof(*w));
    snprintf(w->entity, sizeof(w->entity), "%s", lt->entity);
    w->type = lt->type;

    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, CARD_W, CARD_H);
    /* 坐标相对滚动容器：左右各留 22px，行距 = 卡片高 + 间隙 */
    lv_obj_set_pos(card, 22 + col * (CARD_W + CARD_GAP), row * (CARD_H + CARD_GAP));
    /* 卡片是黑色半透明：叠在"背景图 + 暗化层"之上会更暗，形成玻璃质感 */
    lv_obj_set_style_bg_color(card, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(card, (lv_opa_t)(bg_image_card_opa_pct() * 255 / 100), 0);
    lv_obj_set_style_radius(card, 22, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, COLOR_BORDER, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, tile_clicked_cb, LV_EVENT_CLICKED, w);

    lv_obj_t *label = lv_label_create(card);
    lv_label_set_text(label, lt->label[0] ? lt->label : lt->entity);
    lv_obj_set_style_text_font(label, &lv_font_noto_18_cjk, 0);
    lv_obj_set_style_text_color(label, COLOR_TEXT, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_size(label, CARD_W - 24, 42); /* 最多两行，超出打点 */
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *value = lv_label_create(card);
    lv_label_set_text(value, "--");
    lv_obj_set_style_text_font(value, &lv_font_montserrat_20, 0);
    if (lt->type == TILE_SENSOR) {
        lv_obj_set_style_text_color(value, COLOR_TEXT, 0);
        lv_obj_align(value, LV_ALIGN_LEFT_MID, 0, 8);
        lv_obj_set_width(value, CARD_W - 24);
    } else {
        lv_obj_set_style_text_color(value, COLOR_MUTED, 0);
        lv_obj_align(value, LV_ALIGN_BOTTOM_LEFT, 0, 0);

        lv_obj_t *dot = lv_obj_create(card);
        lv_obj_set_size(dot, 10, 10);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, COLOR_BAD, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_align(dot, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
        w->dot = dot;
    }

    w->card = card;
    w->label = label;
    w->value = value;
}

/** WiFi 图标：三道弧 + 一个点，点亮的弧数量表示信号格数（1~3 格）。 */
static void wifi_icon_draw_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    if (layer == NULL) {
        return;
    }
    lv_area_t a;
    lv_obj_get_coords(obj, &a);

    const int radii[3] = {9, 6, 3};      /* 外 -> 内 */
    const int cx = (a.x1 + a.x2) / 2;
    const int cy = a.y2 - 2;

    for (int i = 0; i < 3; i++) {
        /* 外弧对应第 1 格：level>=1 时点亮最外圈，依此类推 */
        const int need = 3 - i;          /* 外弧需要 3 格中的最高档 */
        const bool lit = (s_wifi_level >= need) && (s_wifi_level > 0);

        lv_draw_arc_dsc_t d;
        lv_draw_arc_dsc_init(&d);
        d.color = lit ? s_wifi_color : lv_color_hex(0x3A3A3C);
        d.width = 2;
        d.rounded = 1;
        d.start_angle = 205;
        d.end_angle = 335;
        d.center.x = cx;
        d.center.y = cy;
        d.radius = radii[i];
        lv_draw_arc(layer, &d);
    }

    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.bg_color = s_wifi_level > 0 ? s_wifi_color : lv_color_hex(0x3A3A3C);
    rd.bg_opa = LV_OPA_COVER;
    rd.radius = 1;
    lv_area_t dot = {cx - 1, cy - 1, cx + 1, cy + 1};
    lv_draw_rect(layer, &rd, &dot);
}

static lv_obj_t *make_status_dot(lv_obj_t *parent, int x, int y)
{
    lv_obj_t *dot = lv_obj_create(parent);
    lv_obj_set_size(dot, 8, 8);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, COLOR_BAD, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_set_pos(dot, x, y);
    return dot;
}

/* ------------------------------------------------------------------------- */
/* System 页                                                                  */
/* ------------------------------------------------------------------------- */

static void ota_button_cb(lv_event_t *e)
{
    (void)e;
    if (!ota_busy()) {
        ota_start_check();
    }
}

static void build_status_page(lv_obj_t *scr)
{
    lv_obj_t *page = lv_obj_create(scr);
    lv_obj_set_size(page, 360, 360);
    lv_obj_set_pos(page, 0, 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);

    /* System 页的参数放在一张卡片里：与其他卡片同样的黑色半透明，
       叠在背景图 + 暗化层之上，比背景更暗一些。 */
    lv_obj_t *info_card = lv_obj_create(page);
    lv_obj_remove_style_all(info_card);
    lv_obj_set_pos(info_card, 32, 88);
    lv_obj_set_size(info_card, 296, 132);
    lv_obj_set_style_bg_color(info_card, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(info_card, (lv_opa_t)(bg_image_card_opa_pct() * 255 / 100), 0);
    lv_obj_set_style_radius(info_card, 22, 0);
    lv_obj_set_style_border_width(info_card, 1, 0);
    lv_obj_set_style_border_color(info_card, COLOR_BORDER, 0);
    lv_obj_remove_flag(info_card, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < 5; i++) {
        s_sys_lines[i] = lv_label_create(page);
        lv_label_set_text(s_sys_lines[i], "--");
        lv_obj_set_style_text_font(s_sys_lines[i], &lv_font_noto_18_cjk, 0);
        lv_obj_set_style_text_color(s_sys_lines[i], COLOR_MUTED, 0);
        lv_obj_set_pos(s_sys_lines[i], 48, 102 + i * 24);
    }

    s_ota_btn = lv_button_create(page);
    lv_obj_set_size(s_ota_btn, 200, 44);
    lv_obj_align(s_ota_btn, LV_ALIGN_TOP_MID, 0, 228);
    lv_obj_set_style_bg_color(s_ota_btn, COLOR_ACCENT, 0);
    lv_obj_set_style_radius(s_ota_btn, 14, 0);
    lv_obj_add_event_cb(s_ota_btn, ota_button_cb, LV_EVENT_CLICKED, NULL);

    s_ota_btn_label = lv_label_create(s_ota_btn);
    lv_label_set_text(s_ota_btn_label, "检查更新");
    lv_obj_set_style_text_font(s_ota_btn_label, &lv_font_noto_18_cjk, 0);
    lv_obj_set_style_text_color(s_ota_btn_label, COLOR_TEXT, 0);
    lv_obj_center(s_ota_btn_label);

    s_ota_bar = lv_bar_create(page);
    lv_obj_set_size(s_ota_bar, 220, 10);
    lv_obj_align(s_ota_bar, LV_ALIGN_TOP_MID, 0, 286);
    lv_bar_set_range(s_ota_bar, 0, 100);
    lv_obj_add_flag(s_ota_bar, LV_OBJ_FLAG_HIDDEN);

    s_ota_status = lv_label_create(page);
    lv_label_set_text(s_ota_status, "");
    lv_obj_set_style_text_font(s_ota_status, &lv_font_noto_18_cjk, 0);
    lv_obj_set_style_text_color(s_ota_status, COLOR_MUTED, 0);
    lv_obj_set_width(s_ota_status, 300);
    lv_obj_set_style_text_align(s_ota_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_ota_status, LV_ALIGN_TOP_MID, 0, 302);

    s_pages[s_json_pages] = page;
}

static void refresh_status_page(void)
{
    if (s_sys_lines[0] == NULL) {
        return;
    }
    char buf[64];

    snprintf(buf, sizeof(buf), "固件  v%s", ota_current_version());
    lv_label_set_text(s_sys_lines[0], buf);

    snprintf(buf, sizeof(buf), "MQTT  %s", mqtt_link_connected() ? "已连接" : "未连接");
    lv_label_set_text(s_sys_lines[1], buf);

    char ip[24] = "-";
    wifi_manager_get_ip(ip, sizeof(ip));
    snprintf(buf, sizeof(buf), "WiFi  %d dBm · %s", wifi_manager_get_rssi(), ip);
    lv_label_set_text(s_sys_lines[2], buf);

    const int64_t up_s = esp_timer_get_time() / 1000000;
    snprintf(buf, sizeof(buf), "运行  %dh%02dm", (int)(up_s / 3600), (int)((up_s % 3600) / 60));
    lv_label_set_text(s_sys_lines[3], buf);

    snprintf(buf, sizeof(buf), "内存  %d KB", (int)(esp_get_free_heap_size() / 1024));
    lv_label_set_text(s_sys_lines[4], buf);

    if (ota_busy()) {
        lv_obj_remove_flag(s_ota_bar, LV_OBJ_FLAG_HIDDEN);
        const int p = ota_percent();
        lv_bar_set_value(s_ota_bar, p < 0 ? 0 : p, LV_ANIM_OFF);
        lv_label_set_text(s_ota_btn_label, p < 0 ? "检查中" : "升级中");
        const char *ota_text = ota_status_text();
    lv_label_set_text(s_ota_status, ota_text);
    if (ota_text[0] != '\0') {
        lv_obj_remove_flag(s_ota_status, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_ota_status, LV_OBJ_FLAG_HIDDEN);
    }
    } else {
        lv_obj_add_flag(s_ota_bar, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_ota_btn_label, "检查更新");
        const char *ota_text = ota_status_text();
    lv_label_set_text(s_ota_status, ota_text);
    if (ota_text[0] != '\0') {
        lv_obj_remove_flag(s_ota_status, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_ota_status, LV_OBJ_FLAG_HIDDEN);
    }
    }
}

/* ------------------------------------------------------------------------- */
/* 刷新                                                                       */
/* ------------------------------------------------------------------------- */

/** 状态文本可能含中文（天气"晴"、模式名等）：非 ASCII 就用 CJK 字体的较小字号。 */
static const lv_font_t *value_font_for(const char *text)
{
    if (text == NULL) {
        return &lv_font_noto_18_cjk;
    }
    /* 统一用同一个字体（这版 CJK 字库自带 ASCII 数字/字母），
       这样"晴"和"56.0 W"的字号完全一致，不会一大一小 */
    (void)text;
    return &lv_font_noto_18_cjk;
}

static void refresh_values(void)
{
    for (int p = 0; p < s_json_pages; p++) {
        for (int i = 0; i < s_page_tiles[p]; i++) {
            tile_widget_t *w = &s_tiles[p][i];
            const ha_entity_t *e = ha_client_find(w->entity);
            if (w->value == NULL) {
                continue;
            }
            if (e == NULL) {
                lv_label_set_text(w->value, "--");
                continue;
            }
            if (w->type == TILE_SENSOR) {
                if (!e->fresh || strcmp(e->state, "unavailable") == 0) {
                    lv_label_set_text(w->value, "--");
                } else if (e->unit[0]) {
                    char buf[64];
                    snprintf(buf, sizeof(buf), "%s %s", e->state, e->unit);
                    lv_label_set_text(w->value, buf);
                    lv_obj_set_style_text_font(w->value, value_font_for(buf), 0);
                } else {
                    lv_label_set_text(w->value, e->state);
                    lv_obj_set_style_text_font(w->value, value_font_for(e->state), 0);
                }
                continue;
            }

            const char *txt;
            if (!e->fresh || strcmp(e->state, "unavailable") == 0 || strcmp(e->state, "unknown") == 0) {
                txt = "--";
            } else if (strcmp(e->state, "on") == 0) {
                txt = "ON";
            } else if (strcmp(e->state, "off") == 0) {
                txt = "OFF";
            } else {
                txt = e->state;
            }
            lv_label_set_text(w->value, txt);
            lv_obj_set_style_text_font(w->value, value_font_for(txt), 0);
            lv_obj_set_style_bg_color(w->card, e->on ? COLOR_CARD_ON : COLOR_CARD, 0);
            lv_obj_set_style_text_color(w->value, e->on ? COLOR_TEXT : COLOR_MUTED, 0);
            if (w->dot) {
                lv_obj_set_style_bg_color(w->dot, e->fresh ? COLOR_OK : COLOR_BAD, 0);
            }
        }
    }
}

static void refresh_timer_cb(lv_timer_t *t)
{
    if (s_detail_visible) {
        detail_apply_entity();     /* 子页数值跟着轮询刷新 */
    }
    (void)t;

    {
        if (s_wifi_icon != NULL) {
        const int rssi = wifi_manager_get_rssi();
        if (!wifi_manager_is_connected()) {
            s_wifi_color = COLOR_BAD;                 /* 未连接：红 */
        } else if (rssi >= -60) {
            s_wifi_color = COLOR_OK;                  /* 强：绿 */
        } else if (rssi >= -78) {
            s_wifi_color = lv_color_hex(0xE5B95B);    /* 中：黄 */
        } else {
            s_wifi_color = lv_color_hex(0xFF9F0A);     /* 弱：橙 */
        }
        /* 信号格数：-55 以上 4 格，往下每档减一格 */
        if (!wifi_manager_is_connected()) {
            s_wifi_level = 0;            /* 断开：全暗 + 红 */
        } else if (rssi >= -60) {
            s_wifi_level = 3;            /* 三道光弧全亮 */
        } else if (rssi >= -73) {
            s_wifi_level = 2;
        } else {
            s_wifi_level = 1;
        }
        lv_obj_invalidate(s_wifi_icon);
        if (s_wifi_label != NULL) {
            lv_label_set_text(s_wifi_label, "");     /* 只留格数，不显示 dBm */
        }
    }
    }

    if (s_ha_dot) {
        lv_obj_set_style_bg_color(s_ha_dot, ha_client_online() ? COLOR_OK : COLOR_BAD, 0);
    }

    refresh_values();
    refresh_status_page();

    if (s_hint) {
        if (s_layout_valid) {
            lv_obj_add_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_label_set_text(s_hint, wifi_manager_is_connected() ? "loading layout..." : "connecting Wi-Fi...");
            lv_obj_remove_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* 构建                                                                       */
/* ------------------------------------------------------------------------- */

static void worker_task(void *arg)
{
    (void)arg;
    while (true) {
        cmd_t cmd;
        if (xQueueReceive(s_cmd_q, &cmd, portMAX_DELAY) == pdTRUE) {
            if (strcmp(cmd.service, "chart") == 0) {
                static float vals[32];
                int n = 0;
                float vmin = 0, vmax = 0, vavg = 0;
                esp_err_t he = ha_client_history(cmd.entity, 6, vals, 32, &n, &vmin, &vmax, &vavg);
                bg_image_set_paused(false);
                if (he == ESP_OK && n > 0) {
                    lv_lock();
                    if (cmd.gen == s_chart_gen && s_chart_wanted) {
                        ui_home_show_chart(cmd.entity, vals, n, vmin, vmax, vavg);
                    } else {
                        ESP_LOGI(TAG, "折线图结果已过期，丢弃 (%d != %d)", cmd.gen, s_chart_gen);
                    }
                    lv_unlock();
                } else {
                    ESP_LOGW(TAG, "折线图无数据: %s (%s)", cmd.entity, esp_err_to_name(he));
                    lv_lock();
                    if (cmd.gen == s_chart_gen) {
                        const ha_entity_t *ent = ha_client_find(cmd.entity);
                        lv_label_set_text(s_chart_title, ent ? ent->name : cmd.entity);
                        lv_label_set_text(s_chart_span, "取不到历史数据，稍后再试");
                        lv_label_set_text(s_chart_min, "");
                        lv_label_set_text(s_chart_avg, "");
                        lv_label_set_text(s_chart_max, "");
                        s_chart_pts_n = 0;
                        lv_obj_invalidate(s_chart);
                        /* 页面保持打开（返回手势照常），但允许下次重新请求 */
                        s_chart_wanted = false;
                        s_chart_visible = true;
                        lv_obj_set_pos(s_chart_page, 0, 0);
                        lv_obj_set_style_translate_x(s_chart_page, 0, 0);
                        lv_obj_remove_flag(s_chart_page, LV_OBJ_FLAG_HIDDEN);
                    }
                    lv_unlock();
                }
                continue;
            }

            ESP_LOGI(TAG, "%s %s %s", cmd.service, cmd.entity, cmd.attrs);
            if (cmd.attrs[0] != '\0') {
                ha_client_action_attrs(cmd.entity, cmd.service, cmd.attrs);
            } else {
                ha_client_action(cmd.entity, cmd.service);
            }
            vTaskDelay(pdMS_TO_TICKS(300));
            const ha_entity_t *e = ha_client_find(cmd.entity);
            if (e) {
                ha_client_refresh_at((int)(e - ha_client_at(0)));
            }
        }
    }
}

/* ---------------- 子页面（灯的详情） ---------------- */

static lv_obj_t *s_power_ring;
static lv_obj_t *s_power_bar;

/** 用两个基本图形画电源符号：缺口圆环 + 顶部竖线。颜色随开关状态。 */
static void power_icon_draw(lv_obj_t *parent)
{
    s_power_ring = lv_obj_create(parent);
    lv_obj_remove_style_all(s_power_ring);
    lv_obj_set_size(s_power_ring, 22, 22);
    lv_obj_set_style_radius(s_power_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_power_ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_power_ring, 3, 0);
    lv_obj_set_style_border_color(s_power_ring, lv_color_white(), 0);
    lv_obj_center(s_power_ring);
    lv_obj_set_y(s_power_ring, 4);
    lv_obj_remove_flag(s_power_ring, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_power_bar = lv_obj_create(parent);
    lv_obj_remove_style_all(s_power_bar);
    lv_obj_set_size(s_power_bar, 3, 10);
    lv_obj_set_style_bg_opa(s_power_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_power_bar, lv_color_white(), 0);
    lv_obj_align(s_power_bar, LV_ALIGN_CENTER, 0, -7);
    lv_obj_remove_flag(s_power_bar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
}

/** 开关状态 -> 图标颜色（开：白底上黑图标；关：黑底上白图标）。 */
static void power_icon_color(bool on)
{
    const lv_color_t c = on ? lv_color_black() : lv_color_white();
    if (s_power_ring) {
        lv_obj_set_style_border_color(s_power_ring, c, 0);
    }
    if (s_power_bar) {
        lv_obj_set_style_bg_color(s_power_bar, c, 0);
    }
}

/** 开尔文转近似色（用于页面背景直观体现色温）。 */
static lv_color_t kelvin_to_color(int kelvin)
{
    /* 2700K 偏橙、4000K 暖白、6500K 偏冷白，简单线性插值 */
    if (kelvin < 3000) {
        return lv_color_make(255, 170, 90);
    } else if (kelvin < 4500) {
        const int t = (kelvin - 3000) * 100 / 1500;    /* 0..100 */
        return lv_color_make(255, 170 + (245 - 170) * t / 100, 90 + (215 - 90) * t / 100);
    } else {
        const int t = (kelvin - 4500) * 100 / 2000;    /* 0..100 */
        return lv_color_make(255 - (255 - 210) * t / 100, 245, 215 + (255 - 215) * t / 100);
    }
}

/** 根据背景亮度切换文字颜色，保证可读。 */
static void detail_apply_foreground(bool dark_text)
{
    const lv_color_t fg = dark_text ? lv_color_black() : lv_color_white();
    const lv_color_t muted = dark_text ? lv_color_hex(0x3A3A3C) : COLOR_MUTED;
    if (s_detail_title)    lv_obj_set_style_text_color(s_detail_title, fg, 0);
    if (s_back_label)      lv_obj_set_style_text_color(s_back_label, fg, 0);
    if (s_br_lab)          lv_obj_set_style_text_color(s_br_lab, muted, 0);
    if (s_ct_lab)          lv_obj_set_style_text_color(s_ct_lab, muted, 0);
    if (s_detail_br_value) lv_obj_set_style_text_color(s_detail_br_value, fg, 0);
    if (s_detail_ct_value) lv_obj_set_style_text_color(s_detail_ct_value, fg, 0);
    /* 返回键底色跟着反色，避免在纯白背景上看不见 */
    if (s_back_label && lv_obj_get_parent(s_back_label)) {
        lv_obj_set_style_bg_color(lv_obj_get_parent(s_back_label),
                                  dark_text ? lv_color_hex(0xE5E5EA) : lv_color_hex(0x2C2C2E), 0);
        lv_obj_set_style_bg_opa(lv_obj_get_parent(s_back_label), LV_OPA_70, 0);
    }
}

/** 背景氛围：按"关=黑、开=灯色 × 亮度"更新子页底色。 */
static void detail_update_ambience(void)
{
    if (s_detail == NULL) {
        return;
    }
    const ha_entity_t *e = ha_client_find(s_detail_entity);
    if (e == NULL || !e->on) {
        lv_obj_set_style_bg_color(s_detail, COLOR_BG, 0);      /* 关灯：纯黑 */
        lv_obj_set_style_bg_opa(s_detail, LV_OPA_COVER, 0);
        detail_apply_foreground(false);
        return;
    }
    int k = (s_ct_override_until > lv_tick_get()) ? s_ct_override
                                                  : (e->color_temp_k ? e->color_temp_k : 4000);
    int pct = (s_br_override_until > lv_tick_get()) ? s_br_override
                                                    : (e->brightness * 100 / 255);
    if (pct <= 0) {
        pct = 100;
    }
    const lv_color_t c = kelvin_to_color(k);

    /* 整个页面的底色就是这只灯：色温决定颜色，亮度决定明暗。
       视觉下限抬到 50%：滑条最小时页面大致相当于现在 50% 的亮度，最高才是纯白。 */
    int pct_eff = 50 + (pct / 2);
    if (pct_eff > 100) {
        pct_eff = 100;
    }
    const lv_color_t bg = lv_color_make(
        (uint8_t)(c.red * pct_eff / 100),
        (uint8_t)(c.green * pct_eff / 100),
        (uint8_t)(c.blue * pct_eff / 100));
    lv_obj_set_style_bg_color(s_detail, bg, 0);
    lv_obj_set_style_bg_opa(s_detail, LV_OPA_COVER, 0);

    /* 背景亮就用黑字，背景暗就用白字 */
    const int lum = (299 * bg.red + 587 * bg.green + 114 * bg.blue) / 1000;
    detail_apply_foreground(lum < 150);      /* true = 用深色文字 */
}

static void detail_anim_done_cb(lv_anim_t *a);
static void detail_close_done_cb(lv_anim_t *a);

static void detail_apply_entity(void)
{
    const ha_entity_t *e = ha_client_find(s_detail_entity);
    if (e == NULL) {
        return;
    }
    ESP_LOGD(TAG, "light attrs: br=%d tint=%d k=%d range=%d-%d state=%s",
             e->has_brightness, e->has_tint, e->color_temp_k, e->min_k, e->max_k, e->state);
    lv_label_set_text(s_detail_title, e->name);

    /* 不再显示"已开/已关"文字：开=白色底、关=黑色底，一眼可辨 */
    bool on = e->on;
    if (s_sw_override_until > lv_tick_get()) {
        on = s_sw_override_on;
        if (e->on == on) {
            s_sw_override_until = 0;          /* HA 已跟上，交还控制权 */
        }
    } else {
        s_sw_override_until = 0;
    }
    if (s_detail_power_sw != NULL) {
        if (on) {
            lv_obj_add_state(s_detail_power_sw, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(s_detail_power_sw, LV_STATE_CHECKED);
        }
        lv_obj_set_style_bg_color(s_detail_power_sw,
                                  on ? lv_color_white() : lv_color_hex(0x151517), LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_detail_power_sw,
                                  on ? lv_color_black() : lv_color_white(), LV_PART_KNOB);
    }
    detail_update_ambience();

    if (s_detail_br_row) {
        if (e->has_brightness) {
            lv_obj_remove_flag(s_detail_br_row, LV_OBJ_FLAG_HIDDEN);
            int pct = e->brightness * 100 / 255;
            if (s_br_override_until > lv_tick_get()) {
                pct = s_br_override;          /* 用户刚调过：先按本地值显示 */
            } else {
                s_br_override_until = 0;
            }
            lv_slider_set_value(s_detail_br_slider, pct, LV_ANIM_OFF);
            char buf[16];
            snprintf(buf, sizeof(buf), "%d%%", pct);
            lv_label_set_text(s_detail_br_value, buf);
        } else {
            lv_obj_add_flag(s_detail_br_row, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_detail_ct_row) {
        if (e->has_tint) {
            lv_obj_remove_flag(s_detail_ct_row, LV_OBJ_FLAG_HIDDEN);
            lv_slider_set_range(s_detail_ct_slider, e->min_k, e->max_k);
            int k = e->color_temp_k ? e->color_temp_k : (e->min_k + e->max_k) / 2;
            if (s_ct_override_until > lv_tick_get()) {
                k = s_ct_override;
            } else {
                s_ct_override_until = 0;
            }
            lv_slider_set_value(s_detail_ct_slider, k, LV_ANIM_OFF);
            char buf[24];
            snprintf(buf, sizeof(buf), "%dK", k);
            lv_label_set_text(s_detail_ct_value, buf);
        } else {
            lv_obj_add_flag(s_detail_ct_row, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void detail_slider_changed_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    const int v = (int)lv_slider_get_value(slider);
    char buf[24];
    if (slider == s_detail_br_slider) {
        snprintf(buf, sizeof(buf), "%d%%", v);
        lv_label_set_text(s_detail_br_value, buf);
    } else {
        snprintf(buf, sizeof(buf), "%dK", v);
        lv_label_set_text(s_detail_ct_value, buf);
    }

    if (slider == s_detail_br_slider) {
        s_br_override = v;
        s_br_override_until = lv_tick_get() + OVERRIDE_MS;
    } else {
        s_ct_override = v;
        s_ct_override_until = lv_tick_get() + OVERRIDE_MS;
    }
    detail_update_ambience();   /* 拖动时底色实时跟着变 */

    if (lv_event_get_code(e) != LV_EVENT_RELEASED && lv_event_get_code(e) != LV_EVENT_PRESS_LOST) {
        return;   /* 拖动过程中只更新显示，松手才发命令 */
    }
    ESP_LOGI(TAG, "滑条松手: %s value=%d", (slider == s_detail_br_slider) ? "亮度" : "色温", v);
    if (s_detail_entity[0] == '\0' || s_cmd_q == NULL) {
        return;
    }
    cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    snprintf(cmd.entity, sizeof(cmd.entity), "%s", s_detail_entity);
    snprintf(cmd.service, sizeof(cmd.service), "turn_on");
    if (slider == s_detail_br_slider) {
        s_br_override = v;
        s_br_override_until = lv_tick_get() + OVERRIDE_MS;
        snprintf(cmd.attrs, sizeof(cmd.attrs), "{\"brightness_pct\":%d}", v);
        /* 调亮度会把灯点亮，但按钮交给轮询去更新，避免拖动时乱跳 */
    } else {
        s_ct_override = v;
        s_ct_override_until = lv_tick_get() + OVERRIDE_MS;
        snprintf(cmd.attrs, sizeof(cmd.attrs), "{\"color_temp_kelvin\":%d}", v);
    }
    xQueueSend(s_cmd_q, &cmd, 0);
}

static void detail_power_changed_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    const bool want_on = lv_obj_has_state(sw, LV_STATE_CHECKED);

    /* 开关自身的视觉已经切换：记住它，别让轮询立刻拉回去 */
    s_sw_override_on = want_on;
    s_sw_override_until = lv_tick_get() + OVERRIDE_MS;

    if (s_detail_entity[0] == '\0' || s_cmd_q == NULL) {
        return;
    }
    cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    snprintf(cmd.entity, sizeof(cmd.entity), "%s", s_detail_entity);
    snprintf(cmd.service, sizeof(cmd.service), "%s", want_on ? "turn_on" : "turn_off");
    xQueueSend(s_cmd_q, &cmd, 0);
}

static void detail_back_cb(lv_event_t *e)
{
    (void)e;
    ui_home_close_detail();
}

static void build_detail_view(lv_obj_t *scr)
{
    s_detail = lv_obj_create(scr);
    lv_obj_remove_style_all(s_detail);
    lv_obj_set_size(s_detail, 360, 360);
    lv_obj_set_pos(s_detail, 0, 0);
    lv_obj_set_style_bg_color(s_detail, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_detail, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_detail, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_detail, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *back = lv_button_create(s_detail);
    lv_obj_set_size(back, 36, 36);
    lv_obj_align(back, LV_ALIGN_TOP_MID, 0, 10);   /* 顶部中间：圆屏这里才看得见 */
    lv_obj_set_style_bg_opa(back, LV_OPA_20, 0);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_event_cb(back, detail_back_cb, LV_EVENT_CLICKED, NULL);
    s_back_label = lv_label_create(back);
    lv_label_set_text(s_back_label, LV_SYMBOL_LEFT);
    lv_obj_center(s_back_label);

    s_detail_title = lv_label_create(s_detail);
    lv_label_set_text(s_detail_title, "--");
    lv_obj_set_style_text_font(s_detail_title, &lv_font_noto_18_cjk, 0);
    lv_obj_set_style_text_color(s_detail_title, COLOR_TEXT, 0);
    lv_obj_align(s_detail_title, LV_ALIGN_TOP_MID, 0, 58);

    /* 开关：lv_switch（开=白色轨道+黑滑块，关=深色轨道+白滑块） */
    s_detail_power_sw = lv_switch_create(s_detail);
    lv_obj_set_size(s_detail_power_sw, 96, 46);
    lv_obj_align(s_detail_power_sw, LV_ALIGN_TOP_MID, 0, 92);
    lv_obj_set_style_radius(s_detail_power_sw, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_detail_power_sw, lv_color_hex(0x151517), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_detail_power_sw, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_detail_power_sw, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_detail_power_sw, lv_color_hex(0x3A3A3C), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_detail_power_sw, lv_color_white(), LV_PART_KNOB);
    lv_obj_add_event_cb(s_detail_power_sw, detail_power_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* 亮度 */
    s_detail_br_row = lv_obj_create(s_detail);
    lv_obj_remove_style_all(s_detail_br_row);
    lv_obj_set_size(s_detail_br_row, 300, 70);
    lv_obj_align(s_detail_br_row, LV_ALIGN_TOP_MID, 0, 152);
    s_br_lab = lv_label_create(s_detail_br_row);
    lv_label_set_text(s_br_lab, "亮度");
    lv_obj_set_style_text_font(s_br_lab, &lv_font_noto_18_cjk, 0);
    lv_obj_set_style_text_color(s_br_lab, COLOR_MUTED, 0);

    s_detail_br_value = lv_label_create(s_detail_br_row);
    lv_label_set_text(s_detail_br_value, "--");
    lv_obj_set_style_text_font(s_detail_br_value, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_detail_br_value, COLOR_TEXT, 0);
    lv_obj_align(s_detail_br_value, LV_ALIGN_TOP_RIGHT, 0, 0);

    s_detail_br_slider = lv_slider_create(s_detail_br_row);
    lv_obj_set_width(s_detail_br_slider, 300);
    lv_obj_set_height(s_detail_br_slider, 12);
    lv_slider_set_range(s_detail_br_slider, 1, 100);
    lv_obj_align(s_detail_br_slider, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_add_event_cb(s_detail_br_slider, detail_slider_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_detail_br_slider, detail_slider_changed_cb, LV_EVENT_RELEASED, NULL);

    /* 色温 */
    s_detail_ct_row = lv_obj_create(s_detail);
    lv_obj_remove_style_all(s_detail_ct_row);
    lv_obj_set_size(s_detail_ct_row, 208, 62);
    lv_obj_align(s_detail_ct_row, LV_ALIGN_TOP_MID, 0, 226);
    s_ct_lab = lv_label_create(s_detail_ct_row);
    lv_label_set_text(s_ct_lab, "色温");
    lv_obj_set_style_text_font(s_ct_lab, &lv_font_noto_18_cjk, 0);
    lv_obj_set_style_text_color(s_ct_lab, COLOR_MUTED, 0);

    s_detail_ct_value = lv_label_create(s_detail_ct_row);
    lv_label_set_text(s_detail_ct_value, "--");
    lv_obj_set_style_text_font(s_detail_ct_value, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_detail_ct_value, COLOR_TEXT, 0);
    lv_obj_align(s_detail_ct_value, LV_ALIGN_TOP_RIGHT, 0, 0);

    s_detail_ct_slider = lv_slider_create(s_detail_ct_row);
    lv_obj_set_width(s_detail_ct_slider, 200);
    lv_obj_set_height(s_detail_ct_slider, 12);
    lv_slider_set_range(s_detail_ct_slider, 2700, 6500);
    lv_obj_align(s_detail_ct_slider, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_add_event_cb(s_detail_ct_slider, detail_slider_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_detail_ct_slider, detail_slider_changed_cb, LV_EVENT_RELEASED, NULL);
}

void ui_home_open_detail(const char *entity_id)
{
    if (s_detail == NULL || entity_id == NULL) {
        ESP_LOGW(TAG, "open detail 失败: detail=%p entity=%s", (void *)s_detail, entity_id ? entity_id : "(null)");
        return;
    }
    ESP_LOGI(TAG, "open detail: %s", entity_id);
    snprintf(s_detail_entity, sizeof(s_detail_entity), "%s", entity_id);
    detail_apply_entity();
    s_detail_visible = true;

    /* 藏掉底下的页面内容，只留背景图，避免卡片透过半透明子页露出来 */
    if (s_page >= 0 && s_page < s_page_count && s_pages[s_page]) {
        lv_obj_add_flag(s_pages[s_page], LV_OBJ_FLAG_HIDDEN);
    }
    if (s_title) {
        lv_obj_add_flag(s_title, LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < s_page_count; i++) {
        if (s_dots[i]) {
            lv_obj_add_flag(s_dots[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    lv_obj_remove_flag(s_detail, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_detail, 0, 0);          /* 位置固定在 0 */
    lv_obj_set_style_translate_x(s_detail, 360, 0);   /* 先移到屏幕右侧外 */

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_detail);
    lv_anim_set_values(&a, 360, 0);
    lv_anim_set_time(&a, 220);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, anim_translate_cb);       /* ← 之前这行丢了 */
    lv_anim_set_completed_cb(&a, detail_anim_done_cb);
    lv_anim_start(&a);
    s_animating = true;
}

static void detail_anim_done_cb(lv_anim_t *a)
{
    (void)a;
    s_animating = false;
    if (s_detail != NULL) {
        lv_area_t area;
        lv_obj_get_coords(s_detail, &area);
        ESP_LOGI(TAG, "detail_geom x=%d tx=%d hidden=%d idx=%d visible=%d",
                 (int)lv_obj_get_x(s_detail),
                 (int)lv_obj_get_style_translate_x(s_detail, 0),
                 (int)lv_obj_has_flag(s_detail, LV_OBJ_FLAG_HIDDEN),
                 (int)lv_obj_get_index(s_detail),
                 (int)lv_obj_is_visible(s_detail));
        (void)area;
    }
}

static void detail_close_done_cb(lv_anim_t *a)
{
    (void)a;
    if (s_detail != NULL) {
        lv_obj_add_flag(s_detail, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_translate_x(s_detail, 0, 0);
        lv_obj_set_pos(s_detail, 0, 0);
    }
    /* 恢复底下的页面 */
    if (s_page >= 0 && s_page < s_page_count && s_pages[s_page]) {
        lv_obj_remove_flag(s_pages[s_page], LV_OBJ_FLAG_HIDDEN);
    }
    if (s_title) {
        lv_obj_remove_flag(s_title, LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < s_page_count; i++) {
        if (s_dots[i]) {
            lv_obj_remove_flag(s_dots[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(s_dots[i], (i == s_page) ? COLOR_TEXT : lv_color_hex(0x3A3A3C), 0);
        }
    }
    s_animating = false;
}

void ui_home_close_detail(void)
{
    if (s_detail == NULL || !s_detail_visible) {
        return;
    }
    ESP_LOGI(TAG, "close detail");
    s_detail_visible = false;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_detail);
    lv_anim_set_values(&a, lv_obj_get_style_translate_x(s_detail, 0), 360);
    lv_anim_set_time(&a, 200);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&a, anim_translate_cb);
    lv_anim_set_completed_cb(&a, detail_close_done_cb);
    lv_anim_start(&a);
}

bool ui_home_detail_open(void)
{
    return s_detail_visible;
}

/* ---------------- 传感器折线图 ---------------- */

static void chart_back_cb(lv_event_t *e)
{
    (void)e;
    ui_home_close_chart();
}

/** 自己画折线：没有内部状态、不分配内存，内存紧张时也稳。 */
static void chart_draw_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    if (s_chart_pts_n < 2 || layer == NULL) {
        return;
    }
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    const int32_t w = lv_area_get_width(&a);
    const int32_t h = lv_area_get_height(&a);
    if (w < 2 || h < 2) {
        return;
    }

    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = lv_color_hex(0x5AC8FA);
    d.width = 3;
    d.round_start = true;
    d.round_end = true;

    const float span = (s_chart_hi - s_chart_lo) > 0.0001f ? (s_chart_hi - s_chart_lo) : 1.0f;
    int32_t prev_x = 0, prev_y = 0;
    for (int i = 0; i < s_chart_pts_n; i++) {
        float t = (s_chart_pts[i] - s_chart_lo) / span;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        const int32_t x = a.x1 + (int32_t)((int64_t)(w - 1) * i / (s_chart_pts_n - 1));
        const int32_t y = a.y2 - (int32_t)(t * (h - 1));
        if (i > 0) {
            d.p1.x = prev_x;
            d.p1.y = prev_y;
            d.p2.x = x;
            d.p2.y = y;
            lv_draw_line(layer, &d);
        }
        prev_x = x;
        prev_y = y;
    }
}

static void build_chart_view(lv_obj_t *scr)
{
    s_chart_page = lv_obj_create(scr);
    lv_obj_remove_style_all(s_chart_page);
    lv_obj_set_size(s_chart_page, 360, 360);
    lv_obj_set_pos(s_chart_page, 0, 0);
    lv_obj_set_style_bg_color(s_chart_page, lv_color_hex(0x0B0B0D), 0);
    lv_obj_set_style_bg_opa(s_chart_page, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_chart_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_chart_page, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *back = lv_button_create(s_chart_page);
    lv_obj_set_size(back, 36, 36);
    lv_obj_align(back, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_set_style_bg_opa(back, LV_OPA_20, 0);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_event_cb(back, chart_back_cb, LV_EVENT_CLICKED, NULL);
    s_chart_back_label = lv_label_create(back);
    lv_label_set_text(s_chart_back_label, LV_SYMBOL_LEFT);
    lv_obj_center(s_chart_back_label);

    s_chart_title = lv_label_create(s_chart_page);
    lv_label_set_text(s_chart_title, "--");
    lv_obj_set_style_text_font(s_chart_title, &lv_font_noto_18_cjk, 0);
    lv_obj_set_style_text_color(s_chart_title, COLOR_TEXT, 0);
    lv_obj_align(s_chart_title, LV_ALIGN_TOP_MID, 0, 58);

    s_chart_span = lv_label_create(s_chart_page);
    lv_label_set_text(s_chart_span, "近 3 小时");
    lv_obj_set_style_text_font(s_chart_span, &lv_font_noto_18_cjk, 0);
    lv_obj_set_style_text_color(s_chart_span, COLOR_MUTED, 0);
    lv_obj_align(s_chart_span, LV_ALIGN_TOP_MID, 0, 76);

    s_chart = lv_obj_create(s_chart_page);
    lv_obj_remove_style_all(s_chart);
    lv_obj_set_size(s_chart, 288, 150);
    lv_obj_align(s_chart, LV_ALIGN_TOP_MID, 0, 100);
    lv_obj_remove_flag(s_chart, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_chart, chart_draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    s_chart_min = lv_label_create(s_chart_page);
    lv_label_set_text(s_chart_min, "");
    lv_obj_set_style_text_font(s_chart_min, &lv_font_noto_18_cjk, 0);
    lv_obj_set_style_text_color(s_chart_min, COLOR_MUTED, 0);
    lv_obj_align(s_chart_min, LV_ALIGN_TOP_MID, -100, 268);

    s_chart_avg = lv_label_create(s_chart_page);
    lv_label_set_text(s_chart_avg, "");
    lv_obj_set_style_text_font(s_chart_avg, &lv_font_noto_18_cjk, 0);
    lv_obj_set_style_text_color(s_chart_avg, COLOR_TEXT, 0);
    lv_obj_align(s_chart_avg, LV_ALIGN_TOP_MID, 0, 268);

    s_chart_max = lv_label_create(s_chart_page);
    lv_label_set_text(s_chart_max, "");
    lv_obj_set_style_text_font(s_chart_max, &lv_font_noto_18_cjk, 0);
    lv_obj_set_style_text_color(s_chart_max, COLOR_MUTED, 0);
    lv_obj_align(s_chart_max, LV_ALIGN_TOP_MID, 100, 268);
}

void ui_home_show_chart(const char *entity_id, const float *vals, int n,
                        float vmin, float vmax, float vavg)
{
    if (s_chart_page == NULL || vals == NULL || n <= 0) {
        return;
    }
    if (!isfinite(vmin) || !isfinite(vmax) || !isfinite(vavg) ||
        fabsf(vmin) > 1.0e5f || fabsf(vmax) > 1.0e5f) {
        ESP_LOGW(TAG, "图表数值异常，忽略 (%f %f %f)", vmin, vmax, vavg);
        return;
    }
    snprintf(s_chart_entity, sizeof(s_chart_entity), "%s", entity_id ? entity_id : "");
    const ha_entity_t *e = ha_client_find(s_chart_entity);
    lv_label_set_text(s_chart_title, e ? e->name : s_chart_entity);

    /* 数值按 10 倍整数化（lv_chart 只收整数），纵向留 10% 余量避免贴边 */
    float lo = vmin, hi = vmax;
    if (hi - lo < 0.01f) {
        lo -= 1.0f;
        hi += 1.0f;
    }
    const float pad = (hi - lo) * 0.1f;
    s_chart_lo = lo - pad;
    s_chart_hi = hi + pad;

    const int keep = (n > 32) ? 32 : n;
    memcpy(s_chart_pts, vals, sizeof(float) * (size_t)keep);
    s_chart_pts_n = keep;
    lv_obj_invalidate(s_chart);

    /* 记下这次结果，60 秒内复用 */
    snprintf(s_chart_cache_entity, sizeof(s_chart_cache_entity), "%s", s_chart_entity);
    memcpy(s_chart_cache_vals, vals, sizeof(float) * (size_t)keep);
    s_chart_cache_n = keep;
    s_chart_cache_min = vmin;
    s_chart_cache_max = vmax;
    s_chart_cache_avg = vavg;
    s_chart_cache_at = lv_tick_get();

    lv_label_set_text(s_chart_span, "近 3 小时");
    const char *unit = (e != NULL) ? e->unit : "";
    char buf[40];
    snprintf(buf, sizeof(buf), "最低 %.1f%s", vmin, unit);
    lv_label_set_text(s_chart_min, buf);
    snprintf(buf, sizeof(buf), "平均 %.1f%s", vavg, unit);
    lv_label_set_text(s_chart_avg, buf);
    snprintf(buf, sizeof(buf), "最高 %.1f%s", vmax, unit);
    lv_label_set_text(s_chart_max, buf);

    const bool already_open = s_chart_visible;
    s_chart_visible = true;
    lv_obj_remove_flag(s_chart_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_chart_page, 0, 0);
    if (!already_open) {
        /* 只有第一次出现才滑入；数据晚到时不要再演一遍动画 */
        lv_obj_set_style_translate_x(s_chart_page, 360, 0);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_chart_page);
        lv_anim_set_values(&a, 360, 0);
        lv_anim_set_time(&a, 220);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&a, anim_translate_cb);
        lv_anim_start(&a);
    } else {
        lv_obj_set_style_translate_x(s_chart_page, 0, 0);
    }
    ESP_LOGI(TAG, "显示折线图: %s (%d 点)%s", s_chart_entity, n, already_open ? " (更新)" : "");
}

void ui_home_close_chart(void)
{
    if (s_chart_page == NULL || !s_chart_visible) {
        return;
    }
    s_chart_visible = false;
    s_chart_wanted = false;
    s_chart_gen++;          /* 在途的请求全部作废，返回后不会再自己弹出来 */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_chart_page);
    lv_anim_set_values(&a, 0, 360);
    lv_anim_set_time(&a, 200);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&a, anim_translate_cb);
    lv_anim_start(&a);
    lv_obj_add_flag(s_chart_page, LV_OBJ_FLAG_HIDDEN);
}

bool ui_home_chart_open(void)
{
    return s_chart_visible;
}

/* ---------------- 空调页 ---------------- */

static const char *hvac_label(const char *mode)
{
    if (strcmp(mode, "off") == 0) return "关";
    if (strcmp(mode, "auto") == 0) return "自动";
    if (strcmp(mode, "cool") == 0) return "制冷";
    if (strcmp(mode, "heat") == 0) return "制热";
    if (strcmp(mode, "dry") == 0) return "除湿";
    if (strcmp(mode, "fan_only") == 0) return "送风";
    return mode;
}

static const char *fan_label(const char *f)
{
    if (strcmp(f, "auto") == 0) return "自动";
    if (strcmp(f, "low") == 0) return "低";
    if (strcmp(f, "medium low") == 0) return "中低";
    if (strcmp(f, "medium") == 0) return "中";
    if (strcmp(f, "medium high") == 0) return "中高";
    if (strcmp(f, "high") == 0) return "高";
    if (strcmp(f, "silent") == 0) return "静音";
    if (strcmp(f, "turbo") == 0) return "强力";
    return f;
}

/* 一次点击的上下文：哪个实体、哪个服务、参数是什么 */
typedef struct {
    char entity[96];
    char service[24];
    char attrs[48];
} clim_cmd_t;

static void clim_chip_cb(lv_event_t *e)
{
    clim_cmd_t *c = (clim_cmd_t *)lv_event_get_user_data(e);
    if (c == NULL || s_cmd_q == NULL) {
        return;
    }
    cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    snprintf(cmd.entity, sizeof(cmd.entity), "%s", c->entity);
    snprintf(cmd.service, sizeof(cmd.service), "%s", c->service);
    snprintf(cmd.attrs, sizeof(cmd.attrs), "%s", c->attrs);
    xQueueSend(s_cmd_q, &cmd, 0);
}

static void clim_add_chip(lv_obj_t *row, const char *entity, const char *service,
                          const char *attrs, const char *label, bool active)
{
    lv_obj_t *chip = lv_button_create(row);
    lv_obj_set_size(chip, 52, 28);
    lv_obj_set_style_radius(chip, 14, 0);
    lv_obj_set_style_bg_color(chip, active ? lv_color_white() : lv_color_hex(0x24262B), 0);
    lv_obj_set_style_bg_opa(chip, active ? LV_OPA_COVER : LV_OPA_80, 0);
    lv_obj_set_style_pad_all(chip, 0, 0);

    lv_obj_t *lbl = lv_label_create(chip);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_font(lbl, &lv_font_noto_18_cjk, 0);
    lv_obj_set_style_text_color(lbl, active ? lv_color_black() : COLOR_TEXT, 0);
    lv_obj_center(lbl);

    clim_cmd_t *ctx = calloc(1, sizeof(clim_cmd_t));
    if (ctx != NULL) {
        snprintf(ctx->entity, sizeof(ctx->entity), "%s", entity);
        snprintf(ctx->service, sizeof(ctx->service), "%s", service);
        snprintf(ctx->attrs, sizeof(ctx->attrs), "%s", attrs);
        lv_obj_add_event_cb(chip, clim_chip_cb, LV_EVENT_CLICKED, ctx);
    }
}

static void clim_slider_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    const int v = (int)lv_slider_get_value(slider);
    if (lv_event_get_code(e) != LV_EVENT_RELEASED || s_cmd_q == NULL) {
        return;
    }
    cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    snprintf(cmd.entity, sizeof(cmd.entity), "%s", s_clim_entity);
    snprintf(cmd.service, sizeof(cmd.service), "set_temperature");
    snprintf(cmd.attrs, sizeof(cmd.attrs), "{\"temperature\":%d}", v);
    xQueueSend(s_cmd_q, &cmd, 0);
    char buf[24];
    snprintf(buf, sizeof(buf), "目标 %d°", v);
    lv_label_set_text(s_clim_temp, buf);
}

static void clim_back_cb(lv_event_t *e)
{
    (void)e;
    ui_home_close_climate();
}

static void build_climate_view(lv_obj_t *scr)
{
    s_clim_page = lv_obj_create(scr);
    lv_obj_remove_style_all(s_clim_page);
    lv_obj_set_size(s_clim_page, 360, 360);
    lv_obj_set_pos(s_clim_page, 0, 0);
    lv_obj_set_style_bg_color(s_clim_page, lv_color_hex(0x0B0B0D), 0);
    lv_obj_set_style_bg_opa(s_clim_page, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_clim_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_clim_page, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *back = lv_button_create(s_clim_page);
    lv_obj_set_size(back, 36, 36);
    lv_obj_align(back, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_set_style_bg_opa(back, LV_OPA_20, 0);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_event_cb(back, clim_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT);
    lv_obj_center(bl);

    s_clim_title = lv_label_create(s_clim_page);
    lv_label_set_text(s_clim_title, "--");
    lv_obj_set_style_text_font(s_clim_title, &lv_font_noto_18_cjk, 0);
    lv_obj_set_style_text_color(s_clim_title, COLOR_TEXT, 0);
    lv_obj_align(s_clim_title, LV_ALIGN_TOP_MID, 0, 54);

    s_clim_temp = lv_label_create(s_clim_page);
    lv_label_set_text(s_clim_temp, "--");
    lv_obj_set_style_text_font(s_clim_temp, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_clim_temp, COLOR_TEXT, 0);
    lv_obj_align(s_clim_temp, LV_ALIGN_TOP_MID, 0, 84);

    s_clim_span = lv_label_create(s_clim_page);
    lv_label_set_text(s_clim_span, "");
    lv_obj_set_style_text_font(s_clim_span, &lv_font_noto_18_cjk, 0);
    lv_obj_set_style_text_color(s_clim_span, COLOR_MUTED, 0);
    lv_obj_align(s_clim_span, LV_ALIGN_TOP_MID, 0, 116);

    s_clim_slider = lv_slider_create(s_clim_page);
    lv_obj_set_width(s_clim_slider, 280);
    lv_obj_set_height(s_clim_slider, 12);
    lv_obj_align(s_clim_slider, LV_ALIGN_TOP_MID, 0, 152);
    lv_slider_set_range(s_clim_slider, 16, 30);
    lv_obj_add_event_cb(s_clim_slider, clim_slider_cb, LV_EVENT_RELEASED, NULL);

    s_clim_modes_row = lv_obj_create(s_clim_page);
    lv_obj_remove_style_all(s_clim_modes_row);
    lv_obj_set_size(s_clim_modes_row, 330, 64);
    lv_obj_align(s_clim_modes_row, LV_ALIGN_TOP_MID, 0, 190);
    lv_obj_set_flex_flow(s_clim_modes_row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(s_clim_modes_row, 6, 0);

    s_clim_fans_row = lv_obj_create(s_clim_page);
    lv_obj_remove_style_all(s_clim_fans_row);
    lv_obj_set_size(s_clim_fans_row, 330, 64);
    lv_obj_align(s_clim_fans_row, LV_ALIGN_TOP_MID, 0, 262);
    lv_obj_set_flex_flow(s_clim_fans_row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(s_clim_fans_row, 6, 0);
}

void ui_home_open_climate(const char *entity_id)
{
    if (s_clim_page == NULL || entity_id == NULL) {
        return;
    }
    snprintf(s_clim_entity, sizeof(s_clim_entity), "%s", entity_id);
    const ha_entity_t *e = ha_client_find(s_clim_entity);
    if (e == NULL) {
        return;
    }
    lv_label_set_text(s_clim_title, e->name);

    char buf[48];
    if (e->cur_temp > 0) {
        snprintf(buf, sizeof(buf), "室内 %.1f°  目标 %.0f°", e->cur_temp, e->target_temp);
    } else {
        snprintf(buf, sizeof(buf), "目标 %.0f°", e->target_temp);
    }
    lv_label_set_text(s_clim_temp, buf);
    lv_label_set_text(s_clim_span, e->state);

    lv_slider_set_range(s_clim_slider, (int32_t)e->min_temp, (int32_t)e->max_temp);
    if (e->target_temp > 0) {
        lv_slider_set_value(s_clim_slider, (int32_t)e->target_temp, LV_ANIM_OFF);
    }

    /* 重建模式/风速按钮 */
    lv_obj_clean(s_clim_modes_row);
    if (e->hvac_modes[0]) {
        char tmp[80];
        snprintf(tmp, sizeof(tmp), "%s", e->hvac_modes);
        char *save = NULL;
        for (char *tok = strtok_r(tmp, ",", &save); tok != NULL; tok = strtok_r(NULL, ",", &save)) {
            char attrs[48];
            snprintf(attrs, sizeof(attrs), "{\"hvac_mode\":\"%s\"}", tok);
            clim_add_chip(s_clim_modes_row, s_clim_entity, "set_hvac_mode", attrs,
                          hvac_label(tok), strcmp(tok, e->state) == 0);
        }
    }

    lv_obj_clean(s_clim_fans_row);
    if (e->fan_modes[0]) {
        char tmp[80];
        snprintf(tmp, sizeof(tmp), "%s", e->fan_modes);
        char *save = NULL;
        int n = 0;
        for (char *tok = strtok_r(tmp, ",", &save); tok != NULL && n < 6; tok = strtok_r(NULL, ",", &save), n++) {
            char attrs[48];
            snprintf(attrs, sizeof(attrs), "{\"fan_mode\":\"%s\"}", tok);
            clim_add_chip(s_clim_fans_row, s_clim_entity, "set_fan_mode", attrs,
                          fan_label(tok), false);
        }
    }

    s_clim_visible = true;
    lv_obj_remove_flag(s_clim_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_clim_page, 0, 0);
    lv_obj_set_style_translate_x(s_clim_page, 360, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_clim_page);
    lv_anim_set_values(&a, 360, 0);
    lv_anim_set_time(&a, 220);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, anim_translate_cb);
    lv_anim_start(&a);
    ESP_LOGI(TAG, "打开空调页: %s (modes=%s)", s_clim_entity, e->hvac_modes);
}

void ui_home_close_climate(void)
{
    if (s_clim_page == NULL || !s_clim_visible) {
        return;
    }
    s_clim_visible = false;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_clim_page);
    lv_anim_set_values(&a, 0, 360);
    lv_anim_set_time(&a, 200);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&a, anim_translate_cb);
    lv_anim_start(&a);
    lv_obj_add_flag(s_clim_page, LV_OBJ_FLAG_HIDDEN);
}

bool ui_home_climate_open(void)
{
    return s_clim_visible;
}

static void build_ui(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    bg_image_bind(scr);       /* 背景图铺在最底层 */
    lv_obj_set_style_bg_color(scr, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    memset(s_pages, 0, sizeof(s_pages));
    memset(s_dots, 0, sizeof(s_dots));
    memset(s_tiles, 0, sizeof(s_tiles));
    memset(s_page_tiles, 0, sizeof(s_page_tiles));
    memset(s_sys_lines, 0, sizeof(s_sys_lines));

    s_json_pages = s_layout.page_count;
    s_page_count = s_json_pages + 1; /* 加 System 页 */
    s_title = NULL;
    s_wifi_icon = NULL;
    s_wifi_label = NULL;
    s_ha_dot = NULL;
    s_ota_btn = NULL;
    s_ota_bar = NULL;
    s_ota_status = NULL;

    /* 标题 */
    s_title = lv_label_create(scr);
    const char *first_title = s_layout_valid ? page_title(0) : "System";
    lv_label_set_text(s_title, first_title);
    lv_obj_set_style_text_color(s_title, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(s_title, title_font_for(first_title), 0);
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 18);

    /* 状态行 */
    s_wifi_color = COLOR_BAD;
    s_wifi_icon = lv_obj_create(scr);
    lv_obj_remove_style_all(s_wifi_icon);
    lv_obj_set_size(s_wifi_icon, 22, 18);
    lv_obj_set_pos(s_wifi_icon, 118, 50);
    lv_obj_remove_flag(s_wifi_icon, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_wifi_icon, wifi_icon_draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    s_wifi_label = lv_label_create(scr);
    lv_label_set_text(s_wifi_label, "--");
    lv_obj_set_style_text_color(s_wifi_label, COLOR_MUTED, 0);
    lv_obj_set_style_text_font(s_wifi_label, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(s_wifi_label, 142, 56);

    s_ha_dot = make_status_dot(scr, 197, 62);
    lv_obj_t *ha_lbl = lv_label_create(scr);
    lv_label_set_text(ha_lbl, "HA");
    lv_obj_set_style_text_color(ha_lbl, COLOR_MUTED, 0);
    lv_obj_set_style_text_font(ha_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(ha_lbl, 209, 56);

    /* JSON 页 */
    for (int p = 0; p < s_json_pages; p++) {
        lv_obj_t *page = lv_obj_create(scr);
        lv_obj_set_size(page, 360, 360);
        lv_obj_set_pos(page, 0, 0);
        lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(page, 0, 0);
        lv_obj_set_style_pad_all(page, 0, 0);
        lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
            s_pages[p] = page;

        const int n = s_layout.pages[p].tile_count;
        s_page_tiles[p] = n;

        /* 卡片放进一个可纵向滚动的容器：超过 4 个时上下滑 */
        lv_obj_t *grid = lv_obj_create(page);
        lv_obj_remove_style_all(grid);
        lv_obj_set_size(grid, 336, 244);            /* 84 .. 328 */
        lv_obj_set_pos(grid, 12, GRID_TOP);
        lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
        lv_obj_add_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(grid, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_style_pad_all(grid, 0, 0);

        for (int i = 0; i < n; i++) {
            /* 两列，行距 CARD_H + GAP；坐标相对滚动容器 */
            make_tile(grid, p, i, i % 2, i / 2);
        }
    }

    /* System 页 */
    build_status_page(scr);

    /* 翻页圆点 */
    if (s_page_count > 1) {
        for (int i = 0; i < s_page_count; i++) {
            int cx = 180 + (int)((i * 2 - (s_page_count - 1)) * 11); /* 间距 22 */
            lv_obj_t *hit = lv_obj_create(scr);
            lv_obj_set_size(hit, 24, 24);
            lv_obj_set_style_bg_opa(hit, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(hit, 0, 0);
            lv_obj_set_pos(hit, cx - 12, 318);
            lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(hit, dot_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

            lv_obj_t *dot = lv_obj_create(hit);
            lv_obj_set_size(dot, 8, 8);
            lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(dot, lv_color_hex(0x3A3A3C), 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(dot, 0, 0);
            lv_obj_center(dot);
            s_dots[i] = dot;
        }
    }

    s_hint = lv_label_create(scr);
    lv_label_set_text(s_hint, "");
    lv_obj_set_style_text_color(s_hint, COLOR_MUTED, 0);
    lv_obj_set_style_text_font(s_hint, &lv_font_montserrat_14, 0);
    lv_obj_align(s_hint, LV_ALIGN_CENTER, 0, 30);

    show_page(0);
    refresh_values();
    refresh_status_page();

    if (s_refresh_timer) {
        lv_timer_delete(s_refresh_timer);
    }
    s_refresh_timer = lv_timer_create(refresh_timer_cb, 500, NULL);

    lv_obj_add_event_cb(scr, gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(scr, swipe_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(scr, swipe_cb, LV_EVENT_RELEASED, NULL);

    /* 首帧：页码夹紧后只显示当前页（否则所有页叠在一起） */
    if (s_page >= s_page_count) {
        s_page = 0;
    }
    if (s_page < 0) {
        s_page = 0;
    }
    pages_apply_visibility();
    s_animating = false;
    ESP_LOGI(TAG, "UI built: %d page(s) + system", s_json_pages);

    build_detail_view(scr);
    s_detail_visible = false;
    build_chart_view(scr);
    s_chart_visible = false;
    build_climate_view(scr);
    s_clim_visible = false;

    ui_enable_gesture_bubble(scr);   /* 左右滑动翻页 */
}

static void apply_async_cb(void *arg)
{
    (void)arg;
    build_ui();
}

void ui_home_boot(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    bg_image_bind(scr);       /* 背景图铺在最底层 */
    lv_obj_set_style_bg_color(scr, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *t = lv_label_create(scr);
    lv_label_set_text(t, "Orbis");
    lv_obj_set_style_text_color(t, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_24, 0);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, -20);

    s_hint = lv_label_create(scr);
    lv_label_set_text(s_hint, "connecting...");
    lv_obj_set_style_text_color(s_hint, COLOR_MUTED, 0);
    lv_obj_set_style_text_font(s_hint, &lv_font_montserrat_14, 0);
    lv_obj_align(s_hint, LV_ALIGN_CENTER, 0, 20);

    if (s_cmd_q == NULL) {
        s_cmd_q = xQueueCreate(16, sizeof(cmd_t));
        if (s_cmd_q) {
            xTaskCreate(worker_task, "ui_cmd", 6144, NULL, 4, NULL);
        }
    }
}

void ui_home_apply(const layout_t *layout)
{
    if (layout != NULL && layout->page_count > 0) {
        s_layout = *layout;
        s_layout_valid = true;
    } else {
        /* 还没有收到 cfg：只保留 System 页 */
        memset(&s_layout, 0, sizeof(s_layout));
        s_layout_valid = false;
    }
    lv_lock();
    lv_async_call(apply_async_cb, NULL);
    lv_unlock();
}
