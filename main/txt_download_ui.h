#pragma once

#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void txt_download_ui_show(lv_display_t *disp);
bool txt_download_debug_search(const char *query);
bool txt_download_debug_book(const char *detail_path, const char *title,
                             int preferred_mirror);

#ifdef __cplusplus
}
#endif
