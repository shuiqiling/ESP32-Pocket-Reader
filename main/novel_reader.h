#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void novel_reader_ui_init(lv_display_t *disp);
void novel_reader_open_builtin(lv_display_t *disp);
void novel_reader_open_sd_text(lv_display_t *disp, const char *path,
                               const char *title);
void novel_reader_open_sd_archive(lv_display_t *disp, const char *path,
                                  const char *title);
void novel_reader_handle_touch(int16_t x, int16_t y, lv_indev_state_t state);

#ifdef __cplusplus
}
#endif
