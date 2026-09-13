/* SPDX-FileCopyrightText: 2025 */
/* SPDX-License-Identifier: MIT */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/lock.h>
#include <sys/param.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "lvgl.h"

#include "esp_lcd_ili9341.h"
#include "novel_reader.h"
#include "wifi_setup_ui.h"
#include "main_menu_ui.h"
#include "display_brightness.h"
#include "touch_bitbang.h"
#include "sd_storage.h"
#include "txt_download_ui.h"

static const char *TAG = "novel_reader";

/* Board: ESP32-2432S028R / 2.8" ILI9341 + XPT2046 */
#define LCD_HOST   SPI2_HOST

#define LCD_H_RES        320
#define LCD_V_RES        240
#define LCD_PIXEL_CLOCK  (20 * 1000 * 1000)
#define LCD_CMD_BITS     8
#define LCD_PARAM_BITS   8

#define PIN_LCD_SCLK     14
#define PIN_LCD_MOSI     13
#define PIN_LCD_MISO     12
#define PIN_LCD_CS       15
#define PIN_LCD_DC       2
#define PIN_LCD_RST      -1
#define PIN_LCD_BL       21

#define LVGL_TICK_MS     2
#define LVGL_TASK_STACK_SIZE (6 * 1024)
#define LVGL_TASK_PRIORITY 2
#define LVGL_TASK_MAX_DELAY_MS 500
#define LVGL_TASK_MIN_DELAY_MS (1000 / CONFIG_FREERTOS_HZ)
#define LVGL_DRAW_BUF_LINES 20
#define TOUCH_CONFIRM_US (100 * 1000)

static _lock_t lvgl_api_lock;

/* XPT2046 panels can produce very short contact spikes.  Do not expose a
 * press to either LVGL or the reader until the same physical contact has
 * lasted for 100 ms.  A contact released before that point therefore never
 * becomes a click, which protects every control (including the keyboard and
 * its candidate panel) in one place. */
static bool touch_tracking;
static bool touch_confirmed;
static int64_t touch_started_us;
static lv_point_t touch_last_point;
static char serial_debug_line[224];
static size_t serial_debug_len;

static void serial_debug_run(char *line)
{
    if (strcmp(line, "TXTONLINE") == 0) {
        wifi_setup_debug_open_download(lv_display_get_default());
        ESP_LOGI(TAG, "serial TXTONLINE started");
        return;
    }
    if (strncmp(line, "TXTSEARCH ", 10) == 0) {
        bool ok = txt_download_debug_search(line + 10);
        ESP_LOGI(TAG, "serial TXTSEARCH %s", ok ? "started" : "rejected");
        return;
    }
    if (strncmp(line, "TXTBOOK ", 8) == 0) {
        char *path = line + 8;
        char *title = strchr(path, '|');
        char *source = title ? strchr(title + 1, '|') : NULL;
        if (!title || !source) {
            ESP_LOGW(TAG, "usage: TXTBOOK /path|title|1-or-2");
            return;
        }
        *title++ = '\0';
        *source++ = '\0';
        int mirror = atoi(source) == 2 ? 1 : 0;
        bool ok = txt_download_debug_book(path, title, mirror);
        ESP_LOGI(TAG, "serial TXTBOOK %s", ok ? "started" : "rejected");
        return;
    }
    ESP_LOGW(TAG, "unknown serial command: %s", line);
}

static void serial_debug_poll(void)
{
    uint8_t c;
    while (uart_read_bytes(UART_NUM_0, &c, 1, 0) == 1) {
        if (c == '\r' || c == '\n') {
            if (serial_debug_len > 0) {
                serial_debug_line[serial_debug_len] = '\0';
                serial_debug_run(serial_debug_line);
                serial_debug_len = 0;
            }
        } else if (c >= 0x20 && serial_debug_len + 1 <
                                      sizeof(serial_debug_line)) {
            serial_debug_line[serial_debug_len++] = (char)c;
        }
    }
}

static bool notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io,
                                    esp_lcd_panel_io_event_data_t *edata,
                                    void *user_ctx)
{
    lv_display_t *disp = (lv_display_t *)user_ctx;
    lv_display_flush_ready(disp);
    return false;
}

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)lv_display_get_user_data(disp);
    int x1 = area->x1;
    int x2 = area->x2;
    int y1 = area->y1;
    int y2 = area->y2;
    int w = x2 - x1 + 1;
    int h = y2 - y1 + 1;

    /* SPI LCD expects RGB565 in big-endian byte order */
    lv_draw_sw_rgb565_swap(px_map, w * h);
    esp_lcd_panel_draw_bitmap(panel, x1, y1, x2 + 1, y2 + 1, px_map);
}

static void lvgl_tick_cb(void *arg)
{
    lv_tick_inc(LVGL_TICK_MS);
}

