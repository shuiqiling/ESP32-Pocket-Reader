/* SPDX-FileCopyrightText: 2025 */
/* SPDX-License-Identifier: MIT */

#include "display_brightness.h"

#include <stdint.h>

#include "driver/ledc.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "brightness";

#define BRIGHTNESS_DEFAULT 80
#define BRIGHTNESS_MIN     10
#define BRIGHTNESS_MAX     100
#define PWM_MAX_DUTY       1023
#define NVS_NAMESPACE      "display"
#define NVS_KEY_LEVEL      "brightness"

static int s_percent = BRIGHTNESS_DEFAULT;
static bool s_initialized;

static int clamp_percent(int percent)
{
    if (percent < BRIGHTNESS_MIN) {
        return BRIGHTNESS_MIN;
    }
    if (percent > BRIGHTNESS_MAX) {
        return BRIGHTNESS_MAX;
    }
    return percent;
}

void display_brightness_set(int percent)
{
    s_percent = clamp_percent(percent);
    if (!s_initialized) {
        return;
    }

    uint32_t duty = (uint32_t)s_percent * PWM_MAX_DUTY / BRIGHTNESS_MAX;
    esp_err_t err = ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    if (err == ESP_OK) {
        err = ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot set backlight duty: %s", esp_err_to_name(err));
    }
}

int display_brightness_get(void)
{
    return s_percent;
}

esp_err_t display_brightness_save(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(handle, NVS_KEY_LEVEL, (uint8_t)s_percent);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Saved brightness: %d%%", s_percent);
    }
    return err;
}

esp_err_t display_brightness_init(int gpio_num)
{
    uint8_t saved = BRIGHTNESS_DEFAULT;
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        if (nvs_get_u8(handle, NVS_KEY_LEVEL, &saved) != ESP_OK) {
            saved = BRIGHTNESS_DEFAULT;
        }
        nvs_close(handle);
    }
    s_percent = clamp_percent(saved);

    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer);
    if (err != ESP_OK) {
        return err;
    }

    const ledc_channel_config_t channel = {
        .gpio_num = gpio_num,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    err = ledc_channel_config(&channel);
    if (err != ESP_OK) {
        return err;
    }

    s_initialized = true;
    display_brightness_set(s_percent);
    ESP_LOGI(TAG, "Backlight PWM ready at %d%%", s_percent);
    return ESP_OK;
}
