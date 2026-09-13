#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Configure PWM backlight control and restore the saved level from NVS. */
esp_err_t display_brightness_init(int gpio_num);

/* Apply a level immediately. Values are clamped to 10..100 percent. */
void display_brightness_set(int percent);
int display_brightness_get(void);

/* Persist the current level. Call on slider release, not on every movement. */
esp_err_t display_brightness_save(void);

#ifdef __cplusplus
}
#endif
