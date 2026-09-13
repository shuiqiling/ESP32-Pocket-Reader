/* SPDX-FileCopyrightText: 2025 */
/* SPDX-License-Identifier: MIT */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "novel_reader.h"
#include "web_scraper.h"
#include "display_brightness.h"
#include "book_select_ui.h"
#include "main_menu_ui.h"
#include "bookshelf.h"
#include "sd_storage.h"

LV_FONT_DECLARE(novel_font_16);

static const char *TAG = "reader";

#define MAX_CHAPTERS     2048
#define MAX_PAGES        300
#define PAGE_BUF_SIZE    1024
#define MAX_CHAPTER_FILE_SIZE (64 * 1024)

#define READER_MARGIN_X  8
#define READER_BODY_WIDTH 292
#define READER_BODY_HEIGHT 156
#define PAGE_FIT_MAX_CHARS 512

typedef struct {
    int number;
} chapter_entry_t;

typedef enum {
    READER_SOURCE_AUTO = 0,
    READER_SOURCE_SPIFFS,
    READER_SOURCE_ONLINE,
    READER_SOURCE_SD_ARCHIVE,
    READER_SOURCE_SD_TEXT,
} reader_source_t;

static lv_obj_t *body_label = NULL;
static lv_obj_t *title_label = NULL;
static lv_obj_t *page_label = NULL;
static lv_obj_t *brightness_overlay = NULL;
static lv_obj_t *brightness_value_label = NULL;

static chapter_entry_t chapters[MAX_CHAPTERS];
static int chapter_count = 0;
static int current_chapter = 0;
static int catalog_total = 0;
static int current_chapter_number = 0;
static int waiting_chapter = 0;
static bool waiting_open_at_end = false;
static uint32_t waiting_offset = 0;
static char selected_book_title[96] = "斗罗大陆";
static lv_timer_t *stream_timer = NULL;
static int current_page = 0;
static bool touch_held = false;
static uint32_t last_turn_ms = 0;

static char *current_raw = NULL;
static size_t current_size = 0;
static char current_title[80] = "";

/* Pages are slices of current_raw. Keeping offsets plus one reusable display
 * buffer avoids the old 1KB heap allocation for every page. */
static size_t page_offsets[MAX_PAGES + 1];
static size_t fit_offsets[PAGE_FIT_MAX_CHARS + 1];
static char page_buf[PAGE_BUF_SIZE];
static int page_count = 0;
static bool progress_dirty = false;
static uint32_t progress_changed_ms = 0;
static reader_source_t active_source = READER_SOURCE_AUTO;
static reader_source_t requested_source = READER_SOURCE_AUTO;
static char requested_path[SD_STORAGE_PATH_SIZE];
static char requested_title[SD_STORAGE_TITLE_SIZE];
static char local_base_path[SD_STORAGE_PATH_SIZE] = "/spiffs";
static char sd_text_path[SD_STORAGE_PATH_SIZE];
static uint32_t sd_text_size;
static uint32_t sd_text_offset;
static uint32_t sd_text_next_offset;
static uint32_t sd_text_page;
static uint32_t sd_text_index_key;

static void save_reading_progress(void)
{
    if (!progress_dirty) {
        return;
    }
    if (active_source == READER_SOURCE_SD_TEXT) {
        esp_err_t err = sd_storage_save_progress(sd_text_path, 1,
                                                 sd_text_offset);
        if (err == ESP_OK) {
            progress_dirty = false;
        } else {
            ESP_LOGW(TAG, "Cannot save TF text position: %s",
                     esp_err_to_name(err));
        }
        return;
    }
    if (active_source == READER_SOURCE_SD_ARCHIVE) {
        if (current_chapter < 0 || page_count <= 0) {
            return;
        }
        esp_err_t err = sd_storage_save_progress(
            local_base_path, current_chapter_number,
            (uint32_t)page_offsets[current_page]);
        if (err == ESP_OK) {
            progress_dirty = false;
        } else {
            ESP_LOGW(TAG, "Cannot save TF archive position: %s",
                     esp_err_to_name(err));
        }
        return;
    }
    if (active_source != READER_SOURCE_ONLINE ||
        current_chapter_number <= 0 || page_count <= 0) {
        progress_dirty = false;
        return;
    }
    uint32_t book_id = web_scraper_get_book_id();
    if (book_id == 0) {
        progress_dirty = false;
        return;
    }
    esp_err_t err = bookshelf_update(book_id, selected_book_title,
                                     current_chapter_number,
                                     (uint32_t)page_offsets[current_page]);
    if (err == ESP_OK) {
        progress_dirty = false;
    } else {
        ESP_LOGW(TAG, "Cannot save reading position: %s", esp_err_to_name(err));
    }
}

static void mark_reading_progress(void)
{
    progress_dirty = true;
    progress_changed_ms = (uint32_t)(esp_timer_get_time() / 1000);
}

static void brightness_update_label(void)
{
    if (!brightness_value_label) {
        return;
    }
    char text[24];
    snprintf(text, sizeof(text), "%d%%", display_brightness_get());
    lv_label_set_text(brightness_value_label, text);
}