static void lvgl_touch_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    uint16_t x[1] = {0};
    uint16_t y[1] = {0};
    bool pressed = touch_bitbang_read(x, y);

    if (pressed) {
        int64_t now_us = esp_timer_get_time();
        touch_last_point.x = x[0];
        touch_last_point.y = y[0];

        if (!touch_tracking) {
            touch_tracking = true;
            touch_confirmed = false;
            touch_started_us = now_us;
        }

        data->point = touch_last_point;
        if (!touch_confirmed && now_us - touch_started_us >= TOUCH_CONFIRM_US) {
            touch_confirmed = true;
        }

        if (touch_confirmed) {
            data->state = LV_INDEV_STATE_PRESSED;
            novel_reader_handle_touch(touch_last_point.x, touch_last_point.y,
                                      LV_INDEV_STATE_PRESSED);
        } else {
            data->state = LV_INDEV_STATE_RELEASED;
        }
        return;
    }

    data->point = touch_last_point;
    data->state = LV_INDEV_STATE_RELEASED;
    novel_reader_handle_touch(-1, -1, LV_INDEV_STATE_RELEASED);
    touch_tracking = false;
    touch_confirmed = false;
}

static void lvgl_port_task(void *arg)
{
    uint32_t time_till_next_ms = 0;
    while (1) {
        _lock_acquire(&lvgl_api_lock);
        time_till_next_ms = lv_timer_handler();
        _lock_release(&lvgl_api_lock);
        serial_debug_poll();
        time_till_next_ms = MAX(time_till_next_ms, LVGL_TASK_MIN_DELAY_MS);
        time_till_next_ms = MIN(time_till_next_ms, LVGL_TASK_MAX_DELAY_MS);
        usleep(1000 * time_till_next_ms);
    }
}

void app_main(void)
{
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    /* Backlight: keep off while starting, turn on after LVGL is ready */
    gpio_config_t bk_cfg = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << PIN_LCD_BL,
    };
    ESP_ERROR_CHECK(gpio_config(&bk_cfg));
    gpio_set_level(PIN_LCD_BL, 0);

    /* ---- Display SPI bus (HSPI) ---- */
    ESP_LOGI(TAG, "Init LCD SPI bus");
    spi_bus_config_t lcd_bus = {
        .sclk_io_num = PIN_LCD_SCLK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = PIN_LCD_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * LVGL_DRAW_BUF_LINES * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &lcd_bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t lcd_io = NULL;
    esp_lcd_panel_io_spi_config_t lcd_io_cfg = {
        .dc_gpio_num = PIN_LCD_DC,
        .cs_gpio_num = PIN_LCD_CS,
        .pclk_hz = LCD_PIXEL_CLOCK,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &lcd_io_cfg, &lcd_io));

    esp_lcd_panel_handle_t lcd_panel = NULL;
    esp_lcd_panel_dev_config_t lcd_cfg = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(lcd_io, &lcd_cfg, &lcd_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(lcd_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(lcd_panel));

    /* Landscaped display (as the shipped demo firmware does) */
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(lcd_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(lcd_panel, true, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(lcd_panel, true));

    /* The board routes LCD, touch and TF to three distinct pin groups but the
     * ESP32 has only two general SPI hosts. Touch uses a short GPIO transaction
     * so SPI3 remains available for the onboard TF slot. */
    ESP_LOGI(TAG, "Init bit-bang touch and TF card");
    ESP_ERROR_CHECK(touch_bitbang_init());
    (void)sd_storage_init();

    /* ---- LVGL ---- */
    ESP_LOGI(TAG, "Init LVGL");
    lv_init();

    lv_display_t *disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    size_t draw_buf_sz = LCD_H_RES * LVGL_DRAW_BUF_LINES * sizeof(lv_color16_t);
    void *buf1 = spi_bus_dma_memory_alloc(LCD_HOST, draw_buf_sz, 0);
    void *buf2 = spi_bus_dma_memory_alloc(LCD_HOST, draw_buf_sz, 0);
    assert(buf1 && buf2);

    lv_display_set_buffers(disp, buf1, buf2, draw_buf_sz, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_user_data(disp, lcd_panel);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, lvgl_flush_cb);

    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = notify_lvgl_flush_ready,
    };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(lcd_io, &cbs, disp));

    const esp_timer_create_args_t tick_args = {
        .callback = lvgl_tick_cb,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, LVGL_TICK_MS * 1000));

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(indev, disp);
    lv_indev_set_read_cb(indev, lvgl_touch_cb);

    /* Start at a stable three-way home screen. Network setup is entered only
     * for online reading or TXT download. */
    main_menu_ui_show(disp);

    /* WiFi UI initialization also makes NVS available. Restore the saved
     * brightness and turn the panel on through PWM only after the first screen
     * is ready, avoiding a full-brightness flash during boot. */
    ESP_ERROR_CHECK(display_brightness_init(PIN_LCD_BL));

    esp_err_t uart_err = uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0);
    if (uart_err == ESP_OK) {
        uart_flush_input(UART_NUM_0);
        ESP_LOGI(TAG, "Serial debug ready: TXTSEARCH / TXTBOOK");
    } else {
        ESP_LOGW(TAG, "Serial debug unavailable: %s", esp_err_to_name(uart_err));
    }

    xTaskCreate(lvgl_port_task, "lvgl", LVGL_TASK_STACK_SIZE, NULL, LVGL_TASK_PRIORITY, NULL);
    ESP_LOGI(TAG, "Main menu UI started");
}
