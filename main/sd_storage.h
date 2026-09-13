#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SD_STORAGE_MAX_BOOKS 16
#define SD_STORAGE_PATH_SIZE 192
#define SD_STORAGE_TITLE_SIZE 80

typedef enum {
    SD_BOOK_TEXT = 0,
    SD_BOOK_ARCHIVE,
} sd_book_type_t;

typedef struct {
    sd_book_type_t type;
    uint32_t book_id;
    int chapter_count;
    char title[SD_STORAGE_TITLE_SIZE];
    char path[SD_STORAGE_PATH_SIZE];
} sd_book_entry_t;

esp_err_t sd_storage_init(void);
bool sd_storage_ready(void);
esp_err_t sd_storage_list_books(sd_book_entry_t *out, size_t capacity,
                                size_t *out_count);
esp_err_t sd_storage_archive_chapter(uint32_t book_id, const char *title,
                                     int chapter_number,
                                     const char *source_path);

esp_err_t sd_storage_read(const char *path, uint32_t offset, void *buffer,
                          size_t capacity, size_t *out_read,
                          uint32_t *out_file_size);

uint32_t sd_storage_book_key(const char *path);
esp_err_t sd_storage_load_progress(const char *path, int *chapter,
                                   uint32_t *offset);
esp_err_t sd_storage_save_progress(const char *path, int chapter,
                                   uint32_t offset);

esp_err_t sd_storage_prepare_page_index(const char *path, uint32_t file_size,
                                        uint32_t *out_key,
                                        uint32_t *out_page_count);
esp_err_t sd_storage_page_offset(uint32_t key, uint32_t page,
                                 uint32_t *out_offset);
esp_err_t sd_storage_append_page_offset(uint32_t key, uint32_t page,
                                        uint32_t offset);
esp_err_t sd_storage_find_page(uint32_t key, uint32_t offset,
                               uint32_t *out_page,
                               uint32_t *out_offset);

#ifdef __cplusplus
}
#endif