static void brightness_slider_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *slider = lv_event_get_target_obj(e);
    if (code == LV_EVENT_VALUE_CHANGED) {
        display_brightness_set((int)lv_slider_get_value(slider));
        brightness_update_label();
    } else if (code == LV_EVENT_RELEASED) {
        esp_err_t err = display_brightness_save();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Cannot save brightness: %s", esp_err_to_name(err));
        }
    }
}

static void brightness_close_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || !brightness_overlay) {
        return;
    }
    lv_obj_delete_async(brightness_overlay);
    brightness_overlay = NULL;
    brightness_value_label = NULL;
}

static void brightness_button_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || brightness_overlay) {
        return;
    }

    lv_obj_t *root = lv_screen_active();
    brightness_overlay = lv_obj_create(root);
    lv_obj_remove_style_all(brightness_overlay);
    lv_obj_set_pos(brightness_overlay, 0, 0);
    lv_obj_set_size(brightness_overlay, 320, 240);
    lv_obj_set_style_bg_color(brightness_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(brightness_overlay, LV_OPA_40, 0);
    lv_obj_remove_flag(brightness_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(brightness_overlay);
    lv_obj_set_size(card, 286, 126);
    lv_obj_center(card);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0xfffdf7), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0xd8cfbd), 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *heading = lv_label_create(card);
    lv_obj_set_style_text_font(heading, &novel_font_16, 0);
    lv_obj_set_style_text_color(heading, lv_color_hex(0x27364a), 0);
    lv_label_set_text(heading, "阅读亮度");
    lv_obj_set_pos(heading, 4, 0);

    brightness_value_label = lv_label_create(card);
    lv_obj_set_style_text_font(brightness_value_label, &novel_font_16, 0);
    lv_obj_set_style_text_color(brightness_value_label, lv_color_hex(0xb45309), 0);
    lv_obj_align(brightness_value_label, LV_ALIGN_TOP_RIGHT, -4, 0);
    brightness_update_label();

    lv_obj_t *slider = lv_slider_create(card);
    lv_obj_set_pos(slider, 8, 43);
    lv_obj_set_size(slider, 238, 18);
    lv_slider_set_range(slider, 10, 100);
    lv_slider_set_value(slider, display_brightness_get(), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0xe5ded0), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0xf59e0b), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0xffffff), LV_PART_KNOB);
    lv_obj_set_style_border_width(slider, 2, LV_PART_KNOB);
    lv_obj_set_style_border_color(slider, lv_color_hex(0xf59e0b), LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 6, LV_PART_KNOB);
    lv_obj_add_event_cb(slider, brightness_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(slider, brightness_slider_cb, LV_EVENT_RELEASED, NULL);

    lv_obj_t *close = lv_button_create(card);
    lv_obj_set_size(close, 64, 30);
    lv_obj_align(close, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_radius(close, 8, 0);
    lv_obj_set_style_bg_color(close, lv_color_hex(0x27364a), 0);
    lv_obj_add_event_cb(close, brightness_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *close_label = lv_label_create(close);
    lv_obj_set_style_text_font(close_label, &novel_font_16, 0);
    lv_obj_set_style_text_color(close_label, lv_color_white(), 0);
    lv_label_set_text(close_label, "完成");
    lv_obj_center(close_label);
}

static void bookshelf_button_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (stream_timer) {
        lv_timer_delete(stream_timer);
        stream_timer = NULL;
    }
    save_reading_progress();
    free(current_raw);
    current_raw = NULL;
    current_size = 0;
    page_count = 0;
    body_label = NULL;
    title_label = NULL;
    page_label = NULL;
    brightness_overlay = NULL;
    brightness_value_label = NULL;
    book_select_ui_show(lv_display_get_default());
}

static void home_button_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (stream_timer) {
        lv_timer_delete(stream_timer);
        stream_timer = NULL;
    }
    save_reading_progress();
    free(current_raw);
    current_raw = NULL;
    current_size = 0;
    page_count = 0;
    body_label = NULL;
    title_label = NULL;
    page_label = NULL;
    brightness_overlay = NULL;
    brightness_value_label = NULL;
    main_menu_ui_show(lv_display_get_default());
}

static const char *utf8_next(const char *p, const char *end)
{
    if (p >= end) {
        return end;
    }
    p++;
    while (p < end && ((unsigned char)*p & 0xC0) == 0x80) {
        p++;
    }
    return p;
}

static int chapter_cmp(const void *a, const void *b)
{
    const chapter_entry_t *ca = (const chapter_entry_t *)a;
    const chapter_entry_t *cb = (const chapter_entry_t *)b;
    return (ca->number > cb->number) - (ca->number < cb->number);
}

static bool mount_spiffs(void)
{
    if (esp_spiffs_mounted("spiffs")) {
        ESP_LOGI(TAG, "SPIFFS already mounted");
        return true;
    }

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "spiffs",
        .max_files = 4,
        .format_if_mount_failed = false,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "SPIFFS mounted");
    return true;
}

