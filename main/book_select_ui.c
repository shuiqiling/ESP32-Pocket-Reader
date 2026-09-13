/* SPDX-FileCopyrightText: 2025 */
/* SPDX-License-Identifier: MIT */

#include "book_select_ui.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "novel_reader.h"
#include "web_scraper.h"
#include "bookshelf.h"
#include "pinyin_dict.h"
#include "sd_storage.h"
#include "main_menu_ui.h"

LV_FONT_DECLARE(novel_font_16);

static lv_obj_t *s_root;
static lv_obj_t *s_query;
static lv_obj_t *s_search_button;
static lv_obj_t *s_results;
static lv_obj_t *s_status;
static lv_obj_t *s_keyboard;
static lv_obj_t *s_ime;
static lv_timer_t *s_timer;
static bool s_waiting_reader;
static bool s_search_rendered;
static bool s_pending_search;
static uint32_t s_current_book_id;
static char s_current_book_title[BOOKSHELF_TITLE_SIZE];
static bookshelf_entry_t s_shelf[BOOKSHELF_MAX_BOOKS];
static int s_shelf_count;
static uint32_t s_pending_book_id;
static int s_pending_book_chapter;
static char s_pending_book_title[BOOKSHELF_TITLE_SIZE];
static web_search_result_t s_result_copy[WEB_SCRAPER_SEARCH_MAX];
static sd_book_entry_t s_sd_books[SD_STORAGE_MAX_BOOKS];
static int s_sd_book_count;
static int s_pending_sd_index = -1;
static bool s_local_only;
static void hide_keyboard(void);

static void home_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    hide_keyboard();
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    main_menu_ui_show(lv_display_get_default());
}

static void set_status(const char *text, uint32_t color)
{
    if (s_status) {
        lv_label_set_text(s_status, text);
        lv_obj_set_style_text_color(s_status, lv_color_hex(color), 0);
    }
}

static void hide_keyboard(void)
{
    if (s_keyboard) {
        lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
#if LV_USE_IME_PINYIN
    if (s_ime) {
        lv_obj_t *cand = lv_ime_pinyin_get_cand_panel(s_ime);
        if (cand) {
            lv_obj_add_flag(cand, LV_OBJ_FLAG_HIDDEN);
        }
    }
#endif
    if (s_status) {
        lv_obj_remove_flag(s_status, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_results && !s_waiting_reader) {
        lv_obj_remove_flag(s_results, LV_OBJ_FLAG_HIDDEN);
    }
}

static void show_keyboard(void)
{
    if (!s_keyboard) {
        return;
    }
    lv_keyboard_set_textarea(s_keyboard, s_query);
    lv_obj_remove_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
#if LV_USE_IME_PINYIN
    if (s_ime) {
        lv_obj_t *cand = lv_ime_pinyin_get_cand_panel(s_ime);
        if (cand) {
            lv_obj_remove_flag(cand, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(cand);
        }
    }
#endif
    lv_obj_move_foreground(s_keyboard);
    lv_obj_add_flag(s_status, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_results, LV_OBJ_FLAG_HIDDEN);
}

static void query_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        show_keyboard();
    }
}

static void enter_reader(void)
{
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    novel_reader_ui_init(lv_display_get_default());
}

static void enter_sd_book(int index)
{
    if (index < 0 || index >= s_sd_book_count) {
        return;
    }
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    sd_book_entry_t *book = &s_sd_books[index];
    if (book->type == SD_BOOK_ARCHIVE) {
        novel_reader_open_sd_archive(lv_display_get_default(), book->path,
                                     book->title);
    } else {
        novel_reader_open_sd_text(lv_display_get_default(), book->path,
                                  book->title);
    }
}

static void launch_book(uint32_t book_id, const char *title, int chapter)
{
    hide_keyboard();
    if (web_scraper_select_book_at(book_id, title, chapter) != ESP_OK ||
        web_scraper_start() != ESP_OK) {
        set_status("启动阅读任务失败，请重试", 0xb42318);
        lv_obj_remove_state(s_search_button, LV_STATE_DISABLED);
        return;
    }
    s_waiting_reader = true;
    lv_obj_add_state(s_search_button, LV_STATE_DISABLED);
    lv_obj_add_flag(s_results, LV_OBJ_FLAG_HIDDEN);
    char status[80];
    if (chapter > 1) {
        snprintf(status, sizeof(status), "正在恢复第 %d 章，完成后进入阅读", chapter);
    } else {
        snprintf(status, sizeof(status), "正在获取第一章，完成后立即进入阅读");
    }
    set_status(status, 0x2563eb);
}

static void begin_book(uint32_t book_id, const char *title, int chapter)
{
    if (book_id == s_current_book_id && web_scraper_is_running()) {
        enter_reader();
        return;
    }
    if (web_scraper_is_running()) {
        s_pending_book_id = book_id;
        s_pending_book_chapter = chapter;
        snprintf(s_pending_book_title, sizeof(s_pending_book_title), "%s", title);
        web_scraper_request_stop();
        lv_obj_add_state(s_search_button, LV_STATE_DISABLED);
        set_status("正在暂停当前书籍缓存...", 0x2563eb);
        return;
    }
    launch_book(book_id, title, chapter);
}

static void result_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || s_waiting_reader) {
        return;
    }
    intptr_t index = (intptr_t)lv_event_get_user_data(e);
    if (index < 0 || index >= WEB_SCRAPER_SEARCH_MAX ||
        s_result_copy[index].book_id == 0) {
        return;
    }
    begin_book(s_result_copy[index].book_id, s_result_copy[index].title,
               bookshelf_progress(s_result_copy[index].book_id));
}

