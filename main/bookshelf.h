#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BOOKSHELF_MAX_BOOKS 6
#define BOOKSHELF_TITLE_SIZE 80

typedef struct {
    uint32_t book_id;
    int32_t chapter;
    uint32_t offset;
    char title[BOOKSHELF_TITLE_SIZE];
} bookshelf_entry_t;

/* Returns newest-first reading history. */
int bookshelf_get(bookshelf_entry_t *out, size_t capacity);

/* Adds/moves a book to the front and updates its reading position. */
esp_err_t bookshelf_update(uint32_t book_id, const char *title, int chapter,
                           uint32_t offset);

int bookshelf_progress(uint32_t book_id);
uint32_t bookshelf_offset(uint32_t book_id);

#ifdef __cplusplus
}
#endif
