#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t touch_bitbang_init(void);
bool touch_bitbang_read(uint16_t *x, uint16_t *y);

#ifdef __cplusplus
}
#endif