static void recover_interrupted_chapter_writes(void)
{
    for (int i = 1; i <= MAX_CHAPTERS; i++) {
        char final_path[64];
        char bak_path[64];
        char tmp_path[64];
        struct stat st;
        snprintf(final_path, sizeof(final_path), "/spiffs/%03d.txt", i);
        snprintf(bak_path, sizeof(bak_path), "/spiffs/.%03d.bak", i);
        snprintf(tmp_path, sizeof(tmp_path), "/spiffs/.%03d.tmp", i);

        bool final_exists = stat(final_path, &st) == 0;
        bool bak_exists = stat(bak_path, &st) == 0;
        if (!final_exists && bak_exists) {
            if (rename(bak_path, final_path) == 0) {
                ESP_LOGW(TAG, "Recovered interrupted chapter %03d", i);
                final_exists = true;
                bak_exists = false;
            } else {
                ESP_LOGE(TAG, "Cannot recover chapter %03d", i);
            }
        }
        if (final_exists && bak_exists) {
            (void)remove(bak_path);
        }
        if (final_exists) {
            (void)remove(tmp_path);
        }
    }
}

static void scan_chapters(const char *base_path, bool recover_spiffs)
{
    if (recover_spiffs) {
        recover_interrupted_chapter_writes();
    }
    chapter_count = 0;
    DIR *d = opendir(base_path);
    if (!d) {
        ESP_LOGW(TAG, "Cannot open %s", base_path);
        return;
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *name = ent->d_name;
        size_t len = strlen(name);
        if (len <= 4 || strcasecmp(name + len - 4, ".txt") != 0) {
            continue;
        }
        int number = 0;
        char extra = '\0';
        char expected[24];
        if (sscanf(name, "%d.txt%c", &number, &extra) != 1 || number < 1) {
            continue;
        }
        snprintf(expected, sizeof(expected),
                 recover_spiffs ? "%03d.txt" : "%06d.txt", number);
        if (strcmp(name, expected) != 0) {
            continue;
        }
        if (chapter_count >= MAX_CHAPTERS) {
            break;
        }
        chapters[chapter_count].number = number;
        chapter_count++;
    }
    closedir(d);
    qsort(chapters, chapter_count, sizeof(chapters[0]), chapter_cmp);
    ESP_LOGI(TAG, "Found %d chapters", chapter_count);
}

static bool read_text_file(const char *path, char **out_buf, size_t *out_size)
{
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
        return false;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }

    size_t sz = (size_t)st.st_size;
    if (sz > MAX_CHAPTER_FILE_SIZE) {
        ESP_LOGE(TAG, "Chapter file too large: %u bytes", (unsigned)sz);
        fclose(f);
        return false;
    }
    char *buf = malloc(sz + 1);
    if (!buf) {
        fclose(f);
        return false;
    }
    size_t rd = fread(buf, 1, sz, f);
    bool ok = (rd == sz) && !ferror(f);
    fclose(f);
    if (!ok) {
        free(buf);
        return false;
    }
    buf[rd] = '\0';

    *out_buf = buf;
    *out_size = rd;
    return true;
}

static bool text_slice_fits(char *text, size_t start, size_t end)
{
    char saved = text[end];
    text[end] = '\0';
    lv_point_t size;
    lv_text_get_size(&size, text + start, &novel_font_16, 0, 2,
                     READER_BODY_WIDTH, LV_TEXT_FLAG_NONE);
    text[end] = saved;
    return size.y <= READER_BODY_HEIGHT;
}

static size_t utf8_valid_prefix(const char *text, size_t len, bool *invalid)
{
    size_t pos = 0;
    *invalid = false;
    while (pos < len) {
        unsigned char lead = (unsigned char)text[pos];
        size_t width = 1;
        if (lead < 0x80) {
            width = 1;
        } else if (lead >= 0xC2 && lead <= 0xDF) {
            width = 2;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            width = 3;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            width = 4;
        } else {
            *invalid = true;
            return pos;
        }
        if (pos + width > len) {
            break;
        }
        for (size_t i = 1; i < width; i++) {
            if (((unsigned char)text[pos + i] & 0xC0) != 0x80) {
                *invalid = true;
                return pos;
            }
        }
        pos += width;
    }
    return pos;
}

static size_t fit_one_page(char *text, size_t len)
{
    int candidates = 0;
    const char *end = text + len;
    const char *p = text;
    fit_offsets[0] = 0;
    while (p < end && candidates < PAGE_FIT_MAX_CHARS) {
        p = utf8_next(p, end);
        fit_offsets[++candidates] = (size_t)(p - text);
    }
    int low = 1;
    int high = candidates;
    int best = 0;
    while (low <= high) {
        int mid = low + (high - low) / 2;
        if (text_slice_fits(text, 0, fit_offsets[mid])) {
            best = mid;
            low = mid + 1;
        } else {
            high = mid - 1;
        }
    }
    return best > 0 ? fit_offsets[best] : 0;
}

