/* SPDX-FileCopyrightText: 2025 */
/* SPDX-License-Identifier: MIT */

#include "bookshelf.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "nvs.h"

#define BOOKSHELF_MAGIC_V1 0x42534831u
#define BOOKSHELF_MAGIC    0x42534832u
#define NVS_NAMESPACE   "bookshelf"
#define NVS_KEY         "recent"

typedef struct {
    uint32_t magic;
    uint8_t count;
    uint8_t reserved[3];
    bookshelf_entry_t entries[BOOKSHELF_MAX_BOOKS];
} bookshelf_store_t;

typedef struct {
    uint32_t book_id;
    int32_t chapter;
    char title[BOOKSHELF_TITLE_SIZE];
} bookshelf_entry_v1_t;

typedef struct {
    uint32_t magic;
    uint8_t count;
    uint8_t reserved[3];
    bookshelf_entry_v1_t entries[BOOKSHELF_MAX_BOOKS];
} bookshelf_store_v1_t;

static bookshelf_store_t s_store;
static bool s_loaded;

static void load_once(void)
{
    if (s_loaded) {
        return;
    }
    memset(&s_store, 0, sizeof(s_store));
    s_store.magic = BOOKSHELF_MAGIC;
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        size_t size = 0;
        if (nvs_get_blob(handle, NVS_KEY, NULL, &size) == ESP_OK) {
            if (size == sizeof(bookshelf_store_t)) {
                bookshelf_store_t saved;
                if (nvs_get_blob(handle, NVS_KEY, &saved, &size) == ESP_OK &&
                    saved.magic == BOOKSHELF_MAGIC &&
                    saved.count <= BOOKSHELF_MAX_BOOKS) {
                    s_store = saved;
                }
            } else if (size == sizeof(bookshelf_store_v1_t)) {
                bookshelf_store_v1_t saved;
                if (nvs_get_blob(handle, NVS_KEY, &saved, &size) == ESP_OK &&
                    saved.magic == BOOKSHELF_MAGIC_V1 &&
                    saved.count <= BOOKSHELF_MAX_BOOKS) {
                    s_store.count = saved.count;
                    for (int i = 0; i < saved.count; i++) {
                        s_store.entries[i].book_id = saved.entries[i].book_id;
                        s_store.entries[i].chapter = saved.entries[i].chapter;
                        s_store.entries[i].offset = 0;
                        snprintf(s_store.entries[i].title,
                                 sizeof(s_store.entries[i].title), "%s",
                                 saved.entries[i].title);
                    }
                }
            }
        }
        nvs_close(handle);
    }
    s_loaded = true;
}

static esp_err_t save(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(handle, NVS_KEY, &s_store, sizeof(s_store));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

int bookshelf_get(bookshelf_entry_t *out, size_t capacity)
{
    load_once();
    size_t count = s_store.count;
    if (count > capacity) {
        count = capacity;
    }
    if (out && count > 0) {
        memcpy(out, s_store.entries, count * sizeof(out[0]));
    }
    return (int)count;
}

int bookshelf_progress(uint32_t book_id)
{
    load_once();
    for (int i = 0; i < s_store.count; i++) {
        if (s_store.entries[i].book_id == book_id) {
            return s_store.entries[i].chapter > 0 ?
                   s_store.entries[i].chapter : 1;
        }
    }
    return 1;
}

uint32_t bookshelf_offset(uint32_t book_id)
{
    load_once();
    for (int i = 0; i < s_store.count; i++) {
        if (s_store.entries[i].book_id == book_id) {
            return s_store.entries[i].offset;
        }
    }
    return 0;
}

esp_err_t bookshelf_update(uint32_t book_id, const char *title, int chapter,
                           uint32_t offset)
{
    if (book_id == 0 || !title || !title[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    load_once();
    if (chapter < 1) {
        chapter = 1;
    }

    int found = -1;
    for (int i = 0; i < s_store.count; i++) {
        if (s_store.entries[i].book_id == book_id) {
            found = i;
            break;
        }
    }

    bookshelf_entry_t entry = {
        .book_id = book_id,
        .chapter = chapter,
        .offset = offset,
    };
    snprintf(entry.title, sizeof(entry.title), "%s", title);

    int move_count;
    uint32_t evicted_book_id = 0;
    if (found >= 0) {
        move_count = found;
    } else {
        move_count = s_store.count;
        if (move_count >= BOOKSHELF_MAX_BOOKS) {
            evicted_book_id =
                s_store.entries[BOOKSHELF_MAX_BOOKS - 1].book_id;
            move_count = BOOKSHELF_MAX_BOOKS - 1;
        } else {
            s_store.count++;
        }
    }
    if (move_count > 0) {
        memmove(&s_store.entries[1], &s_store.entries[0],
                (size_t)move_count * sizeof(s_store.entries[0]));
    }
    s_store.entries[0] = entry;
    esp_err_t err = save();
    if (err == ESP_OK && evicted_book_id != 0 &&
        evicted_book_id != book_id) {
        char path[64];
        snprintf(path, sizeof(path), "/spiffs/catalog_%u.ids",
                 (unsigned)evicted_book_id);
        (void)remove(path);
    }
    return err;
}
