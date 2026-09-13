/* SPDX-FileCopyrightText: 2025 */
/* SPDX-License-Identifier: MIT */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "lvgl.h"
#include "novel_reader.h"
#include "web_scraper.h"
#include "wifi_setup_ui.h"
#include "book_select_ui.h"
#include "main_menu_ui.h"
#include "txt_download_ui.h"

static const char *TAG = "wifi_setup";

LV_FONT_DECLARE(novel_font_16);

#define NVS_NS "wifi_cfg"
#define KEY_SSID "ssid"
#define KEY_PASS "pass"

#define WIFI_GOT_IP_BIT      BIT0
#define WIFI_FAIL_BIT        BIT1
#define CONNECT_OP_DONE_BIT  BIT2
#define CONNECT_OP_OK_BIT    BIT3
#define WIFI_LINK_UP_BIT     BIT4
#define WIFI_CONNECT_TIMEOUT_MS 15000
#define WIFI_CONNECT_RETRIES 3

static lv_obj_t *ssid_ta = NULL;
static lv_obj_t *pass_ta = NULL;
static lv_obj_t *status_label = NULL;
static lv_obj_t *connect_btn = NULL;
static lv_obj_t *local_btn = NULL;
static lv_obj_t *keyboard = NULL;
static lv_obj_t *progress_bar = NULL;
static lv_obj_t *status_dot = NULL;
static bool connecting = false;
static bool download_started = false;
static bool open_download_after_connect = false;
static lv_timer_t *poll_timer = NULL;

static EventGroupHandle_t wifi_events = NULL;
static bool wifi_stack_ready = false;
static volatile bool wifi_auto_reconnect = false;
static volatile bool wifi_reconnect_task_running = false;

static char task_ssid[32] = "";
static char task_pass[64] = "";

static void keyboard_event_cb(lv_event_t *e);
static bool wifi_is_connected(void);
static void wifi_runtime_reconnect_task(void *arg);
static void delete_keyboard(void);

static void home_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    delete_keyboard();
    if (poll_timer) {
        lv_timer_delete(poll_timer);
        poll_timer = NULL;
    }
    status_label = NULL;
    connect_btn = NULL;
    local_btn = NULL;
    keyboard = NULL;
    progress_bar = NULL;
    status_dot = NULL;
    main_menu_ui_show(lv_display_get_default());
}

static void ui_set_status(const char *text, uint32_t color)
{
    if (status_label) {
        lv_label_set_text(status_label, text ? text : "");
        lv_obj_set_style_text_color(status_label, lv_color_hex(color), 0);
    }
    if (status_dot) {
        lv_obj_set_style_bg_color(status_dot, lv_color_hex(color), 0);
    }
}

static void ui_set_progress(int current, int total)
{
    if (!progress_bar) {
        return;
    }
    int value = (total > 0) ? (current * 100 / total) : 0;
    if (value < 0) value = 0;
    if (value > 100) value = 100;
    lv_bar_set_value(progress_bar, value, LV_ANIM_ON);
}

static void delete_keyboard(void)
{
    if (keyboard) {
        lv_obj_delete_async(keyboard);
        keyboard = NULL;
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi STA start, connecting...");
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *evt =
            (const wifi_event_sta_disconnected_t *)data;
        ESP_LOGW(TAG, "WiFi disconnected, reason=%d", evt ? evt->reason : -1);
        xEventGroupClearBits(wifi_events, WIFI_LINK_UP_BIT);
        xEventGroupSetBits(wifi_events, WIFI_FAIL_BIT);
        if (wifi_auto_reconnect && !wifi_reconnect_task_running) {
            wifi_reconnect_task_running = true;
            if (xTaskCreate(wifi_runtime_reconnect_task, "wifi_reconnect", 3072,
                            NULL, 4, NULL) != pdPASS) {
                wifi_reconnect_task_running = false;
                ESP_LOGE(TAG, "cannot create WiFi reconnect task");
            }
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&evt->ip_info.ip));
        wifi_auto_reconnect = true;
        xEventGroupSetBits(wifi_events, WIFI_GOT_IP_BIT | WIFI_LINK_UP_BIT);
    }
}