static bool load_sd_text_page(uint32_t offset, uint32_t page)
{
    size_t got = 0;
    uint32_t file_size = 0;
    esp_err_t err = sd_storage_read(sd_text_path, offset, page_buf,
                                    sizeof(page_buf) - 1, &got, &file_size);
    if (err != ESP_OK || got == 0) {
        return false;
    }
    page_buf[got] = '\0';
    size_t prefix = 0;
    if (offset == 0 && got >= 3 &&
        (unsigned char)page_buf[0] == 0xEF &&
        (unsigned char)page_buf[1] == 0xBB &&
        (unsigned char)page_buf[2] == 0xBF) {
        prefix = 3;
    }
    bool invalid = false;
    size_t valid = utf8_valid_prefix(page_buf + prefix, got - prefix, &invalid);
    if (invalid) {
        lv_label_set_text(body_label,
                          "这个 TXT 不是 UTF-8 编码\n请先转换为 UTF-8 后再放入 TF 卡");
        lv_label_set_text(page_label, "TF · 编码不支持");
        page_count = 0;
        return false;
    }
    size_t used = fit_one_page(page_buf + prefix, valid);
    if (used == 0) {
        return false;
    }
    page_buf[prefix + used] = '\0';
    lv_label_set_text(body_label, page_buf + prefix);
    lv_obj_set_height(body_label, READER_BODY_HEIGHT);
    char title[192];
    snprintf(title, sizeof(title), "《%s》", selected_book_title);
    lv_label_set_text(title_label, title);
    sd_text_size = file_size;
    sd_text_offset = offset;
    sd_text_next_offset = offset + (uint32_t)(prefix + used);
    sd_text_page = page;
    page_count = 1;
    current_page = 0;
    unsigned percent = file_size > 0 ?
        (unsigned)((uint64_t)sd_text_next_offset * 100u / file_size) : 100u;
    if (percent > 100) percent = 100;
    char footer[96];
    snprintf(footer, sizeof(footer), "TF · 第 %lu 页 · %u%%",
             (unsigned long)(page + 1), percent);
    lv_label_set_text(page_label, footer);
    return true;
}

static bool open_sd_text(void)
{
    char probe = 0;
    size_t got = 0;
    uint32_t file_size = 0;
    if (sd_storage_read(sd_text_path, 0, &probe, 1, &got, &file_size) != ESP_OK ||
        file_size == 0 || file_size > INT32_MAX) {
        return false;
    }
    uint32_t indexed_pages = 0;
    if (sd_storage_prepare_page_index(sd_text_path, file_size,
                                      &sd_text_index_key,
                                      &indexed_pages) != ESP_OK) {
        return false;
    }
    int saved_chapter = 1;
    uint32_t saved_offset = 0;
    (void)sd_storage_load_progress(sd_text_path, &saved_chapter,
                                   &saved_offset);
    if (saved_offset >= file_size) saved_offset = 0;
    uint32_t page = 0;
    uint32_t indexed_offset = 0;
    if (sd_storage_find_page(sd_text_index_key, saved_offset, &page,
                             &indexed_offset) != ESP_OK) {
        page = 0;
        indexed_offset = 0;
    }
    return load_sd_text_page(indexed_offset, page);
}

static bool build_page_index(char *text, size_t len,
                             size_t *offsets, int *out_count)
{
    *out_count = 0;
    if (!text || len == 0) {
        return false;
    }

    char *cur = text;
    const char *end = text + len;
    offsets[0] = 0;

    while (cur < end && *out_count < MAX_PAGES) {
        int candidates = 0;
        const char *p = cur;
        fit_offsets[0] = (size_t)(cur - text);
        while (p < end && candidates < PAGE_FIT_MAX_CHARS) {
            const char *next = utf8_next(p, end);
            if ((size_t)(next - cur) >= PAGE_BUF_SIZE) {
                break;
            }
            p = next;
            fit_offsets[++candidates] = (size_t)(p - text);
        }
        if (candidates == 0) {
            break;
        }

        int low = 1;
        int high = candidates;
        int best = 0;
        while (low <= high) {
            int mid = low + (high - low) / 2;
            if (text_slice_fits(text, fit_offsets[0], fit_offsets[mid])) {
                best = mid;
                low = mid + 1;
            } else {
                high = mid - 1;
            }
        }
        if (best == 0) {
            best = 1;
        }
        (*out_count)++;
        offsets[*out_count] = fit_offsets[best];
        cur = text + fit_offsets[best];
    }
    return cur == end && *out_count > 0;
}


static void extract_title(const char *text, size_t len, char *out, size_t out_size)
{
    out[0] = '\0';
    if (!text || len == 0) {
        return;
    }

    const char *nl = memchr(text, '\n', len);
    size_t line_len = nl ? (size_t)(nl - text) : len;

    /* trim leading spaces */
    const char *start = text;
    const char *end = text + line_len;
    while (start < end && (*start == ' ' || *start == '\t' || *start == '\r')) {
        start++;
    }
    while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) {
        end--;
    }

    size_t n = (size_t)(end - start);
    if (n >= out_size) {
        n = out_size - 1;
    }
    memcpy(out, start, n);
    out[n] = '\0';
}