static void shelf_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || s_waiting_reader) {
        return;
    }
    intptr_t index = (intptr_t)lv_event_get_user_data(e);
    if (index >= 0 && index < s_shelf_count) {
        begin_book(s_shelf[index].book_id, s_shelf[index].title,
                   s_shelf[index].chapter);
    }
}

static void sd_book_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || s_waiting_reader) {
        return;
    }
    intptr_t index = (intptr_t)lv_event_get_user_data(e);
    if (index < 0 || index >= s_sd_book_count) {
        return;
    }
    if (web_scraper_is_running()) {
        s_pending_sd_index = (int)index;
        web_scraper_request_stop();
        lv_obj_add_state(s_search_button, LV_STATE_DISABLED);
        set_status("正在暂停在线缓存，随后打开 TF 卡...", 0x2563eb);
        return;
    }
    enter_sd_book((int)index);
}

static void header_action_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || s_waiting_reader) {
        return;
    }
    /* While a search/cache transition owns running, changing screens would
     * leave the reader without its background cache task. */
    if (s_pending_search || s_pending_book_id != 0 ||
        lv_obj_has_state(s_search_button, LV_STATE_DISABLED)) {
        return;
    }
    if (s_current_book_id != 0) {
        begin_book(s_current_book_id, s_current_book_title,
                   bookshelf_progress(s_current_book_id));
    } else {
        begin_book(233, "斗罗大陆", bookshelf_progress(233));
    }
}

static void add_list_heading(const char *text)
{
    lv_obj_t *label = lv_label_create(s_results);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_height(label, 24);
    lv_obj_set_style_text_font(label, &novel_font_16, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0x7c6f64), 0);
    lv_label_set_text(label, text);
}