static void wifi_runtime_reconnect_task(void *arg)
{
    (void)arg;
    unsigned attempt = 0;
    while (wifi_auto_reconnect) {
        if (wifi_is_connected()) {
            attempt = 0;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        uint32_t delay_ms = 500u << (attempt < 4 ? attempt : 4);
        ESP_LOGI(TAG, "runtime reconnect attempt %u in %u ms",
                 attempt + 1, (unsigned)delay_ms);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
        if (!wifi_auto_reconnect) {
            break;
        }
        if (wifi_is_connected()) {
            continue;
        }
        xEventGroupClearBits(wifi_events, WIFI_GOT_IP_BIT | WIFI_FAIL_BIT);
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "runtime reconnect start failed: %s",
                     esp_err_to_name(err));
        } else {
            EventBits_t bits = xEventGroupWaitBits(
                wifi_events, WIFI_GOT_IP_BIT | WIFI_FAIL_BIT,
                pdTRUE, pdFALSE, pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));
            if (bits & WIFI_GOT_IP_BIT) {
                attempt = 0;
                continue;
            }
        }
        attempt++;
    }
    wifi_reconnect_task_running = false;
    vTaskDelete(NULL);
}

static void ensure_wifi_stack(void)
{
    if (wifi_stack_ready) {
        return;
    }

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, NULL));

    wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(wifi_events ? ESP_OK : ESP_ERR_NO_MEM);
    wifi_stack_ready = true;
}

static bool wifi_is_connected(void)
{
    return wifi_events &&
           (xEventGroupGetBits(wifi_events) & WIFI_LINK_UP_BIT) != 0;
}

static void nvs_save_credentials(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, KEY_SSID, ssid);
        nvs_set_str(h, KEY_PASS, pass);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "WiFi credentials saved");
    }
}

static void nvs_load_credentials(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz)
{
    ssid[0] = '\0';
    pass[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_str(h, KEY_SSID, ssid, &ssid_sz);
        nvs_get_str(h, KEY_PASS, pass, &pass_sz);
        nvs_close(h);
    }
}

static void wifi_connect_task(void *arg)
{
    (void)arg;

    /* Stop first so a delayed DISCONNECTED event cannot poison the new
     * attempt, then clear all transient result bits. */
    wifi_auto_reconnect = false;
    (void)esp_wifi_stop();
    xEventGroupClearBits(wifi_events,
                         WIFI_GOT_IP_BIT | WIFI_FAIL_BIT | WIFI_LINK_UP_BIT |
                         CONNECT_OP_DONE_BIT | CONNECT_OP_OK_BIT);

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set STA mode failed: %s", esp_err_to_name(err));
        xEventGroupSetBits(wifi_events, CONNECT_OP_DONE_BIT);
        vTaskDelete(NULL);
        return;
    }

    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid, task_ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, task_pass, sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK;

    err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err == ESP_OK) {
        err = esp_wifi_start();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi start failed: %s", esp_err_to_name(err));
        xEventGroupSetBits(wifi_events, CONNECT_OP_DONE_BIT);
        vTaskDelete(NULL);
        return;
    }

    bool connected_ok = false;
    for (int attempt = 1; attempt <= WIFI_CONNECT_RETRIES; attempt++) {
        EventBits_t bits = xEventGroupWaitBits(wifi_events,
                                               WIFI_GOT_IP_BIT | WIFI_FAIL_BIT,
                                               pdTRUE, pdFALSE,
                                               pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));
        if (bits & WIFI_GOT_IP_BIT) {
            connected_ok = true;
            break;
        }
        ESP_LOGW(TAG, "WiFi connect attempt %d/%d failed",
                 attempt, WIFI_CONNECT_RETRIES);
        if (attempt < WIFI_CONNECT_RETRIES) {
            vTaskDelay(pdMS_TO_TICKS(300));
            xEventGroupClearBits(wifi_events, WIFI_FAIL_BIT);
            (void)esp_wifi_connect();
        }
    }

    EventBits_t op_bits = CONNECT_OP_DONE_BIT;
    if (connected_ok) {
        nvs_save_credentials(task_ssid, task_pass);
        op_bits |= CONNECT_OP_OK_BIT;
    }
    xEventGroupSetBits(wifi_events, op_bits);
    vTaskDelete(NULL);
}

static void ta_event_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (!keyboard) {
            keyboard = lv_keyboard_create(lv_screen_active());
            /* Four compact key rows fit in 104px and leave both text fields
             * fully visible on the 240px-tall display. */
            lv_obj_set_size(keyboard, 320, 104);
            lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
            lv_obj_set_style_bg_color(keyboard, lv_color_hex(0xf7f9fc), 0);
            lv_obj_set_style_border_width(keyboard, 0, 0);
            lv_obj_add_event_cb(keyboard, keyboard_event_cb,
                                LV_EVENT_READY, NULL);
            lv_obj_add_event_cb(keyboard, keyboard_event_cb,
                                LV_EVENT_CANCEL, NULL);
        }
        lv_keyboard_set_textarea(keyboard, ta);
        lv_obj_move_foreground(keyboard);
    }
}