static void update_display(void)
{
    if (active_source == READER_SOURCE_SD_TEXT) {
        return;
    }
    if (!body_label || page_count == 0) {
        if (body_label) {
            lv_label_set_text(body_label,
                              "没有可读内容\n请返回主页选择在线阅读或本地阅读");
        }
        return;
    }

    if (current_page < 0) {
        current_page = 0;
    }
    if (current_page >= page_count) {
        current_page = page_count - 1;
    }

    size_t start = page_offsets[current_page];
    size_t end = page_offsets[current_page + 1];
    size_t n = end > start ? end - start : 0;
    if (n >= sizeof(page_buf)) {
        n = sizeof(page_buf) - 1;
    }
    memcpy(page_buf, current_raw + start, n);
    page_buf[n] = '\0';
    lv_label_set_text(body_label, page_buf);
    /* WRAP recalculates label height when text changes. Restore the fixed
     * viewport after every update so it can never draw into the footer. */
    lv_obj_set_height(body_label, READER_BODY_HEIGHT);

    char buf[192];
    if (current_title[0] != '\0') {
        snprintf(buf, sizeof(buf), "《%s》%s", selected_book_title, current_title);
    } else if (chapter_count > 0 && current_chapter >= 0 && current_chapter < chapter_count) {
        snprintf(buf, sizeof(buf), "《%s》第%d章", selected_book_title,
                 chapters[current_chapter].number);
    } else {
        snprintf(buf, sizeof(buf), "《%s》", selected_book_title);
    }
    lv_label_set_text(title_label, buf);

    if (active_source == READER_SOURCE_SD_ARCHIVE && chapter_count > 0) {
        snprintf(buf, sizeof(buf), "TF · 第 %d 章 · 已存 %d 章  %d/%d 页",
                 current_chapter_number, chapter_count,
                 current_page + 1, page_count);
    } else if (catalog_total > 0 && current_chapter_number > 0 &&
        web_scraper_catalog_complete()) {
        snprintf(buf, sizeof(buf), "第 %d/%d 章  %d/%d 页",
                 current_chapter_number, catalog_total,
                 current_page + 1, page_count);
    } else if (catalog_total > 0 && current_chapter_number > 0) {
        snprintf(buf, sizeof(buf), "第 %d 章  %d/%d 页 · 已发现 %d 章",
                 current_chapter_number, current_page + 1, page_count,
                 catalog_total);
    } else {
        snprintf(buf, sizeof(buf), "第 %d / %d 页",
                 current_page + 1, page_count);
    }
    lv_label_set_text(page_label, buf);
}

static bool load_chapter_path(const char *path, int local_index,
                              int absolute_number, bool open_at_end,
                              uint32_t restore_offset)
{
    char *new_raw = NULL;
    size_t new_size = 0;
    if (!read_text_file(path, &new_raw, &new_size)) {
        return false;
    }

    size_t new_offsets[MAX_PAGES + 1];
    int new_page_count = 0;
    if (!build_page_index(new_raw, new_size, new_offsets, &new_page_count)) {
        ESP_LOGE(TAG, "Cannot paginate complete chapter: %s", path);
        free(new_raw);
        return false;
    }

    char new_title[sizeof(current_title)];
    extract_title(new_raw, new_size, new_title, sizeof(new_title));

    free(current_raw);
    current_raw = new_raw;
    current_size = new_size;
    memcpy(page_offsets, new_offsets,
           (size_t)(new_page_count + 1) * sizeof(page_offsets[0]));
    page_count = new_page_count;
    snprintf(current_title, sizeof(current_title), "%s", new_title);
    current_chapter = local_index;
    current_chapter_number = absolute_number;
    current_page = open_at_end ? new_page_count - 1 : 0;
    if (!open_at_end && restore_offset > 0) {
        for (int i = 0; i < new_page_count; i++) {
            if (restore_offset < page_offsets[i + 1]) {
                current_page = i;
                break;
            }
            current_page = i;
        }
    }
    ESP_LOGI(TAG, "Open %s: %d bytes, %d pages", path, (int)current_size, page_count);
    uint32_t book_id = web_scraper_get_book_id();
    if (active_source == READER_SOURCE_ONLINE && book_id != 0 &&
        absolute_number > 0) {
        esp_err_t err = bookshelf_update(book_id, selected_book_title,
                                         absolute_number,
                                         (uint32_t)page_offsets[current_page]);
        if (err == ESP_OK) {
            progress_dirty = false;
        } else {
            mark_reading_progress();
            ESP_LOGW(TAG, "Cannot update bookshelf: %s", esp_err_to_name(err));
        }
    } else if (active_source == READER_SOURCE_SD_ARCHIVE &&
               absolute_number > 0) {
        esp_err_t err = sd_storage_save_progress(
            local_base_path, absolute_number,
            (uint32_t)page_offsets[current_page]);
        if (err == ESP_OK) {
            progress_dirty = false;
        } else {
            mark_reading_progress();
        }
    }
    update_display();
    return true;
}

static bool open_local_chapter(int idx, bool open_at_end,
                               uint32_t restore_offset)
{
    if (idx < 0 || idx >= chapter_count) {
        return false;
    }
    char path[SD_STORAGE_PATH_SIZE + 72];
    char chapter_name[24];
    snprintf(chapter_name, sizeof(chapter_name),
             active_source == READER_SOURCE_SD_ARCHIVE ? "%06d.txt" :
                                                         "%03d.txt",
             chapters[idx].number);
    snprintf(path, sizeof(path), "%s/%s", local_base_path, chapter_name);
    int absolute_number = active_source == READER_SOURCE_SD_ARCHIVE ?
                          chapters[idx].number : 0;
    if (!load_chapter_path(path, idx, absolute_number, open_at_end,
                           restore_offset)) {
        ESP_LOGE(TAG, "Failed to read %s", path);
        return false;
    }
    return true;
}

static bool open_stream_chapter(int chapter_number, bool open_at_end,
                                uint32_t restore_offset)
{
    if (chapter_number < 1 || chapter_number > catalog_total) {
        return false;
    }
    char path[64];
    snprintf(path, sizeof(path), "/spiffs/cache_%03d.txt", chapter_number);
    if (!load_chapter_path(path, -1, chapter_number, open_at_end,
                           restore_offset)) {
        return false;
    }
    waiting_chapter = 0;
    web_scraper_set_reading_chapter(chapter_number);
    return true;
}

