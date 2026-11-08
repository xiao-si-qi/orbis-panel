/**
 * 家庭控制屏界面：JSON 布局驱动 + 一个固件内置的「System」页。
 *
 * 页 0..N-1 来自 HA 的 /local/orbis/layout.json（每页最多 4 个 tile）；
 * 最后一页是固件自己画的系统页：固件版本 / Wi-Fi / IP / 运行时长 / 内存，
 * 以及「检查更新」按钮（拉 HA 上的 firmware.json，有新版本就下载并重启）。
 */

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
static lv_obj_t *s_wifi_dot;
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
static uint32_t s_last_gesture_ms;   /* 最近一次手势时间，用来吃掉误触 */
static QueueHandle_t s_cmd_q;

typedef struct {
    char entity[96];
    char service[12];
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
        return;
    }
    if (code != LV_EVENT_RELEASED || !pressed) {
        return;
    }
    pressed = false;

    const int dx = p.x - start.x;
    const int dy = p.y - start.y;
    ESP_LOGD(TAG, "swipe dx=%d dy=%d", dx, dy);

    if (abs(dx) < 40 || abs(dx) < abs(dy)) {
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
    lv_obj_set_pos(card, GRID_LEFT + col * (CARD_W + CARD_GAP), GRID_TOP + row * (CARD_H + CARD_GAP));
    /* 卡片是黑色半透明：叠在"背景图 + 暗化层"之上会更暗，形成玻璃质感 */
    lv_obj_set_style_bg_color(card, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(card, (lv_opa_t)(bg_image_card_opa_pct() * 255 / 100), 0);
    lv_obj_set_style_radius(card, 22, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, COLOR_BORDER, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    if (lt->type != TILE_SENSOR) {
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, tile_clicked_cb, LV_EVENT_CLICKED, w);
    }

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
                } else {
                    lv_label_set_text(w->value, e->state);
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
    (void)t;

    if (s_wifi_dot) {
        lv_obj_set_style_bg_color(s_wifi_dot, wifi_manager_is_connected() ? COLOR_OK : COLOR_BAD, 0);
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
            ESP_LOGI(TAG, "%s %s", cmd.service, cmd.entity);
            ha_client_action(cmd.entity, cmd.service);
            vTaskDelay(pdMS_TO_TICKS(300));
            const ha_entity_t *e = ha_client_find(cmd.entity);
            if (e) {
                ha_client_refresh_at((int)(e - ha_client_at(0)));
            }
        }
    }
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
    s_wifi_dot = NULL;
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
    s_wifi_dot = make_status_dot(scr, 131, 62);
    lv_obj_t *wifi_lbl = lv_label_create(scr);
    lv_label_set_text(wifi_lbl, "WiFi");
    lv_obj_set_style_text_color(wifi_lbl, COLOR_MUTED, 0);
    lv_obj_set_style_text_font(wifi_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(wifi_lbl, 143, 56);

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
        for (int i = 0; i < n; i++) {
            make_tile(page, p, i, i % 2, i / 2);
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
        s_cmd_q = xQueueCreate(8, sizeof(cmd_t));
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
    lv_async_call(apply_async_cb, NULL);
}