static void keyboard_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_READY || lv_event_get_code(e) == LV_EVENT_CANCEL) {
        delete_keyboard();
    }
}

static void connect_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (connecting || web_scraper_is_running()) {
        return;
    }

    delete_keyboard();

    /* If WiFi is still connected, re-running the scraper is enough. */
    if (wifi_is_connected()) {
        if (poll_timer) {
            lv_timer_delete(poll_timer);
            poll_timer = NULL;
        }
        if (open_download_after_connect) {
            txt_download_ui_show(lv_display_get_default());
        } else {
            book_select_ui_show(lv_display_get_default());
        }
        return;
    }

    const char *ssid = lv_textarea_get_text(ssid_ta);
    const char *pass = lv_textarea_get_text(pass_ta);
    if (!ssid || strlen(ssid) == 0) {
        ui_set_status("请输入 WiFi 名称", 0xdc2626);
        return;
    }

    connecting = true;
    lv_obj_add_state(connect_btn, LV_STATE_DISABLED);
    ui_set_progress(0, 0);
    ui_set_status("正在连接...", 0x2563eb);

    strncpy(task_ssid, ssid, sizeof(task_ssid) - 1);
    strncpy(task_pass, pass, sizeof(task_pass) - 1);
    task_ssid[sizeof(task_ssid) - 1] = '\0';
    task_pass[sizeof(task_pass) - 1] = '\0';

    ensure_wifi_stack();
    BaseType_t created = xTaskCreate(wifi_connect_task, "wifi_conn", 4096,
                                     NULL, 5, NULL);
    if (created != pdPASS) {
        connecting = false;
        lv_obj_remove_state(connect_btn, LV_STATE_DISABLED);
        ui_set_status("内存不足，无法创建连接任务", 0xdc2626);
    }
}

static void local_btn_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    if (web_scraper_is_running()) {
        ui_set_status("下载进行中，稍后可进入本地阅读", 0xd97706);
        return;
    }
    if (poll_timer) {
        lv_timer_delete(poll_timer);
        poll_timer = NULL;
    }
    wifi_auto_reconnect = false;
    (void)esp_wifi_stop();
    book_select_ui_show_local(lv_display_get_default());
}

static void poll_connect_cb(lv_timer_t *timer)
{
    /* WiFi connect phase finished?  Completion is signaled through the event
     * group so there is no cross-task plain-bool data race. */
    EventBits_t bits = xEventGroupGetBits(wifi_events);
    if (connecting && (bits & CONNECT_OP_DONE_BIT)) {
        bool ok = (bits & CONNECT_OP_OK_BIT) != 0;
        xEventGroupClearBits(wifi_events,
                             CONNECT_OP_DONE_BIT | CONNECT_OP_OK_BIT);
        connecting = false;
        lv_obj_remove_state(connect_btn, LV_STATE_DISABLED);
        if (ok) {
            ui_set_status("连接成功，进入选书", 0x16a34a);
            if (poll_timer) {
                lv_timer_delete(poll_timer);
                poll_timer = NULL;
            }
            if (open_download_after_connect) {
                txt_download_ui_show(lv_display_get_default());
            } else {
                book_select_ui_show(lv_display_get_default());
            }
        } else {
            ui_set_status("连接失败，请检查账号密码", 0xdc2626);
        }
        return;
    }

    /* Downloader progress. This timer stays alive on failure so the user can
     * tap the button again; it is only removed after a successful download. */
    if (download_started) {
        web_scraper_status_t st;
        web_scraper_get_status(&st);

        if (st.state == WEB_SCRAPER_GETTING_INDEX ||
            st.state == WEB_SCRAPER_DOWNLOADING) {
            lv_obj_add_state(connect_btn, LV_STATE_DISABLED);
            ui_set_progress(st.current, st.total);
            ui_set_status(st.message, 0x2563eb);
            return;
        }

        if (st.state == WEB_SCRAPER_READY ||
            st.state == WEB_SCRAPER_DONE) {
            download_started = false;
            ui_set_progress(100, 100);
            ui_set_status("当前章节已就绪，进入阅读器", 0x16a34a);
            if (poll_timer) {
                lv_timer_delete(poll_timer);
                poll_timer = NULL;
            }
            /* Keep WiFi alive: the scraper continues maintaining the
             * five-chapter window while the reader is open. */
            novel_reader_ui_init(lv_display_get_default());
            return;
        }

        if (st.state == WEB_SCRAPER_FAILED) {
            download_started = false;
            lv_obj_remove_state(connect_btn, LV_STATE_DISABLED);
            ui_set_status(st.message, 0xdc2626);
            return;
        }
    }
}