static void request_stream_chapter_at(int chapter_number, bool open_at_end)
{
    if (chapter_number < 1 || chapter_number > catalog_total) {
        return;
    }
    web_scraper_set_reading_chapter(chapter_number);
    if (open_stream_chapter(chapter_number, open_at_end, 0)) {
        return;
    }
    waiting_chapter = chapter_number;
    waiting_open_at_end = open_at_end;
    waiting_offset = 0;
    char status[64];
    snprintf(status, sizeof(status), "第 %d 章正在加载...", chapter_number);
    lv_label_set_text(page_label, status);
    ESP_LOGI(TAG, "waiting for chapter %d", chapter_number);
}

static void stream_poll_cb(lv_timer_t *timer)
{
    (void)timer;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (progress_dirty && now_ms - progress_changed_ms >= 2000) {
        save_reading_progress();
    }
    if (active_source != READER_SOURCE_ONLINE) {
        return;
    }
    catalog_total = web_scraper_get_catalog_size();
    if (catalog_total <= 0 || !page_label) {
        return;
    }
    if (waiting_chapter > 0) {
        int target = waiting_chapter;
        if (open_stream_chapter(target, waiting_open_at_end, waiting_offset)) {
            ESP_LOGI(TAG, "chapter %d became available", target);
            return;
        }
    }

    web_scraper_status_t status;
    if (web_scraper_get_status(&status) == ESP_OK &&
        status.state == WEB_SCRAPER_FAILED) {
        char text[96];
        snprintf(text, sizeof(text), "缓存失败：%.60s", status.message);
        lv_label_set_text(page_label, text);
    }
}

