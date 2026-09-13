#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts a WiFi AP named ESP32-Reader and tries to download the first
 * max_chapters from the PC server (http://192.168.4.2:8000). */
esp_err_t wifi_loader_try_load_first_n(int max_chapters);

#ifdef __cplusplus
}
#endif