static void render_shelf(void)
{
    lv_obj_clean(s_results);
    s_shelf_count = bookshelf_get(s_shelf, BOOKSHELF_MAX_BOOKS);
    add_list_heading("我的书架 · 最近阅读");
    if (s_shelf_count == 0) {
        lv_obj_t *empty = lv_label_create(s_results);
        lv_obj_set_style_text_font(empty, &novel_font_16, 0);
        lv_obj_set_style_text_color(empty, lv_color_hex(0x9a8b7a), 0);
        lv_label_set_text(empty, "还没有阅读记录，先搜索一本书吧");
    } else {
        for (int i = 0; i < s_shelf_count; i++) {
            lv_obj_t *button = lv_button_create(s_results);
        lv_obj_set_width(button, LV_PCT(100));
        lv_obj_set_height(button, 42);
        lv_obj_set_style_radius(button, 9, 0);
        lv_obj_set_style_bg_color(button, lv_color_hex(0xfffdf7), 0);
        lv_obj_set_style_border_width(button, 1, 0);
        lv_obj_set_style_border_color(button, lv_color_hex(0xe4dccd), 0);
        lv_obj_set_style_shadow_width(button, 0, 0);
        lv_obj_add_event_cb(button, shelf_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        lv_obj_t *label = lv_label_create(button);
        lv_obj_set_style_text_font(label, &novel_font_16, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(0x29231d), 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(label, 270);
        char line[128];
        snprintf(line, sizeof(line), "%s  ·  第 %ld 章",
                 s_shelf[i].title, (long)s_shelf[i].chapter);
        lv_label_set_text(label, line);
            lv_obj_center(label);
        }
    }

    add_list_heading("TF 卡书库");
    size_t sd_count = 0;
    esp_err_t sd_err = sd_storage_list_books(
        s_sd_books, SD_STORAGE_MAX_BOOKS, &sd_count);
    s_sd_book_count = (int)sd_count;
    if (sd_err != ESP_OK) {
        lv_obj_t *unavailable = lv_label_create(s_results);
        lv_obj_set_style_text_font(unavailable, &novel_font_16, 0);
        lv_obj_set_style_text_color(unavailable, lv_color_hex(0xb42318), 0);
        lv_label_set_text(unavailable, "未识别 TF 卡，请插卡并使用 FAT32 格式");
        return;
    }
    if (s_sd_book_count == 0) {
        lv_obj_t *empty = lv_label_create(s_results);
        lv_obj_set_style_text_font(empty, &novel_font_16, 0);
        lv_obj_set_style_text_color(empty, lv_color_hex(0x9a8b7a), 0);
        lv_label_set_text(empty, "把 UTF-8 TXT 放到 TF 卡根目录或 novels 文件夹");
        return;
    }
    for (int i = 0; i < s_sd_book_count; i++) {
        lv_obj_t *button = lv_button_create(s_results);
        lv_obj_set_width(button, LV_PCT(100));
        lv_obj_set_height(button, 42);
        lv_obj_set_style_radius(button, 9, 0);
        lv_obj_set_style_bg_color(button, lv_color_hex(0xf0f7f4), 0);
        lv_obj_set_style_border_width(button, 1, 0);
        lv_obj_set_style_border_color(button, lv_color_hex(0xb9d4ca), 0);
        lv_obj_set_style_shadow_width(button, 0, 0);
        lv_obj_add_event_cb(button, sd_book_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        lv_obj_t *label = lv_label_create(button);
        lv_obj_set_style_text_font(label, &novel_font_16, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(0x23483d), 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(label, 270);
        char line[144];
        if (s_sd_books[i].type == SD_BOOK_ARCHIVE) {
            snprintf(line, sizeof(line), "%s · 已存 %d 章",
                     s_sd_books[i].title, s_sd_books[i].chapter_count);
        } else {
            snprintf(line, sizeof(line), "%s · TXT", s_sd_books[i].title);
        }
        lv_label_set_text(label, line);
        lv_obj_center(label);
    }
}

static void render_results(const web_search_status_t *search)
{
    lv_obj_clean(s_results);
    add_list_heading("搜索结果");
    s_search_rendered = false;
    memset(s_result_copy, 0, sizeof(s_result_copy));
    for (int i = 0; i < search->count && i < WEB_SCRAPER_SEARCH_MAX; i++) {
        s_result_copy[i] = search->results[i];
        lv_obj_t *button = lv_button_create(s_results);
        lv_obj_set_width(button, LV_PCT(100));
        lv_obj_set_height(button, 38);
        lv_obj_set_style_radius(button, 8, 0);
        lv_obj_set_style_bg_color(button, lv_color_hex(0xfffdf7), 0);
        lv_obj_set_style_border_width(button, 1, 0);
        lv_obj_set_style_border_color(button, lv_color_hex(0xe4dccd), 0);
        lv_obj_set_style_shadow_width(button, 0, 0);
        lv_obj_set_style_pad_hor(button, 9, 0);
        lv_obj_set_style_pad_ver(button, 4, 0);
        lv_obj_add_event_cb(button, result_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);

        lv_obj_t *label = lv_label_create(button);
        lv_obj_set_style_text_font(label, &novel_font_16, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(0x29231d), 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(label, 274);
        char line[150];
        if (search->results[i].author[0]) {
            snprintf(line, sizeof(line), "%s  ·  %s",
                     search->results[i].title, search->results[i].author);
        } else {
            snprintf(line, sizeof(line), "%s", search->results[i].title);
        }
        lv_label_set_text(label, line);
        lv_obj_center(label);
    }
}

static void start_search(void)
{
    const char *query = lv_textarea_get_text(s_query);
    if (!query || !query[0]) {
        set_status("请输入书名或作者", 0xb42318);
        return;
    }
    hide_keyboard();
    lv_obj_add_state(s_search_button, LV_STATE_DISABLED);
    lv_obj_clean(s_results);
    s_search_rendered = false;
    if (web_scraper_is_running()) {
        s_pending_search = true;
        web_scraper_request_stop();
        set_status("正在暂停后台缓存，随后搜索...", 0x2563eb);
        return;
    }
    set_status("正在搜索...", 0x2563eb);
    esp_err_t err = web_scraper_search(query);
    if (err != ESP_OK) {
        lv_obj_remove_state(s_search_button, LV_STATE_DISABLED);
        set_status("搜索任务启动失败", 0xb42318);
    }
}

static void search_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED && !s_waiting_reader) {
        start_search();
    }
}

static void keyboard_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        start_search();
    } else if (code == LV_EVENT_CANCEL) {
        hide_keyboard();
    }
}

static void poll_cb(lv_timer_t *timer)
{
    (void)timer;
    if ((s_pending_search || s_pending_book_id != 0 ||
         s_pending_sd_index >= 0) &&
        !web_scraper_is_running()) {
        if (s_pending_search) {
            s_pending_search = false;
            set_status("正在搜索...", 0x2563eb);
            esp_err_t err = web_scraper_search(lv_textarea_get_text(s_query));
            if (err != ESP_OK) {
                lv_obj_remove_state(s_search_button, LV_STATE_DISABLED);
                set_status("搜索任务启动失败", 0xb42318);
            }
        } else if (s_pending_book_id != 0) {
            uint32_t id = s_pending_book_id;
            int chapter = s_pending_book_chapter;
            char title[BOOKSHELF_TITLE_SIZE];
            snprintf(title, sizeof(title), "%s", s_pending_book_title);
            s_pending_book_id = 0;
            launch_book(id, title, chapter);
        } else {
            int index = s_pending_sd_index;
            s_pending_sd_index = -1;
            enter_sd_book(index);
        }
        return;
    }
    if (s_waiting_reader) {
        web_scraper_status_t status;
        web_scraper_get_status(&status);
        if (status.state == WEB_SCRAPER_READY || status.state == WEB_SCRAPER_DONE) {
            enter_reader();
        } else if (status.state == WEB_SCRAPER_FAILED) {
            s_waiting_reader = false;
            lv_obj_remove_state(s_search_button, LV_STATE_DISABLED);
            lv_obj_remove_flag(s_results, LV_OBJ_FLAG_HIDDEN);
            set_status(status.message, 0xb42318);
        } else {
            set_status(status.message, 0x2563eb);
        }
        return;
    }

    web_search_status_t search;
    web_scraper_get_search_status(&search);
    if (search.state == WEB_SEARCH_READY && !s_search_rendered) {
        render_results(&search);
        s_search_rendered = true;
        lv_obj_remove_state(s_search_button, LV_STATE_DISABLED);
        set_status(search.message, search.count ? 0x16835d : 0x7c6f64);
        /* Consume this UI state once so the list is not rebuilt every tick. */
        return;
    }
    if (search.state == WEB_SEARCH_FAILED) {
        lv_obj_remove_state(s_search_button, LV_STATE_DISABLED);
        set_status(search.message, 0xb42318);
    }
}

static void book_select_ui_show_mode(lv_display_t *disp, bool local_only)
{
    (void)disp;
    s_local_only = local_only;
    s_waiting_reader = false;
    /* Ignore the previous search snapshot and keep the shelf visible until
     * the user starts a fresh search. */
    s_search_rendered = true;
    s_pending_search = false;
    s_pending_book_id = 0;
    s_pending_sd_index = -1;
    s_current_book_id = web_scraper_get_book_id();
    web_scraper_get_book_title(s_current_book_title,
                               sizeof(s_current_book_title));
    memset(s_result_copy, 0, sizeof(s_result_copy));

    s_root = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, 320, 240);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(0xf4efe5), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);

    lv_obj_t *header = lv_obj_create(s_root);
    lv_obj_remove_style_all(header);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_size(header, 320, 38);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x263b35), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_t *title = lv_label_create(header);
    lv_obj_set_style_text_font(title, &novel_font_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xfffbeb), 0);
    lv_label_set_text(title, s_local_only ? "本地书库" : "在线选书");
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 68, 0);

    lv_obj_t *home = lv_button_create(header);
    lv_obj_set_pos(home, 6, 5);
    lv_obj_set_size(home, 52, 28);
    lv_obj_set_style_radius(home, 8, 0);
    lv_obj_set_style_bg_color(home, lv_color_hex(0x4f6f65), 0);
    lv_obj_add_event_cb(home, home_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *home_label = lv_label_create(home);
    lv_obj_set_style_text_font(home_label, &novel_font_16, 0);
    lv_obj_set_style_text_color(home_label, lv_color_white(), 0);
    lv_label_set_text(home_label, "主页");
    lv_obj_center(home_label);

    lv_obj_t *header_action = lv_button_create(header);
    lv_obj_set_size(header_action, 118, 28);
    lv_obj_align(header_action, LV_ALIGN_RIGHT_MID, -7, 0);
    lv_obj_set_style_radius(header_action, 14, 0);
    lv_obj_set_style_bg_color(header_action, lv_color_hex(0xeab464), 0);
    lv_obj_add_event_cb(header_action, header_action_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *header_action_label = lv_label_create(header_action);
    lv_obj_set_style_text_font(header_action_label, &novel_font_16, 0);
    lv_obj_set_style_text_color(header_action_label, lv_color_hex(0x33230f), 0);
    lv_label_set_text(header_action_label,
                      s_current_book_id ? "继续阅读" : "斗罗大陆");
    lv_obj_center(header_action_label);

    s_query = lv_textarea_create(s_root);
    lv_obj_set_pos(s_query, 10, 46);
    lv_obj_set_size(s_query, 229, 38);
    lv_obj_set_style_text_font(s_query, &novel_font_16, 0);
    lv_obj_set_style_radius(s_query, 9, 0);
    lv_obj_set_style_bg_color(s_query, lv_color_hex(0xfffdf7), 0);
    lv_obj_set_style_border_color(s_query, lv_color_hex(0xcfc4b2), 0);
    lv_obj_set_style_pad_top(s_query, 7, 0);
    lv_textarea_set_one_line(s_query, true);
    lv_textarea_set_max_length(s_query, 63);
    lv_textarea_set_placeholder_text(s_query, "输入书名或作者（支持拼音）");
    lv_obj_add_event_cb(s_query, query_cb, LV_EVENT_CLICKED, NULL);

    s_search_button = lv_button_create(s_root);
    lv_obj_set_pos(s_search_button, 246, 46);
    lv_obj_set_size(s_search_button, 64, 38);
    lv_obj_set_style_radius(s_search_button, 9, 0);
    lv_obj_set_style_bg_color(s_search_button, lv_color_hex(0xc9783b), 0);
    lv_obj_add_event_cb(s_search_button, search_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *search_label = lv_label_create(s_search_button);
    lv_obj_set_style_text_font(search_label, &novel_font_16, 0);
    lv_obj_set_style_text_color(search_label, lv_color_white(), 0);
    lv_label_set_text(search_label, "搜索");
    lv_obj_center(search_label);

    if (s_local_only) {
        lv_obj_add_flag(s_query, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_search_button, LV_OBJ_FLAG_HIDDEN);
    }

    s_status = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_status, &novel_font_16, 0);
    lv_obj_set_pos(s_status, 12, 88);
    lv_obj_set_width(s_status, 296);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_CLIP);
    set_status(s_local_only ? "选择 TF 卡中的 TXT 或已缓存书籍" :
                              "搜索书名或作者，阅读记录会自动加入书架",
               0x7c6f64);

    s_results = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_results);
    lv_obj_set_pos(s_results, 8, 109);
    lv_obj_set_size(s_results, 304, 126);
    lv_obj_set_flex_flow(s_results, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_results, 5, 0);
    lv_obj_set_style_pad_hor(s_results, 2, 0);
    lv_obj_set_scroll_dir(s_results, LV_DIR_VER);
    if (s_local_only) {
        lv_obj_set_pos(s_status, 12, 45);
        lv_obj_set_pos(s_results, 8, 68);
        lv_obj_set_size(s_results, 304, 167);
    }

    s_keyboard = lv_keyboard_create(s_root);
    lv_obj_set_size(s_keyboard, 320, 122);
    lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(s_keyboard, &novel_font_16, 0);
    lv_keyboard_set_textarea(s_keyboard, s_query);
    lv_obj_add_event_cb(s_keyboard, keyboard_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_keyboard, keyboard_cb, LV_EVENT_CANCEL, NULL);