void novel_reader_ui_init(lv_display_t *disp)
{
    (void)disp;

    mount_spiffs();
    active_source = requested_source;
    if (active_source == READER_SOURCE_SD_TEXT ||
        active_source == READER_SOURCE_SD_ARCHIVE) {
        snprintf(selected_book_title, sizeof(selected_book_title), "%s",
                 requested_title);
        snprintf(local_base_path, sizeof(local_base_path), "%s",
                 requested_path);
        snprintf(sd_text_path, sizeof(sd_text_path), "%s", requested_path);
        catalog_total = 0;
    } else {
        web_scraper_get_book_title(selected_book_title,
                                   sizeof(selected_book_title));
        catalog_total = web_scraper_get_catalog_size();
        if (active_source == READER_SOURCE_AUTO) {
            active_source = catalog_total > 0 ? READER_SOURCE_ONLINE :
                                               READER_SOURCE_SPIFFS;
        }
        if (active_source == READER_SOURCE_SPIFFS) {
            catalog_total = 0;
            snprintf(local_base_path, sizeof(local_base_path), "/spiffs");
        }
    }
    requested_source = READER_SOURCE_AUTO;
    waiting_chapter = 0;
    waiting_open_at_end = false;
    waiting_offset = 0;
    progress_dirty = false;
    brightness_overlay = NULL;
    brightness_value_label = NULL;
    current_chapter_number = 0;
    if (active_source == READER_SOURCE_SPIFFS) {
        scan_chapters(local_base_path, true);
    } else if (active_source == READER_SOURCE_SD_ARCHIVE) {
        scan_chapters(local_base_path, false);
    } else {
        chapter_count = 0;
    }

    lv_obj_t *root = lv_obj_create(NULL);
    lv_obj_set_size(root, 320, 240);
    lv_obj_remove_style_all(root);
    lv_obj_set_style_bg_color(root, lv_color_hex(0xf3efe5), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *header = lv_obj_create(root);
    lv_obj_remove_style_all(header);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_size(header, 320, 27);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x27364a), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);

    title_label = lv_label_create(header);
    lv_obj_set_style_text_font(title_label, &novel_font_16, 0);
    lv_obj_set_style_text_color(title_label, lv_color_hex(0xf8fafc), 0);
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(title_label, 142);
    lv_obj_align(title_label, LV_ALIGN_LEFT_MID, 8, 0);
    char initial_title[112];
    snprintf(initial_title, sizeof(initial_title), "《%s》", selected_book_title);
    lv_label_set_text(title_label, initial_title);

    lv_obj_t *home_button = lv_button_create(header);
    lv_obj_set_size(home_button, 44, 23);
    lv_obj_align(home_button, LV_ALIGN_RIGHT_MID, -108, 0);
    lv_obj_set_style_radius(home_button, 7, 0);
    lv_obj_set_style_bg_color(home_button, lv_color_hex(0x475569), 0);
    lv_obj_set_style_shadow_width(home_button, 0, 0);
    lv_obj_set_style_pad_all(home_button, 0, 0);
    lv_obj_add_event_cb(home_button, home_button_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *home_button_label = lv_label_create(home_button);
    lv_obj_set_style_text_font(home_button_label, &novel_font_16, 0);
    lv_obj_set_style_text_color(home_button_label, lv_color_white(), 0);
    lv_label_set_text(home_button_label, "主页");
    lv_obj_center(home_button_label);

    lv_obj_t *bookshelf_button = lv_button_create(header);
    lv_obj_set_size(bookshelf_button, 44, 23);
    lv_obj_align(bookshelf_button, LV_ALIGN_RIGHT_MID, -58, 0);
    lv_obj_set_style_radius(bookshelf_button, 7, 0);
    lv_obj_set_style_bg_color(bookshelf_button, lv_color_hex(0x4f6f65), 0);
    lv_obj_set_style_shadow_width(bookshelf_button, 0, 0);
    lv_obj_set_style_pad_all(bookshelf_button, 0, 0);
    lv_obj_add_event_cb(bookshelf_button, bookshelf_button_cb,
                        LV_EVENT_CLICKED, NULL);
    lv_obj_t *bookshelf_button_label = lv_label_create(bookshelf_button);
    lv_obj_set_style_text_font(bookshelf_button_label, &novel_font_16, 0);
    lv_obj_set_style_text_color(bookshelf_button_label, lv_color_white(), 0);
    lv_label_set_text(bookshelf_button_label, "书架");
    lv_obj_center(bookshelf_button_label);

    lv_obj_t *brightness_button = lv_button_create(header);
    lv_obj_set_size(brightness_button, 50, 23);
    lv_obj_align(brightness_button, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_set_style_radius(brightness_button, 7, 0);
    lv_obj_set_style_bg_color(brightness_button, lv_color_hex(0xf59e0b), 0);
    lv_obj_set_style_shadow_width(brightness_button, 0, 0);
    lv_obj_set_style_pad_all(brightness_button, 0, 0);
    lv_obj_add_event_cb(brightness_button, brightness_button_cb,
                        LV_EVENT_CLICKED, NULL);
    lv_obj_t *brightness_button_label = lv_label_create(brightness_button);
    lv_obj_set_style_text_font(brightness_button_label, &novel_font_16, 0);
    lv_obj_set_style_text_color(brightness_button_label, lv_color_white(), 0);
    lv_label_set_text(brightness_button_label, "亮度");
    lv_obj_center(brightness_button_label);

    lv_obj_t *paper = lv_obj_create(root);
    lv_obj_remove_style_all(paper);
    lv_obj_set_pos(paper, 6, 33);
    lv_obj_set_size(paper, 308, 174);
    lv_obj_set_style_radius(paper, 9, 0);
    lv_obj_set_style_bg_color(paper, lv_color_hex(0xfffdf7), 0);
    lv_obj_set_style_bg_opa(paper, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(paper, 1, 0);
    lv_obj_set_style_border_color(paper, lv_color_hex(0xd8cfbd), 0);
    lv_obj_remove_flag(paper, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    body_label = lv_label_create(paper);
    lv_obj_set_style_text_font(body_label, &novel_font_16, 0);
    lv_obj_set_style_text_color(body_label, lv_color_hex(0x26221c), 0);
    lv_obj_set_style_text_line_space(body_label, 2, 0);
    lv_label_set_long_mode(body_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(body_label, READER_MARGIN_X, 6);
    lv_obj_set_width(body_label, READER_BODY_WIDTH);
    lv_obj_set_height(body_label, READER_BODY_HEIGHT);

    page_label = lv_label_create(root);
    lv_obj_set_style_text_font(page_label, &novel_font_16, 0);
    lv_obj_set_style_text_color(page_label, lv_color_hex(0x64748b), 0);
    lv_obj_align(page_label, LV_ALIGN_BOTTOM_MID, 0, -5);
    lv_label_set_text(page_label, "");

    lv_obj_t *left_hint = lv_label_create(root);
    lv_obj_set_style_text_font(left_hint, &novel_font_16, 0);
    lv_obj_set_style_text_color(left_hint, lv_color_hex(0x94a3b8), 0);
    lv_label_set_text(left_hint, "<");
    lv_obj_align(left_hint, LV_ALIGN_BOTTOM_LEFT, 12, -5);

    lv_obj_t *right_hint = lv_label_create(root);
    lv_obj_set_style_text_font(right_hint, &novel_font_16, 0);
    lv_obj_set_style_text_color(right_hint, lv_color_hex(0x94a3b8), 0);
    lv_label_set_text(right_hint, ">");
    lv_obj_align(right_hint, LV_ALIGN_BOTTOM_RIGHT, -12, -5);

    lv_obj_t *old_screen = lv_screen_active();
    lv_screen_load(root);
    if (old_screen && old_screen != root) {
        lv_obj_delete_async(old_screen);
    }

    if (stream_timer) {
        lv_timer_delete(stream_timer);
        stream_timer = NULL;
    }

    if (active_source == READER_SOURCE_ONLINE && catalog_total > 0) {
        int initial = web_scraper_get_reading_chapter();
        uint32_t book_id = web_scraper_get_book_id();
        uint32_t restore_offset = 0;
        if (initial == bookshelf_progress(book_id)) {
            restore_offset = bookshelf_offset(book_id);
        }
        if (!open_stream_chapter(initial, false, restore_offset)) {
            waiting_chapter = initial;
            waiting_open_at_end = false;
            waiting_offset = restore_offset;
            web_scraper_set_reading_chapter(initial);
            char status[64];
            snprintf(status, sizeof(status), "第 %d 章正在加载...", initial);
            lv_label_set_text(page_label, status);
        }
        stream_timer = lv_timer_create(stream_poll_cb, 300, NULL);
    } else if (active_source == READER_SOURCE_SD_TEXT) {
        if (!open_sd_text()) {
            lv_label_set_text(body_label,
                              "无法打开 TF 卡中的 TXT\n请确认文件存在且为 UTF-8 编码");
            lv_label_set_text(page_label, "TF · 打开失败");
        }
        stream_timer = lv_timer_create(stream_poll_cb, 300, NULL);
    } else if (active_source == READER_SOURCE_SD_ARCHIVE &&
               chapter_count > 0) {
        int saved_chapter = 1;
        uint32_t saved_offset = 0;
        (void)sd_storage_load_progress(local_base_path, &saved_chapter,
                                       &saved_offset);
        int saved_index = 0;
        for (int i = 0; i < chapter_count; i++) {
            if (chapters[i].number == saved_chapter) {
                saved_index = i;
                break;
            }
        }
        if (chapters[saved_index].number != saved_chapter) {
            saved_offset = 0;
        }
        open_local_chapter(saved_index, false, saved_offset);
        stream_timer = lv_timer_create(stream_poll_cb, 300, NULL);
    } else if (chapter_count > 0) {
        open_local_chapter(0, false, 0);
    } else {
        update_display();
    }
}

void novel_reader_open_builtin(lv_display_t *disp)
{
    requested_source = READER_SOURCE_SPIFFS;
    requested_path[0] = '\0';
    requested_title[0] = '\0';
    novel_reader_ui_init(disp);
}

void novel_reader_open_sd_text(lv_display_t *disp, const char *path,
                               const char *title)
{
    if (!path || !path[0]) return;
    requested_source = READER_SOURCE_SD_TEXT;
    snprintf(requested_path, sizeof(requested_path), "%s", path);
    snprintf(requested_title, sizeof(requested_title), "%s",
             title && title[0] ? title : "TF 卡小说");
    novel_reader_ui_init(disp);
}

void novel_reader_open_sd_archive(lv_display_t *disp, const char *path,
                                  const char *title)
{
    if (!path || !path[0]) return;
    requested_source = READER_SOURCE_SD_ARCHIVE;
    snprintf(requested_path, sizeof(requested_path), "%s", path);
    snprintf(requested_title, sizeof(requested_title), "%s",
             title && title[0] ? title : "TF 卡小说");
    novel_reader_ui_init(disp);
}

void novel_reader_handle_touch(int16_t x, int16_t y, lv_indev_state_t state)
{
    if (state == LV_INDEV_STATE_RELEASED) {
        touch_held = false;
        return;
    }
    if (state != LV_INDEV_STATE_PRESSED) {
        return;
    }
    /* Let the header button and modal controls consume their own touch events;
     * neither should accidentally turn the page underneath. */
    if (brightness_overlay || y < 30) {
        return;
    }
    if (!body_label || page_count == 0) {
        return;
    }
    if (waiting_chapter > 0) {
        return;
    }

    /* Ignore repeated presses while the same touch is still held. */
    if (touch_held) {
        return;
    }
    touch_held = true;

    /* 1 second guard between actual page turns. */
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (now_ms - last_turn_ms < 300) {
        return;
    }

    bool turned = false;
    if (active_source == READER_SOURCE_SD_TEXT) {
        if (x >= 0 && x < 80) {
            if (sd_text_page > 0) {
                uint32_t previous = 0;
                if (sd_storage_page_offset(sd_text_index_key,
                                           sd_text_page - 1,
                                           &previous) == ESP_OK &&
                    load_sd_text_page(previous, sd_text_page - 1)) {
                    turned = true;
                }
            }
        } else if (sd_text_next_offset < sd_text_size) {
            uint32_t next_page = sd_text_page + 1;
            esp_err_t err = sd_storage_append_page_offset(
                sd_text_index_key, next_page, sd_text_next_offset);
            if (err == ESP_OK &&
                load_sd_text_page(sd_text_next_offset, next_page)) {
                turned = true;
            } else if (err != ESP_OK) {
                ESP_LOGW(TAG, "Cannot extend TF page index: %s",
                         esp_err_to_name(err));
            }
        }
        if (turned) {
            mark_reading_progress();
            last_turn_ms = now_ms;
        }
        return;
    }
    if (x >= 0 && x < 80) {
        /* left zone: previous */
        if (current_page > 0) {
            current_page--;
            update_display();
            mark_reading_progress();
            turned = true;
        } else if (catalog_total > 0 && current_chapter_number > 1) {
            request_stream_chapter_at(current_chapter_number - 1, true);
            turned = true;
        } else if (catalog_total <= 0 && current_chapter > 0) {
            open_local_chapter(current_chapter - 1, true, 0);
            turned = true;
        }
    } else {
        /* anywhere else: next page */
        if (current_page + 1 < page_count) {
            current_page++;
            update_display();
            mark_reading_progress();
            turned = true;
        } else if (catalog_total > 0 &&
                   current_chapter_number < catalog_total) {
            request_stream_chapter_at(current_chapter_number + 1, false);
            turned = true;
        } else if (catalog_total <= 0 &&
                   current_chapter + 1 < chapter_count) {
            open_local_chapter(current_chapter + 1, false, 0);
            turned = true;
        }
    }

    if (turned) {
        last_turn_ms = now_ms;
    }
}
