/* SPDX-License-Identifier: MIT */

#include "touch_bitbang.h"

#include "driver/gpio.h"
#include "esp_rom_sys.h"

#define PIN_TOUCH_CLK   25
#define PIN_TOUCH_CS    33
#define PIN_TOUCH_MOSI  32
#define PIN_TOUCH_MISO  39
#define PIN_TOUCH_IRQ   36

#define XPT_CMD_Z1 0xB1
#define XPT_CMD_Z2 0xC1
#define XPT_CMD_Y  0x91
#define XPT_CMD_X  0xD1
#define XPT_ADC_MAX 4096
#define XPT_Z_THRESHOLD 100
#define TOUCH_SAMPLES 5

static inline void half_cycle(void)
{
    esp_rom_delay_us(1);
}

static uint16_t read_adc(uint8_t command)
{
    uint16_t value = 0;
    gpio_set_level(PIN_TOUCH_CS, 0);
    for (int bit = 7; bit >= 0; bit--) {
        gpio_set_level(PIN_TOUCH_MOSI, (command >> bit) & 1);
        half_cycle();
        gpio_set_level(PIN_TOUCH_CLK, 1);
        half_cycle();
        gpio_set_level(PIN_TOUCH_CLK, 0);
    }
    for (int bit = 0; bit < 16; bit++) {
        half_cycle();
        gpio_set_level(PIN_TOUCH_CLK, 1);
        value = (uint16_t)((value << 1) | gpio_get_level(PIN_TOUCH_MISO));
        half_cycle();
        gpio_set_level(PIN_TOUCH_CLK, 0);
    }
    gpio_set_level(PIN_TOUCH_CS, 1);
    gpio_set_level(PIN_TOUCH_MOSI, 0);
    return value >> 3;
}

esp_err_t touch_bitbang_init(void)
{
    gpio_config_t outputs = {
        .pin_bit_mask = (1ULL << PIN_TOUCH_CLK) |
                        (1ULL << PIN_TOUCH_CS) |
                        (1ULL << PIN_TOUCH_MOSI),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&outputs);
    if (err != ESP_OK) {
        return err;
    }
    gpio_config_t inputs = {
        .pin_bit_mask = (1ULL << PIN_TOUCH_MISO) |
                        (1ULL << PIN_TOUCH_IRQ),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&inputs);
    if (err != ESP_OK) {
        return err;
    }
    gpio_set_level(PIN_TOUCH_CS, 1);
    gpio_set_level(PIN_TOUCH_CLK, 0);
    gpio_set_level(PIN_TOUCH_MOSI, 0);
    return ESP_OK;
}

bool touch_bitbang_read(uint16_t *x, uint16_t *y)
{
    if (!x || !y) {
        return false;
    }

    /* The board previously used the XPT2046 driver in polling mode. Its
     * commands keep the ADC powered (PD=01), which disables PENIRQ, so IRQ
     * cannot be used as a gate here. Poll Z1/Z2 just like the former driver. */
    uint16_t z1 = read_adc(XPT_CMD_Z1);
    uint16_t z2 = read_adc(XPT_CMD_Z2);
    uint16_t pressure = (uint16_t)(z1 + XPT_ADC_MAX - z2);
    if (pressure < XPT_Z_THRESHOLD) {
        return false;
    }

    (void)read_adc(XPT_CMD_X);
    uint32_t raw_x = 0;
    uint32_t raw_y = 0;
    unsigned valid = 0;
    for (unsigned i = 0; i < TOUCH_SAMPLES; i++) {
        uint16_t sample_x = read_adc(XPT_CMD_X);
        uint16_t sample_y = read_adc(XPT_CMD_Y);
        if (sample_x >= 50 && sample_x <= XPT_ADC_MAX - 50 &&
            sample_y >= 50 && sample_y <= XPT_ADC_MAX - 50) {
            raw_x += sample_x;
            raw_y += sample_y;
            valid++;
        }
    }
    if (valid < TOUCH_SAMPLES / 2) {
        return false;
    }
    raw_x /= valid;
    raw_y /= valid;

    /* Match the former driver configuration: native 240x320, mirror both
     * axes, then swap for the 320x240 landscape display. */
    uint32_t native_x = raw_x * 240u / XPT_ADC_MAX;
    uint32_t native_y = raw_y * 320u / XPT_ADC_MAX;
    uint32_t screen_x = 320u - native_y;
    uint32_t screen_y = 240u - native_x;
    *x = (uint16_t)(screen_x < 320u ? screen_x : 319u);
    *y = (uint16_t)(screen_y < 240u ? screen_y : 239u);
    return true;
}
