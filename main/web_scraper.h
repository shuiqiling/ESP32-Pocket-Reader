/* SPDX-FileCopyrightText: 2025 */
/* SPDX-License-Identifier: MIT */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WEB_SCRAPER_IDLE = 0,
    WEB_SCRAPER_GETTING_INDEX,
    WEB_SCRAPER_DOWNLOADING,
    WEB_SCRAPER_READY,
    WEB_SCRAPER_DONE,
    WEB_SCRAPER_FAILED,
} web_scraper_state_t;

typedef struct {
    web_scraper_state_t state;
    int total;       /* number of chapters discovered            */
    int current;     /* 1-based chapter currently being fetched  */
    char message[160];
} web_scraper_status_t;

#define WEB_SCRAPER_SEARCH_MAX 8
typedef enum {
    WEB_SEARCH_IDLE = 0,
    WEB_SEARCH_RUNNING,
    WEB_SEARCH_READY,
    WEB_SEARCH_FAILED,
} web_search_state_t;

typedef struct {
    uint32_t book_id;
    char title[96];
    char author[48];
} web_search_result_t;

typedef struct {
    web_search_state_t state;
    int count;
    char message[96];
    web_search_result_t results[WEB_SCRAPER_SEARCH_MAX];
} web_search_status_t;

/*
 * Starts the background downloader. Safe to call from the LVGL task; the
 * actual HTTP/SPIFFS work runs on a dedicated FreeRTOS task and never calls
 * LVGL. Returns ESP_ERR_INVALID_STATE if a download is already running.
 */
esp_err_t web_scraper_start(void);

/* Starts a bounded background search against the online catalog. */
esp_err_t web_scraper_search(const char *query);
esp_err_t web_scraper_get_search_status(web_search_status_t *out);

/* Selects one search result before web_scraper_start(). */
esp_err_t web_scraper_select_book(uint32_t book_id, const char *title);
esp_err_t web_scraper_select_book_at(uint32_t book_id, const char *title,
                                     int chapter_number);
uint32_t web_scraper_get_book_id(void);
void web_scraper_get_book_title(char *out, size_t cap);
bool web_scraper_catalog_complete(void);

bool web_scraper_is_running(void);
/* Cooperative stop: an in-flight HTTP request may take up to its timeout. */
void web_scraper_request_stop(void);

/* Streaming-cache control. The downloader keeps a five-chapter window around
 * the current reading position. These functions are safe from the LVGL task. */
int web_scraper_get_catalog_size(void);
void web_scraper_set_reading_chapter(int chapter_number);
int web_scraper_get_reading_chapter(void);

/* Thread-safe read-only snapshot of the downloader progress. */
esp_err_t web_scraper_get_status(web_scraper_status_t *out);

const char *web_scraper_state_str(web_scraper_state_t s);

#ifdef __cplusplus
}
#endif
