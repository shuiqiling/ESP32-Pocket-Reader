/* SPDX-License-Identifier: MIT */

#include "main_menu_ui.h"

#include "book_select_ui.h"
#include "web_scraper.h"
#include "wifi_setup_ui.h"

LV_FONT_DECLARE(novel_font_16);

static void online_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        wifi_setup_ui_show(lv_display_get_default());
    }
}

static void download_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        wifi_setup_ui_show_download(lv_display_get_default());
    }
}

static void local_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        book_select_ui_show_local(lv_display_get_default());
    }
}

static void add_menu_button(lv_obj_t *parent, int y, uint32_t color,
                            const char *text, lv_event_cb_t cb)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_pos(button, 28, y);
    lv_obj_set_size(button, 264, 48);
    lv_obj_set_style_radius(button, 12, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(color), 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(button);
    lv_obj_set_style_text_font(label, &novel_font_16, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_label_set_text(label, text);
    lv_obj_center(label);
}

void main_menu_ui_show(lv_display_t *disp)
{
    (void)disp;
    if (web_scraper_is_running()) {
        web_scraper_request_stop();
    }

    lv_obj_t *root = lv_obj_create(NULL);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, 320, 240);
    lv_obj_set_style_bg_color(root, lv_color_hex(0xf4efe5), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(root);
    lv_obj_set_style_text_font(title, &novel_font_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x263b35), 0);
    lv_label_set_text(title, "小说阅读器 · 主页");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 14);

    add_menu_button(root, 48, 0x2563eb, "在线阅读", online_cb);
    add_menu_button(root, 106, 0xc9783b, "下载 TXT 到 TF 卡", download_cb);
    add_menu_button(root, 164, 0x16835d, "本地阅读", local_cb);

    lv_obj_t *old = lv_screen_active();
    lv_screen_load(root);
    if (old && old != root) {
        lv_obj_delete_async(old);
    }
}
