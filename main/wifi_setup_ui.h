#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void wifi_setup_ui_show(lv_display_t *disp);
void wifi_setup_ui_show_download(lv_display_t *disp);
void wifi_setup_debug_open_download(lv_display_t *disp);

#ifdef __cplusplus
}
#endif