#if LV_USE_IME_PINYIN
    s_ime = lv_ime_pinyin_create(s_root);
    lv_obj_set_style_text_font(s_ime, &novel_font_16, 0);
    lv_ime_pinyin_set_dict(s_ime, (lv_pinyin_dict_t *)novel_pinyin_dict);
    lv_ime_pinyin_set_keyboard(s_ime, s_keyboard);
    /* Full QWERTY pinyin is much less ambiguous than the old K9 mode and
     * lets book names be typed directly with one key per letter. */
    lv_ime_pinyin_set_mode(s_ime, LV_IME_PINYIN_MODE_K26);
    lv_obj_t *cand = lv_ime_pinyin_get_cand_panel(s_ime);
    lv_obj_set_size(cand, 320, 32);
    lv_obj_align_to(cand, s_keyboard, LV_ALIGN_OUT_TOP_MID, 0, 0);
    lv_obj_set_style_text_font(cand, &novel_font_16, 0);
#else
    s_ime = NULL;
#endif
    hide_keyboard();
    render_shelf();

    if (s_timer) {
        lv_timer_delete(s_timer);
    }
    s_timer = lv_timer_create(poll_cb, 250, NULL);
    lv_obj_t *old = lv_screen_active();
    lv_screen_load(s_root);
    if (old && old != s_root) {
        lv_obj_delete_async(old);
    }
}

void book_select_ui_show(lv_display_t *disp)
{
    book_select_ui_show_mode(disp, false);
}

void book_select_ui_show_local(lv_display_t *disp)
{
    book_select_ui_show_mode(disp, true);
}