void wifi_setup_ui_show(lv_display_t *disp)
{
    open_download_after_connect = false;
    (void)disp;
    ensure_wifi_stack();

    char saved_ssid[32] = "";
    char saved_pass[64] = "";
    nvs_load_credentials(saved_ssid, sizeof(saved_ssid), saved_pass, sizeof(saved_pass));

    lv_obj_t *root = lv_obj_create(NULL);
    lv_obj_set_size(root, 320, 240);
    lv_obj_remove_style_all(root);
    lv_obj_set_style_bg_color(root, lv_color_hex(0xf2f5fa), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *header = lv_obj_create(root);
    lv_obj_remove_style_all(header);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_size(header, 320, 38);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x1d4ed8), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(header);
    lv_obj_set_style_text_font(title, &novel_font_16, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(title, "WiFi 设置");

    lv_obj_t *home = lv_button_create(header);
    lv_obj_set_size(home, 52, 28);
    lv_obj_align(home, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_set_style_radius(home, 8, 0);
    lv_obj_set_style_bg_color(home, lv_color_hex(0x0f3d8c), 0);
    lv_obj_add_event_cb(home, home_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *home_label = lv_label_create(home);
    lv_obj_set_style_text_font(home_label, &novel_font_16, 0);
    lv_obj_set_style_text_color(home_label, lv_color_white(), 0);
    lv_label_set_text(home_label, "主页");
    lv_obj_center(home_label);

    lv_obj_t *card = lv_obj_create(root);
    lv_obj_set_pos(card, 10, 44);
    lv_obj_set_size(card, 300, 104);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0xdbe3ef), 0);
    lv_obj_set_style_pad_all(card, 8, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ssid_label = lv_label_create(card);
    lv_obj_set_style_text_font(ssid_label, &novel_font_16, 0);
    lv_obj_set_style_text_color(ssid_label, lv_color_hex(0x475569), 0);
    lv_obj_set_pos(ssid_label, 0, 4);
    lv_label_set_text(ssid_label, "名称");

    ssid_ta = lv_textarea_create(card);
    lv_obj_set_style_text_font(ssid_ta, &novel_font_16, 0);
    lv_obj_set_pos(ssid_ta, 54, 0);
    lv_obj_set_size(ssid_ta, 228, 36);
    lv_obj_set_style_radius(ssid_ta, 8, 0);
    lv_obj_set_style_border_color(ssid_ta, lv_color_hex(0xcbd5e1), 0);
    lv_obj_set_style_bg_color(ssid_ta, lv_color_hex(0xf8fafc), 0);
    lv_obj_set_style_pad_left(ssid_ta, 8, 0);
    lv_obj_set_style_pad_top(ssid_ta, 7, 0);
    lv_textarea_set_one_line(ssid_ta, true);
    lv_textarea_set_placeholder_text(ssid_ta, "请输入 WiFi 名称");
    lv_textarea_set_max_length(ssid_ta, 31);
    lv_obj_add_event_cb(ssid_ta, ta_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *pass_label = lv_label_create(card);
    lv_obj_set_style_text_font(pass_label, &novel_font_16, 0);
    lv_obj_set_style_text_color(pass_label, lv_color_hex(0x475569), 0);
    lv_obj_set_pos(pass_label, 0, 51);
    lv_label_set_text(pass_label, "密码");

    pass_ta = lv_textarea_create(card);
    lv_obj_set_style_text_font(pass_ta, &novel_font_16, 0);
    lv_obj_set_pos(pass_ta, 54, 47);
    lv_obj_set_size(pass_ta, 228, 36);
    lv_obj_set_style_radius(pass_ta, 8, 0);
    lv_obj_set_style_border_color(pass_ta, lv_color_hex(0xcbd5e1), 0);
    lv_obj_set_style_bg_color(pass_ta, lv_color_hex(0xf8fafc), 0);
    lv_obj_set_style_pad_left(pass_ta, 8, 0);
    lv_obj_set_style_pad_top(pass_ta, 7, 0);
    lv_textarea_set_one_line(pass_ta, true);
    lv_textarea_set_password_mode(pass_ta, true);
    lv_textarea_set_placeholder_text(pass_ta, "请输入 WiFi 密码");
    lv_textarea_set_max_length(pass_ta, 63);
    lv_obj_add_event_cb(pass_ta, ta_event_cb, LV_EVENT_CLICKED, NULL);

    if (saved_ssid[0]) {
        lv_textarea_set_text(ssid_ta, saved_ssid);
    }
    if (saved_pass[0]) {
        lv_textarea_set_text(pass_ta, saved_pass);
    }

    connect_btn = lv_button_create(root);
    lv_obj_set_pos(connect_btn, 10, 155);
    lv_obj_set_size(connect_btn, 188, 36);
    lv_obj_set_style_radius(connect_btn, 9, 0);
    lv_obj_set_style_bg_color(connect_btn, lv_color_hex(0x2563eb), 0);
    lv_obj_add_event_cb(connect_btn, connect_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_label = lv_label_create(connect_btn);
    lv_obj_set_style_text_font(btn_label, &novel_font_16, 0);
    lv_obj_set_style_text_color(btn_label, lv_color_white(), 0);
    lv_label_set_text(btn_label, "连接并选书");
    lv_obj_center(btn_label);

    /* Always-available entry to the locally stored chapters (useful after a
     * download failure or without any network). */
    local_btn = lv_button_create(root);
    lv_obj_set_pos(local_btn, 204, 155);
    lv_obj_set_size(local_btn, 106, 36);
    lv_obj_set_style_radius(local_btn, 9, 0);
    lv_obj_set_style_bg_color(local_btn, lv_color_white(), 0);
    lv_obj_set_style_border_width(local_btn, 1, 0);
    lv_obj_set_style_border_color(local_btn, lv_color_hex(0x94a3b8), 0);
    lv_obj_add_event_cb(local_btn, local_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *local_label = lv_label_create(local_btn);
    lv_obj_set_style_text_font(local_label, &novel_font_16, 0);
    lv_obj_set_style_text_color(local_label, lv_color_hex(0x334155), 0);
    lv_label_set_text(local_label, "本地阅读");
    lv_obj_center(local_label);

    progress_bar = lv_bar_create(root);
    lv_obj_set_pos(progress_bar, 10, 197);
    lv_obj_set_size(progress_bar, 300, 5);
    lv_bar_set_range(progress_bar, 0, 100);
    lv_bar_set_value(progress_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(progress_bar, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(progress_bar, lv_color_hex(0xdbe3ef), LV_PART_MAIN);
    lv_obj_set_style_bg_color(progress_bar, lv_color_hex(0x2563eb),
                              LV_PART_INDICATOR);

    status_dot = lv_obj_create(root);
    lv_obj_remove_style_all(status_dot);
    lv_obj_set_pos(status_dot, 11, 212);
    lv_obj_set_size(status_dot, 8, 8);
    lv_obj_set_style_radius(status_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(status_dot, LV_OPA_COVER, 0);

    status_label = lv_label_create(root);
    lv_obj_set_style_text_font(status_label, &novel_font_16, 0);
    lv_obj_set_pos(status_label, 25, 207);
    lv_obj_set_width(status_label, 285);
    lv_label_set_long_mode(status_label, LV_LABEL_LONG_WRAP);
    ui_set_status(saved_ssid[0] ? "已保存 WiFi，点击连接" :
                                  "请输入 WiFi 账号和密码",
                  saved_ssid[0] ? 0x2563eb : 0x64748b);

    poll_timer = lv_timer_create(poll_connect_cb, 200, NULL);
    lv_obj_t *old_screen = lv_screen_active();
    lv_screen_load(root);
    if (old_screen && old_screen != root) {
        lv_obj_delete_async(old_screen);
    }
}

void wifi_setup_ui_show_download(lv_display_t *disp)
{
    wifi_setup_ui_show(disp);
    open_download_after_connect = true;
    if (status_label) {
        ui_set_status("连接 WiFi 后进入 TXT 下载", 0x2563eb);
    }
}

void wifi_setup_debug_open_download(lv_display_t *disp)
{
    wifi_setup_ui_show_download(disp);
    if (connect_btn) {
        lv_obj_send_event(connect_btn, LV_EVENT_CLICKED, NULL);
    }
}
