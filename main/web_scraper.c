/* SPDX-FileCopyrightText: 2025 */
/* SPDX-License-Identifier: MIT */

#include "web_scraper.h"
#include "web_html.h"
#include "chapter_cache.h"
#include "sd_storage.h"

#include <dirent.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "miniz.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "web_scraper";

#define BASE_SCHEME_HOST  "http://m.zhaobiquge.com"
#define INDEX_PATH        "/biquge/2/2031/index.html"
#define INDEX_PAGE_PATH   "/biquge/2/2031/index_%d.html"
#define SEARCH_SCHEME_HOST "http://www.biqukong.com"
#define SEARCH_FALLBACK_HOST "http://www.biquguo.com"
#define SEARCH_FALLBACK2_HOST "http://www.biqugeww.com"
#define LEGACY_CATALOG_ID_PATH "/spiffs/catalog.ids"
#define CACHE_OWNER_PATH "/spiffs/cache_owner.id"
#define CACHE_OWNER_TMP_PATH "/spiffs/.cache_owner.tmp"

#define HTTP_RESPONSE_CAP (40 * 1024)
#define HTTP_PARSE_WINDOW (22 * 1024)
#define HTTP_RX_INITIAL    2048
#define DECOMPRESS_CAP    (48 * 1024)
#define CHAPTER_TEXT_CAP  (32 * 1024)
#define MAX_PAGE_PARTS    8
#define HTTP_TIMEOUT_MS   20000
#define TASK_STACK_BYTES  (12 * 1024)

static SemaphoreHandle_t s_mutex = NULL;
static bool s_running = false;
static bool s_stop_requested = false;
static bool s_reader_ready = false;
static int s_catalog_count = 0;
static int s_reading_chapter = 1;
static web_html_chapter_t s_catalog_page[WEB_HTML_INDEX_PAGE_SIZE];
static int s_loaded_catalog_page = 0;
static int s_loaded_catalog_count = 0;
static web_scraper_status_t s_status = { WEB_SCRAPER_IDLE, 0, 0, "" };
static web_search_status_t s_search = { WEB_SEARCH_IDLE, 0, "", {{0}} };
static bool s_search_running = false;
static uint32_t s_selected_book_id = 0;
static uint32_t s_cached_book_id = 0;
static int s_selected_start_chapter = 1;
static char s_catalog_id_path[64] = LEGACY_CATALOG_ID_PATH;
static char s_selected_book_title[WEB_HTML_TITLE_SIZE] = "斗罗大陆";
static bool s_catalog_complete = true;

/* ------------------------------------------------------------------ */
/* status helpers                                                      */
/* ------------------------------------------------------------------ */

static void lock(void)
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
    }
    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if (s_mutex) {
        xSemaphoreGive(s_mutex);
    }
}

static void set_state(web_scraper_state_t state, const char *fmt, ...)
{
    char msg[160];
    msg[0] = '\0';
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    lock();
    s_status.state = state;
    if (state == WEB_SCRAPER_READY) {
        s_reader_ready = true;
    }
    snprintf(s_status.message, sizeof(s_status.message), "%s", msg);
    unlock();
}

static void set_progress(int current, int total, const char *msg)
{
    lock();
    s_status.state = s_reader_ready ? WEB_SCRAPER_READY
                                    : WEB_SCRAPER_DOWNLOADING;
    s_status.current = current;
    s_status.total = total;
    snprintf(s_status.message, sizeof(s_status.message), "%s", msg);
    unlock();
}

bool web_scraper_is_running(void)
{
    bool r;
    lock();
    r = s_running;
    unlock();
    return r;
}

static bool stop_requested(void)
{
    bool stop;
    lock();
    stop = s_stop_requested;
    unlock();
    return stop;
}

void web_scraper_request_stop(void)
{
    lock();
    s_stop_requested = true;
    unlock();
}

int web_scraper_get_catalog_size(void)
{
    int count;
    lock();
    count = s_catalog_count;
    unlock();
    return count;
}

void web_scraper_set_reading_chapter(int chapter_number)
{
    lock();
    if (chapter_number < 1) {
        chapter_number = 1;
    }
    if (s_catalog_count > 0 && chapter_number > s_catalog_count) {
        chapter_number = s_catalog_count;
    }
    s_reading_chapter = chapter_number;
    unlock();
}

int web_scraper_get_reading_chapter(void)
{
    int chapter;
    lock();
    chapter = s_reading_chapter;
    unlock();
    return chapter;
}

bool web_scraper_catalog_complete(void)
{
    bool complete;
    lock();
    complete = s_catalog_complete;
    unlock();
    return complete;
}

void web_scraper_get_book_title(char *out, size_t cap)
{
    if (!out || cap == 0) {
        return;
    }
    lock();
    snprintf(out, cap, "%s", s_selected_book_title);
    unlock();
}

uint32_t web_scraper_get_book_id(void)
{
    uint32_t id;
    lock();
    id = s_selected_book_id;
    unlock();
    return id;
}

esp_err_t web_scraper_select_book(uint32_t book_id, const char *title)
{
    return web_scraper_select_book_at(book_id, title, 1);
}

esp_err_t web_scraper_select_book_at(uint32_t book_id, const char *title,
                                     int chapter_number)
{
    if (book_id == 0 || !title || !title[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    if (s_running) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    s_selected_book_id = book_id;
    s_selected_start_chapter = chapter_number > 0 ? chapter_number : 1;
    snprintf(s_selected_book_title, sizeof(s_selected_book_title), "%s", title);
    snprintf(s_catalog_id_path, sizeof(s_catalog_id_path),
             "/spiffs/catalog_%u.ids", (unsigned)book_id);
    s_catalog_complete = false;
    unlock();
    return ESP_OK;
}

esp_err_t web_scraper_get_search_status(web_search_status_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    *out = s_search;
    unlock();
    return ESP_OK;
}

esp_err_t web_scraper_get_status(web_scraper_status_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    *out = s_status;
    unlock();
    return ESP_OK;
}

const char *web_scraper_state_str(web_scraper_state_t s)
{
    switch (s) {
    case WEB_SCRAPER_IDLE: return "空闲";
    case WEB_SCRAPER_GETTING_INDEX: return "正在获取目录";
    case WEB_SCRAPER_DOWNLOADING: return "正在下载";
    case WEB_SCRAPER_READY: return "可以阅读";
    case WEB_SCRAPER_DONE: return "下载完成";
    case WEB_SCRAPER_FAILED: return "下载失败";
    default: return "未知";
    }
}

/* ------------------------------------------------------------------ */
/* SPIFFS                                                              */
/* ------------------------------------------------------------------ */

static bool ensure_spiffs(void)
{
    if (esp_spiffs_mounted("spiffs")) {
        return true;
    }
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "spiffs",
        .max_files = 6,
        .format_if_mount_failed = false,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "SPIFFS mounted");
    return true;
}

/* Writes atomically through a temporary file then rename() over the target.
 *
 * Power-loss recovery state machine (per chapter):
 *  A) final exists, .bak absent            -> normal old chapter
 *  B) final MISSING, .bak exists           -> crash happened between
 *     final->bak and tmp->final. .bak is the ONLY copy; restore it first.
 *  C) final exists AND .bak exists         -> new final was installed but
 *     cleanup was interrupted; .bak is a stale duplicate, safe to remove.
 *  D) final missing, .bak missing          -> first install, nothing to restore.
 * tmp is always only a staging file.
 */
typedef struct {
    char final_path[64];
    char tmp_path[64];
    char bak_path[64];
    FILE *file;
    bool had_old;
    size_t bytes;
} chapter_writer_t;

static void chapter_writer_abort(chapter_writer_t *writer)
{
    if (!writer) {
        return;
    }
    if (writer->file) {
        fclose(writer->file);
        writer->file = NULL;
    }
    if (writer->tmp_path[0]) {
        remove(writer->tmp_path);
    }
}

static bool chapter_writer_begin(chapter_writer_t *writer, int index,
                                 const char *title)
{
    if (!writer || !title) {
        return false;
    }
    memset(writer, 0, sizeof(*writer));
    snprintf(writer->final_path, sizeof(writer->final_path),
             "/spiffs/cache_%03d.txt", index);
    snprintf(writer->tmp_path, sizeof(writer->tmp_path),
             "/spiffs/.cache_%03d.tmp", index);
    snprintf(writer->bak_path, sizeof(writer->bak_path),
             "/spiffs/.cache_%03d.bak", index);

    /* ---- recovery before writing anything new ---- */
    struct stat st;
    bool final_exists = (stat(writer->final_path, &st) == 0);
    bool bak_exists = (stat(writer->bak_path, &st) == 0);
    if (!final_exists && bak_exists) {
        if (rename(writer->bak_path, writer->final_path) != 0) {
            ESP_LOGE(TAG, "recovery failed: %s -> %s", writer->bak_path,
                     writer->final_path);
            return false; /* .bak is the only copy, never delete it here */
        }
        final_exists = true;
        bak_exists = false;
        ESP_LOGW(TAG, "recovered %s from %s", writer->final_path,
                 writer->bak_path);
    }
    if (bak_exists) {
        /* final exists too: bak is stale; safe to clean after recovery. */
        remove(writer->bak_path);
    }
    writer->had_old = final_exists;

    writer->file = fopen(writer->tmp_path, "wb");
    if (!writer->file) {
        ESP_LOGE(TAG, "open tmp failed: %s", writer->tmp_path);
        return false;
    }
    size_t title_len = strlen(title);
    if (fwrite(title, 1, title_len, writer->file) != title_len ||
        fwrite("\n", 1, 1, writer->file) != 1) {
        chapter_writer_abort(writer);
        return false;
    }
    writer->bytes = title_len + 1;
    return true;
}

static bool chapter_writer_append(chapter_writer_t *writer, const char *body,
                                  size_t body_len, bool leading_newline)
{
    if (!writer || !writer->file || !body || body_len == 0) {
        return false;
    }
    if (leading_newline) {
        if (fwrite("\n", 1, 1, writer->file) != 1) {
            return false;
        }
        writer->bytes++;
    }
    if (fwrite(body, 1, body_len, writer->file) != body_len) {
        return false;
    }
    writer->bytes += body_len;
    return true;
}

static bool chapter_writer_finish(chapter_writer_t *writer)
{
    if (!writer || !writer->file) {
        return false;
    }
    bool ok = fflush(writer->file) == 0 && fsync(fileno(writer->file)) == 0;
    if (fclose(writer->file) != 0) {
        ok = false;
    }
    writer->file = NULL;
    if (!ok) {
        remove(writer->tmp_path);
        ESP_LOGE(TAG, "temporary chapter write failed: %s", writer->tmp_path);
        return false;
    }

    /* ---- two-phase replace with rollback ---- */
    if (writer->had_old) {
        if (rename(writer->final_path, writer->bak_path) != 0) {
            ESP_LOGE(TAG, "backup rename failed: %s -> %s",
                     writer->final_path, writer->bak_path);
            remove(writer->tmp_path);
            return false;
        }
    }

    if (rename(writer->tmp_path, writer->final_path) != 0) {
        ESP_LOGE(TAG, "install rename failed: %s -> %s", writer->tmp_path,
                 writer->final_path);
        if (writer->had_old) {
            if (rename(writer->bak_path, writer->final_path) != 0) {
                ESP_LOGE(TAG, "ROLLBACK FAILED: cannot restore %s from %s",
                         writer->final_path, writer->bak_path);
            }
        }
        remove(writer->tmp_path);
        return false;
    }

    remove(writer->bak_path); /* success: clean up the backup copy */
    remove(writer->tmp_path);
    ESP_LOGI(TAG, "saved %s (%u bytes)", writer->final_path,
             (unsigned)writer->bytes);
    return true;
}

static bool write_chapter_atomic(int index, const char *title,
                                 const char *body, size_t body_len)
{
    chapter_writer_t writer;
    if (!chapter_writer_begin(&writer, index, title)) {
        return false;
    }
    if (body && body_len > 0 &&
        !chapter_writer_append(&writer, body, body_len, false)) {
        chapter_writer_abort(&writer);
        return false;
    }
    return chapter_writer_finish(&writer);
}

/* ------------------------------------------------------------------ */
/* HTTP helpers                                                        */
/* ------------------------------------------------------------------ */

/* Parse a gzip header and point *raw_start / *raw_len at the raw DEFLATE
 * stream. Only DEFLATE (method 8) is accepted. */
static uint32_t read_le32(const unsigned char *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static bool gzip_raw_offset(const unsigned char *d, size_t len,
                            const unsigned char **raw_start, size_t *raw_len,
                            uint32_t *expected_crc, uint32_t *expected_size)
{
    if (len < 18 || d[0] != 0x1f || d[1] != 0x8b || d[2] != 8) {
        return false;
    }
    unsigned flg = d[3];
    if (flg & 0xe0) { /* reserved gzip flag bits */
        return false;
    }
    size_t p = 10;
    if (flg & 0x04) { /* FEXTRA */
        if (p + 2 > len) return false;
        size_t xlen = (size_t)d[p] | ((size_t)d[p + 1] << 8);
        p += 2;
        if (p + xlen > len) return false;
        p += xlen;
    }
    if (flg & 0x08) { /* FNAME */
        while (p < len && d[p]) p++;
        if (p >= len) return false;
        p++;
    }
    if (flg & 0x10) { /* FCOMMENT */
        while (p < len && d[p]) p++;
        if (p >= len) return false;
        p++;
    }
    if (flg & 0x02) { /* FHCRC */
        if (p + 2 > len) return false;
        p += 2;
    }
    if (p + 8 > len) {
        return false;
    }
    *raw_start = d + p;
    *raw_len = len - p - 8; /* exclude CRC32 + ISIZE trailer */
    *expected_crc = read_le32(d + len - 8);
    *expected_size = read_le32(d + len - 4);
    return true;
}

static void log_heap_error(const char *operation, size_t wanted)
{
    ESP_LOGE(TAG, "%s: cannot allocate %u bytes (free=%u, largest=%u)",
             operation, (unsigned)wanted,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

/* Fetch one URL into a caller supplied buffer. When the response is gzip
 * encoded it is transparently decoded. Returns ESP_OK and sets out buffer
 * ownership:
 *   - out_buf points at *heap_buf when *owned_by_caller is true.
 * The helper always returns a freshly allocated heap block (for the plain
 * case it is the response buffer; for gzip it is the decoded buffer and the
 * original response buffer is freed before returning). */
static esp_err_t http_get_page_once_buffered(const char *url,
                                             char **out_buf, size_t *out_len)
{
    if (!url || !out_buf || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_buf = NULL;
    *out_len = 0;

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .user_agent = "Mozilla/5.0 (Linux; Android 10) AppleWebKit/537.36 "
                      "Chrome/120.0 Mobile Safari/537.36",
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "http client init failed: %s", url);
        return ESP_ERR_NO_MEM;
    }
    /* The target currently returns gzip even when identity is requested. Ask
     * for gzip explicitly so the wire representation stays small. */
    esp_http_client_set_header(client, "Accept-Encoding", "gzip");
    esp_http_client_set_header(client, "Accept",
                               "text/html,application/xhtml+xml,*/*;q=0.8");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open failed (%s): %s", esp_err_to_name(err), url);
        esp_http_client_cleanup(client);
        return err;
    }

    int64_t header_len = esp_http_client_fetch_headers(client);
    if (header_len < 0) {
        ESP_LOGE(TAG, "fetch headers failed (%lld): %s",
                 (long long)header_len, url);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return header_len == -ESP_ERR_HTTP_EAGAIN ? ESP_ERR_TIMEOUT : ESP_FAIL;
    }
    int status = esp_http_client_get_status_code(client);
    bool chunked = esp_http_client_is_chunked_response(client);
    ESP_LOGI(TAG, "HTTP %d, content_length=%lld, chunked=%d, free=%u, largest=%u",
             status, (long long)header_len, chunked,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    if (status != 200) {
        ESP_LOGE(TAG, "HTTP status %d for %s", status, url);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_HTTP_CONNECT;
    }

    if (header_len > HTTP_RESPONSE_CAP) {
        ESP_LOGE(TAG, "response Content-Length exceeds %u bytes",
                 (unsigned)HTTP_RESPONSE_CAP);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_SIZE;
    }

    /* Start small and grow only when bytes actually arrive. The previous
     * fixed 24KB allocation happened while TLS was still live and exhausted
     * the largest contiguous block on classic ESP32. */
    size_t capacity = HTTP_RX_INITIAL;
    if (header_len > 0 && (size_t)header_len < capacity) {
        capacity = (size_t)header_len;
    }
    if (capacity == 0) {
        capacity = 1;
    }
    char *resp = malloc(capacity + 1);
    if (!resp) {
        log_heap_error("HTTP receive buffer", capacity + 1);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    size_t got = 0;
    esp_err_t read_err = ESP_OK;
    while (true) {
        if (got == capacity) {
            if (esp_http_client_is_complete_data_received(client)) {
                break;
            }
            if (capacity >= HTTP_RESPONSE_CAP) {
                read_err = ESP_ERR_INVALID_SIZE;
                break;
            }
            size_t next = capacity * 2;
            if (next > HTTP_RESPONSE_CAP) {
                next = HTTP_RESPONSE_CAP;
            }
            char *grown = realloc(resp, next + 1);
            if (!grown) {
                log_heap_error("HTTP receive buffer growth", next + 1);
                read_err = ESP_ERR_NO_MEM;
                break;
            }
            resp = grown;
            capacity = next;
        }

        int rd = esp_http_client_read(client, resp + got,
                                      (int)(capacity - got));
        if (rd < 0) {
            read_err = rd == -ESP_ERR_HTTP_EAGAIN ? ESP_ERR_TIMEOUT : ESP_FAIL;
            ESP_LOGE(TAG, "response read failed (%d) after %u bytes",
                     rd, (unsigned)got);
            break;
        }
        if (rd == 0) {
            break;
        }
        got += (size_t)rd;
    }
    bool complete = esp_http_client_is_complete_data_received(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "HTTP body: %u bytes, complete=%d, capacity=%u",
             (unsigned)got, complete, (unsigned)capacity);
    if (!heap_caps_check_integrity_all(true)) {
        ESP_LOGE(TAG, "heap corruption detected after HTTP receive");
        return ESP_FAIL;
    }
    if (read_err != ESP_OK || !complete) {
        if (read_err == ESP_OK) {
            read_err = ESP_ERR_INVALID_SIZE;
            ESP_LOGE(TAG, "incomplete HTTP response (%u bytes)",
                     (unsigned)got);
        }
        free(resp);
        return read_err;
    }
    resp[got] = '\0';

    /* Release unused growth slack before allocating the decoded page. On the
     * classic ESP32 a chunked 11KB response otherwise owns a 16KB block and
     * can leave the largest free block just a few bytes below gzip ISIZE. */
    char *tight = realloc(resp, got + 1);
    if (tight) {
        resp = tight;
        capacity = got;
    }

    /* Decode gzip after closing the client so TLS has already returned its
     * heap. ISIZE lets us allocate exactly what this page needs. */
    if (got >= 2 && (unsigned char)resp[0] == 0x1f &&
        (unsigned char)resp[1] == 0x8b) {
        const unsigned char *raw = NULL;
        size_t raw_len = 0;
        uint32_t expected_crc = 0;
        uint32_t expected_size = 0;
        if (!gzip_raw_offset((const unsigned char *)resp, (size_t)got,
                             &raw, &raw_len, &expected_crc, &expected_size)) {
            ESP_LOGE(TAG, "unrecognized gzip header for %s", url);
            free(resp);
            return ESP_FAIL;
        }
        if (expected_size == 0 || expected_size >= DECOMPRESS_CAP) {
            ESP_LOGE(TAG, "gzip ISIZE invalid/too large: %u",
                     (unsigned)expected_size);
            free(resp);
            return ESP_ERR_INVALID_SIZE;
        }
        ESP_LOGI(TAG, "gzip metadata: raw=%u, output=%u, crc=%08x",
                 (unsigned)raw_len, (unsigned)expected_size,
                 (unsigned)expected_crc);
        size_t raw_offset = (size_t)(raw - (const unsigned char *)resp);
        char *dec = NULL;

        /* Some source pages expand to just over 32KB. While the total heap is
         * sufficient, classic ESP32 heap regions can have a 32768-byte largest
         * block after networking. Spool only those large compressed pages,
         * release their receive block, then reserve the decode block first. */
        if (expected_size >= 32 * 1024 && esp_spiffs_mounted("spiffs")) {
            static const char spool_path[] = "/spiffs/.http-page.gz";
            FILE *spool = fopen(spool_path, "wb");
            bool stored = false;
            if (spool) {
                stored = fwrite(resp, 1, got, spool) == got &&
                         fflush(spool) == 0;
                if (fclose(spool) != 0) {
                    stored = false;
                }
                spool = NULL;
            }
            if (!stored) {
                remove(spool_path);
                free(resp);
                return ESP_FAIL;
            }
            free(resp);
            resp = NULL;
            dec = malloc((size_t)expected_size + 1);
            if (dec) {
                resp = malloc(got + 1);
            }
            spool = resp ? fopen(spool_path, "rb") : NULL;
            bool loaded = spool && fread(resp, 1, got, spool) == got;
            if (spool) {
                fclose(spool);
            }
            remove(spool_path);
            if (!dec || !resp || !loaded) {
                log_heap_error("large gzip staging", (size_t)expected_size + got + 2);
                free(dec);
                free(resp);
                return ESP_ERR_NO_MEM;
            }
            resp[got] = '\0';
            raw = (const unsigned char *)resp + raw_offset;
            ESP_LOGI(TAG, "large gzip page staged through SPIFFS");
        } else {
            dec = malloc((size_t)expected_size + 1);
        }
        if (!dec) {
            log_heap_error("gzip output", (size_t)expected_size + 1);
            free(resp);
            return ESP_ERR_NO_MEM;
        }
        /* Reserve the largest object first. miniz state is much smaller and
         * can fit into a remaining fragment; doing this in the opposite order
         * can split a 47KB block just below a 33KB page's requirement. */
        tinfl_decompressor *inflator = malloc(sizeof(*inflator));
        if (!inflator) {
            log_heap_error("gzip state", sizeof(*inflator));
            free(dec);
            free(resp);
            return ESP_ERR_NO_MEM;
        }
        tinfl_init(inflator);
        size_t in_bytes = raw_len;
        size_t dec_len = expected_size;
        tinfl_status inflate_status = tinfl_decompress(
            inflator, raw, &in_bytes,
            (unsigned char *)dec, (unsigned char *)dec, &dec_len,
            TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
        free(inflator);
        if (!heap_caps_check_integrity_all(true)) {
            ESP_LOGE(TAG, "heap corruption detected after gzip decode");
            return ESP_FAIL;
        }
        if (inflate_status != TINFL_STATUS_DONE ||
            dec_len != expected_size || in_bytes != raw_len) {
            ESP_LOGE(TAG,
                     "gzip decompress failed (status=%d, in=%u/%u, out=%u/%u)",
                     (int)inflate_status, (unsigned)in_bytes, (unsigned)raw_len,
                     (unsigned)dec_len, (unsigned)expected_size);
            free(dec);
            free(resp);
            return ESP_FAIL;
        }
        uint32_t actual_crc = (uint32_t)mz_crc32(MZ_CRC32_INIT,
                                                 (const unsigned char *)dec,
                                                 dec_len);
        if (!heap_caps_check_integrity_all(true)) {
            ESP_LOGE(TAG, "heap corruption detected after gzip CRC");
            return ESP_FAIL;
        }
        if (actual_crc != expected_crc) {
            ESP_LOGE(TAG, "gzip CRC mismatch (got=%08x expected=%08x)",
                     (unsigned)actual_crc, (unsigned)expected_crc);
            free(dec);
            free(resp);
            return ESP_ERR_INVALID_CRC;
        }
        dec[dec_len] = '\0';
        free(resp);
        *out_buf = dec;
        *out_len = dec_len;
        ESP_LOGI(TAG, "gzip decoded: %u -> %u bytes",
                 (unsigned)got, (unsigned)dec_len);
    } else {
        *out_buf = resp;
        *out_len = (size_t)got;
        ESP_LOGI(TAG, "identity body: %u bytes", (unsigned)got);
    }
    return ESP_OK;
}

/* The search result page expands to about 18 KiB. Keeping that page, the
 * 11 KiB miniz state and the HTTP client alive at the same time exceeds the
 * largest free block once LVGL and the TF driver are running. Ask for an
 * identity response and stream it to a temporary file; after the socket is
 * closed, allocate exactly one page-sized block for the parser. */
static esp_err_t http_get_page_once(const char *url,
                                    char **out_buf, size_t *out_len)
{
    if (!url || !out_buf || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_buf = NULL;
    *out_len = 0;

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .user_agent = "Mozilla/5.0 (Linux; Android 10) AppleWebKit/537.36 "
                      "Chrome/120.0 Mobile Safari/537.36",
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    esp_http_client_set_header(client, "Accept",
                               "text/html,application/xhtml+xml,*/*;q=0.8");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open failed (%s): %s", esp_err_to_name(err), url);
        esp_http_client_cleanup(client);
        return err;
    }
    int64_t header_len = esp_http_client_fetch_headers(client);
    if (header_len < 0) {
        ESP_LOGE(TAG, "fetch headers failed (%lld): %s",
                 (long long)header_len, url);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return header_len == -ESP_ERR_HTTP_EAGAIN ? ESP_ERR_TIMEOUT : ESP_FAIL;
    }
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP %d identity, content_length=%lld, free=%u, largest=%u",
             status, (long long)header_len,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    if (status != 200) {
        ESP_LOGE(TAG, "HTTP status %d for %s", status, url);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_HTTP_CONNECT;
    }
    if (header_len > HTTP_RESPONSE_CAP) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_SIZE;
    }

    const char *tmp_path = sd_storage_ready()
        ? "/sdcard/.reader/.http-page.tmp"
        : "/spiffs/.http-page.tmp";
    FILE *spool = fopen(tmp_path, "wb");
    if (!spool && sd_storage_ready()) {
        tmp_path = "/spiffs/.http-page.tmp";
        spool = fopen(tmp_path, "wb");
    }
    if (!spool) {
        ESP_LOGW(TAG, "cannot open HTTP spool file; using buffered fallback");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return http_get_page_once_buffered(url, out_buf, out_len);
    }

    unsigned char chunk[1024];
    size_t got = 0;
    esp_err_t read_err = ESP_OK;
    while (true) {
        int rd = esp_http_client_read(client, (char *)chunk, sizeof(chunk));
        if (rd < 0) {
            read_err = rd == -ESP_ERR_HTTP_EAGAIN ? ESP_ERR_TIMEOUT : ESP_FAIL;
            break;
        }
        if (rd == 0) {
            break;
        }
        if (got + (size_t)rd > HTTP_RESPONSE_CAP) {
            read_err = ESP_ERR_INVALID_SIZE;
            break;
        }
        if (fwrite(chunk, 1, (size_t)rd, spool) != (size_t)rd) {
            read_err = ESP_FAIL;
            break;
        }
        got += (size_t)rd;
    }
    bool complete = esp_http_client_is_complete_data_received(client);
    if (fclose(spool) != 0 && read_err == ESP_OK) {
        read_err = ESP_FAIL;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (read_err != ESP_OK || !complete || got == 0) {
        ESP_LOGE(TAG, "streamed HTTP receive failed (%s, complete=%d, bytes=%u)",
                 esp_err_to_name(read_err), complete, (unsigned)got);
        remove(tmp_path);
        return read_err != ESP_OK ? read_err : ESP_ERR_INVALID_SIZE;
    }

    /* Directory pages can be 30-40 KiB even though the first chapter link is
     * near the start. The classic ESP32 cannot always provide one contiguous
     * block that large after LVGL starts, so load the parser's bounded prefix.
     * Search pages and normal chapter pages fit completely in this window. */
    size_t parse_len = got > HTTP_PARSE_WINDOW ? HTTP_PARSE_WINDOW : got;
    char *page = malloc(parse_len + 1);
    if (!page) {
        log_heap_error("HTTP page after spool", parse_len + 1);
        remove(tmp_path);
        return ESP_ERR_NO_MEM;
    }
    spool = fopen(tmp_path, "rb");
    size_t loaded = spool ? fread(page, 1, parse_len, spool) : 0;
    if (spool) {
        fclose(spool);
    }
    remove(tmp_path);
    if (loaded != parse_len) {
        free(page);
        return ESP_FAIL;
    }
    if (parse_len >= 2 && (unsigned char)page[0] == 0x1f &&
        (unsigned char)page[1] == 0x8b) {
        ESP_LOGE(TAG, "server ignored identity encoding: %s", url);
        free(page);
        return ESP_ERR_NOT_SUPPORTED;
    }
    page[parse_len] = '\0';
    *out_buf = page;
    *out_len = parse_len;
    ESP_LOGI(TAG, "streamed identity body: %u/%u bytes",
             (unsigned)parse_len, (unsigned)got);
    return ESP_OK;
}

static esp_err_t http_get_page(const char *url, char **out_buf, size_t *out_len)
{
    esp_err_t err = ESP_FAIL;
    bool can_failover = strncmp(url, SEARCH_SCHEME_HOST,
                                strlen(SEARCH_SCHEME_HOST)) == 0;
    int attempts = can_failover ? 6 : 2;
    static const char *source_hosts[] = {
        SEARCH_SCHEME_HOST, SEARCH_FALLBACK_HOST, SEARCH_FALLBACK2_HOST,
    };
    char target_url[256];
    for (int attempt = 1; attempt <= attempts; attempt++) {
        const char *target = url;
        if (can_failover) {
            const char *host = source_hosts[(attempt - 1) % 3];
            snprintf(target_url, sizeof(target_url), "%s%s", host,
                     url + strlen(SEARCH_SCHEME_HOST));
            target = target_url;
        }
        err = http_get_page_once(target, out_buf, out_len);
        if (err == ESP_OK) {
            return ESP_OK;
        }
        if (attempt < attempts) {
            ESP_LOGW(TAG, "HTTP attempt %d failed (%s), switching source",
                     attempt, esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(800));
        }
    }
    return err;
}

static bool url_encode_query(const char *src, char *out, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    if (!src || !out || cap == 0) {
        return false;
    }
    size_t written = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
        bool plain = (*p >= 'a' && *p <= 'z') ||
                     (*p >= 'A' && *p <= 'Z') ||
                     (*p >= '0' && *p <= '9') ||
                     *p == '-' || *p == '_' || *p == '.' || *p == '~';
        size_t needed = plain ? 1 : 3;
        if (written + needed + 1 > cap) {
            return false;
        }
        if (plain) {
            out[written++] = (char)*p;
        } else {
            out[written++] = '%';
            out[written++] = hex[*p >> 4];
            out[written++] = hex[*p & 0x0f];
        }
    }
    out[written] = '\0';
    return written > 0;
}

static void web_search_task(void *arg)
{
    char *query = (char *)arg;
    char encoded[192];
    char url[256];
    char *html = NULL;
    size_t html_len = 0;
    web_html_book_t parsed[WEB_SCRAPER_SEARCH_MAX];

    if (!url_encode_query(query, encoded, sizeof(encoded))) {
        lock();
        s_search.state = WEB_SEARCH_FAILED;
        snprintf(s_search.message, sizeof(s_search.message), "搜索词过长");
        goto done_locked;
    }
    snprintf(url, sizeof(url), "%s/search.php?q=%s",
             SEARCH_SCHEME_HOST, encoded);
    esp_err_t err = http_get_page(url, &html, &html_len);
    if (err != ESP_OK) {
        lock();
        s_search.state = WEB_SEARCH_FAILED;
        snprintf(s_search.message, sizeof(s_search.message),
                 "搜索失败: %s", esp_err_to_name(err));
        goto done_locked;
    }

    int count = web_html_parse_book_search(html, html_len, parsed,
                                            WEB_SCRAPER_SEARCH_MAX);
    free(html);
    html = NULL;
    lock();
    if (count < 0) {
        s_search.state = WEB_SEARCH_FAILED;
        snprintf(s_search.message, sizeof(s_search.message), "搜索结果解析失败");
    } else {
        s_search.count = count;
        for (int i = 0; i < count; i++) {
            s_search.results[i].book_id = parsed[i].book_id;
            snprintf(s_search.results[i].title,
                     sizeof(s_search.results[i].title), "%s", parsed[i].title);
            snprintf(s_search.results[i].author,
                     sizeof(s_search.results[i].author), "%s", parsed[i].author);
        }
        s_search.state = WEB_SEARCH_READY;
        snprintf(s_search.message, sizeof(s_search.message),
                 count > 0 ? "找到 %d 本书" : "没有找到相关书籍", count);
    }

done_locked:
    s_search_running = false;
    unlock();
    free(html);
    free(query);
    vTaskDelete(NULL);
}

esp_err_t web_scraper_search(const char *query)
{
    if (!query || !query[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t len = strlen(query);
    if (len > 63) {
        return ESP_ERR_INVALID_SIZE;
    }
    char *copy = malloc(len + 1);
    if (!copy) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, query, len + 1);

    lock();
    if (s_search_running || s_running) {
        unlock();
        free(copy);
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_search, 0, sizeof(s_search));
    s_search.state = WEB_SEARCH_RUNNING;
    snprintf(s_search.message, sizeof(s_search.message), "正在搜索...");
    s_search_running = true;
    unlock();

    BaseType_t ok = xTaskCreate(web_search_task, "book_search", 10 * 1024,
                                copy, 4, NULL);
    if (ok != pdPASS) {
        lock();
        s_search_running = false;
        s_search.state = WEB_SEARCH_FAILED;
        snprintf(s_search.message, sizeof(s_search.message), "无法创建搜索任务");
        unlock();
        free(copy);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* download task                                                       */
/* ------------------------------------------------------------------ */

static bool load_catalog_page(int page_number, int total_hint)
{
    if (page_number < 1) {
        return false;
    }
    char path[96];
    char url[160];
    char message[96];
    snprintf(path, sizeof(path), INDEX_PAGE_PATH, page_number);
    snprintf(url, sizeof(url), "%s%s", BASE_SCHEME_HOST, path);
    snprintf(message, sizeof(message), "正在读取目录第 %d 页", page_number);
    set_progress((page_number - 1) * WEB_HTML_INDEX_PAGE_SIZE + 1,
                 total_hint, message);

    char *html = NULL;
    size_t html_len = 0;
    esp_err_t err = http_get_page(url, &html, &html_len);
    if (err != ESP_OK) {
        free(html);
        set_state(WEB_SCRAPER_FAILED, "目录第 %d 页获取失败: %s",
                  page_number, esp_err_to_name(err));
        return false;
    }
    int count = web_html_parse_index(html, html_len, s_catalog_page,
                                     WEB_HTML_INDEX_PAGE_SIZE);
    free(html);
    if (count <= 0) {
        set_state(WEB_SCRAPER_FAILED, "目录第 %d 页解析失败", page_number);
        return false;
    }
    s_loaded_catalog_page = page_number;
    s_loaded_catalog_count = count;
    ESP_LOGI(TAG, "catalog page %d loaded (%d chapters)",
             page_number, count);
    return true;
}

static const web_html_chapter_t *catalog_chapter(int chapter_number,
                                                  int total)
{
    int page = (chapter_number - 1) / WEB_HTML_INDEX_PAGE_SIZE + 1;
    int slot = (chapter_number - 1) % WEB_HTML_INDEX_PAGE_SIZE;
    if (s_loaded_catalog_page != page && !load_catalog_page(page, total)) {
        return NULL;
    }
    if (slot < 0 || slot >= s_loaded_catalog_count) {
        set_state(WEB_SCRAPER_FAILED, "目录中找不到第 %d 章", chapter_number);
        return NULL;
    }
    return &s_catalog_page[slot];
}

static bool chapter_file_exists(int chapter_number)
{
    char path[64];
    struct stat st;
    snprintf(path, sizeof(path), "/spiffs/cache_%03d.txt", chapter_number);
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

static void prune_chapter_cache(int keep_start, int keep_end)
{
    DIR *dir = opendir("/spiffs");
    if (!dir) {
        ESP_LOGW(TAG, "cannot scan SPIFFS for cache pruning");
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        int number = 0;
        char extra = '\0';
        char expected_name[24];
        bool final_file =
            sscanf(entry->d_name, "cache_%d.txt%c", &number, &extra) == 1;
        if (final_file) {
            snprintf(expected_name, sizeof(expected_name), "cache_%03d.txt", number);
            final_file = number >= 1 && strcmp(entry->d_name, expected_name) == 0;
        }
        bool work_file = false;
        bool backup_file = false;
        int work_number = 0;
        if (!final_file) {
            work_file = sscanf(entry->d_name, ".cache_%d.tmp%c",
                               &work_number, &extra) == 1;
            if (!work_file) {
                backup_file = sscanf(entry->d_name, ".cache_%d.bak%c",
                                     &work_number, &extra) == 1;
                work_file = backup_file;
            }
        }
        bool only_backup_copy = false;
        if (backup_file && work_number >= keep_start &&
            work_number <= keep_end) {
            char final_path[64];
            struct stat st;
            snprintf(final_path, sizeof(final_path),
                     "/spiffs/cache_%03d.txt", work_number);
            only_backup_copy = stat(final_path, &st) != 0;
        }
        if ((!final_file && !work_file) ||
            (final_file && number >= keep_start && number <= keep_end) ||
            only_backup_copy) {
            continue;
        }
        char path[320];
        snprintf(path, sizeof(path), "/spiffs/%s", entry->d_name);
        if (remove(path) == 0) {
            ESP_LOGI(TAG, "removed cache file %s", entry->d_name);
        } else {
            ESP_LOGW(TAG, "cannot remove cache file %s", entry->d_name);
        }
    }
    closedir(dir);
}

static uint32_t read_cache_owner(void)
{
    uint32_t book_id = 0;
    FILE *f = fopen(CACHE_OWNER_PATH, "rb");
    if (f) {
        if (fread(&book_id, sizeof(book_id), 1, f) != 1) {
            book_id = 0;
        }
        fclose(f);
    }
    return book_id;
}

static bool write_cache_owner(uint32_t book_id)
{
    FILE *f = fopen(CACHE_OWNER_TMP_PATH, "wb");
    if (!f) {
        return false;
    }
    bool ok = fwrite(&book_id, sizeof(book_id), 1, f) == 1 &&
              fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f) != 0) {
        ok = false;
    }
    if (ok) {
        (void)remove(CACHE_OWNER_PATH);
        ok = rename(CACHE_OWNER_TMP_PATH, CACHE_OWNER_PATH) == 0;
    }
    if (!ok) {
        remove(CACHE_OWNER_TMP_PATH);
        return false;
    }
    return true;
}

static int choose_missing_chapter(int center, int start, int end)
{
    static const int offsets[CHAPTER_CACHE_SIZE] = { 0, 1, -1, 2, -2 };
    for (int i = 0; i < CHAPTER_CACHE_SIZE; i++) {
        int candidate = center + offsets[i];
        if (candidate >= start && candidate <= end &&
            !chapter_file_exists(candidate)) {
            return candidate;
        }
    }
    for (int candidate = start; candidate <= end; candidate++) {
        if (!chapter_file_exists(candidate)) {
            return candidate;
        }
    }
    return 0;
}

static bool download_one_chapter(int chapter_number, int total,
                                  const web_html_chapter_t *meta)
{
    char url[160];
    char *chapter = NULL;
    size_t chapter_len = 0;
    esp_err_t err;
    bool success = false;

    char first_title[WEB_HTML_TITLE_SIZE];
    snprintf(first_title, sizeof(first_title), "%s", meta->title);

    char base_num[32];
    const char *slash = strrchr(meta->path, '/');
    const char *name = slash ? slash + 1 : meta->path;
    const char *dot = strrchr(name, '.');
    if (!dot) {
        set_state(WEB_SCRAPER_FAILED, "第 %d 章 URL 无效", chapter_number);
        return false;
    }
    size_t numlen = (size_t)(dot - name);
    if (numlen >= sizeof(base_num)) {
        numlen = sizeof(base_num) - 1;
    }
    memcpy(base_num, name, numlen);
    base_num[numlen] = '\0';

    char current_path[WEB_HTML_PATH_SIZE];
    snprintf(current_path, sizeof(current_path), "%s", meta->path);

    int part = 0;
    bool first_part = true;
    bool has_next = false;
    while (part < MAX_PAGE_PARTS) {
        char progress[96];
        snprintf(progress, sizeof(progress), "正在后台缓存第 %d/%d 章",
                 chapter_number, total);
        set_progress(chapter_number, total, progress);

        snprintf(url, sizeof(url), "%s%s", BASE_SCHEME_HOST, current_path);
        char *html = NULL;
        size_t html_len = 0;
        err = http_get_page(url, &html, &html_len);
        if (err != ESP_OK) {
            free(html);
            set_state(WEB_SCRAPER_FAILED, "第 %d 章获取失败: %s",
                      chapter_number, esp_err_to_name(err));
            goto cleanup;
        }

        if (first_part) {
            char title[WEB_HTML_TITLE_SIZE] = "";
            web_html_extract_title(html, html_len, title, sizeof(title));
            if (title[0]) {
                snprintf(first_title, sizeof(first_title), "%s", title);
            }
        }

        char next_path[WEB_HTML_PATH_SIZE] = "";
        has_next = web_html_next_part_path(html, html_len, base_num,
                                           part, next_path,
                                           sizeof(next_path));
        size_t blen = web_html_extract_body(html, html_len,
                                            html, html_len + 1);
        if (blen == 0 || blen == (size_t)-1) {
            free(html);
            set_state(WEB_SCRAPER_FAILED, "第 %d 章正文为空", chapter_number);
            goto cleanup;
        }

        bool add_newline = !first_part && chapter_len > 0 &&
                           chapter[chapter_len - 1] != '\n';
        size_t needed = chapter_len + (add_newline ? 1 : 0) + blen + 1;
        if (needed > CHAPTER_TEXT_CAP) {
            free(html);
            set_state(WEB_SCRAPER_FAILED, "第 %d 章过长，超过内存上限",
                      chapter_number);
            goto cleanup;
        }
        char *grown = realloc(chapter, needed);
        if (!grown) {
            log_heap_error("chapter text", needed);
            free(html);
            set_state(WEB_SCRAPER_FAILED, "第 %d 章内存不足", chapter_number);
            goto cleanup;
        }
        chapter = grown;
        if (add_newline) {
            chapter[chapter_len++] = '\n';
        }
        memcpy(chapter + chapter_len, html, blen);
        chapter_len += blen;
        chapter[chapter_len] = '\0';
        free(html);

        if (!has_next) {
            break;
        }
        snprintf(current_path, sizeof(current_path), "%s", next_path);
        first_part = false;
        part++;
    }

    if (has_next) {
        set_state(WEB_SCRAPER_FAILED,
                  "第 %d 章分页超过 %d 页上限，已停止",
                  chapter_number, MAX_PAGE_PARTS);
        goto cleanup;
    }

    set_progress(chapter_number, total, "正在保存章节");
    if (!write_chapter_atomic(chapter_number, first_title,
                              chapter, chapter_len)) {
        set_state(WEB_SCRAPER_FAILED,
                  "第 %d 章写入失败（原有章节已保留）", chapter_number);
        goto cleanup;
    }
    success = true;

cleanup:
    free(chapter);
    return success;
}

static bool reset_id_catalog(uint32_t first_id)
{
    FILE *f = fopen(s_catalog_id_path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "cannot create %s", s_catalog_id_path);
        return false;
    }
    bool ok = fwrite(&first_id, sizeof(first_id), 1, f) == 1 &&
              fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f) != 0) {
        ok = false;
    }
    return ok;
}

static bool append_catalog_id(uint32_t chapter_id)
{
    FILE *f = fopen(s_catalog_id_path, "ab");
    if (!f) {
        return false;
    }
    bool ok = fwrite(&chapter_id, sizeof(chapter_id), 1, f) == 1 &&
              fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f) != 0) {
        ok = false;
    }
    if (ok) {
        lock();
        s_catalog_count++;
        s_status.total = s_catalog_count;
        unlock();
    }
    return ok;
}

static bool catalog_id_at(int chapter_number, uint32_t *out_id)
{
    if (chapter_number < 1 || !out_id) {
        return false;
    }
    FILE *f = fopen(s_catalog_id_path, "rb");
    if (!f) {
        return false;
    }
    long offset = (long)(chapter_number - 1) * (long)sizeof(uint32_t);
    bool ok = fseek(f, offset, SEEK_SET) == 0 &&
              fread(out_id, sizeof(*out_id), 1, f) == 1;
    fclose(f);
    return ok;
}

static bool download_biquge_chapter(int chapter_number, uint32_t book_id,
                                    uint32_t chapter_id)
{
    char url[192];
    char *html = NULL;
    size_t html_len = 0;
    char path[96];
    char next_path[96];
    uint32_t next_id = 0;
    bool has_next = false;
    bool chapter_finished = false;
    chapter_writer_t writer;
    memset(&writer, 0, sizeof(writer));
    bool writer_started = false;
    size_t chapter_body_len = 0;
    int known = web_scraper_get_catalog_size();
    char progress[96];
    snprintf(progress, sizeof(progress), "正在后台缓存第 %d 章", chapter_number);
    set_progress(chapter_number, known, progress);
    char title[WEB_HTML_TITLE_SIZE] = "";
    snprintf(path, sizeof(path), "/%u/%u/%u.html",
             (unsigned)(book_id / 1000u), (unsigned)book_id,
             (unsigned)chapter_id);

    for (int part = 0; part < MAX_PAGE_PARTS; part++) {
        if (stop_requested()) {
            chapter_writer_abort(&writer);
            return false;
        }
        snprintf(url, sizeof(url), "%s%s", SEARCH_SCHEME_HOST, path);
        esp_err_t err = http_get_page(url, &html, &html_len);
        if (err != ESP_OK) {
            free(html);
            chapter_writer_abort(&writer);
            set_state(WEB_SCRAPER_FAILED, "第 %d 章获取失败: %s",
                      chapter_number, esp_err_to_name(err));
            return false;
        }
        if (!title[0]) {
            web_html_extract_biquge_title(html, html_len, title, sizeof(title));
        }
        uint32_t link_id = 0;
        bool has_link = web_html_biquguo_next_path(html, html_len, book_id,
                                                   next_path, sizeof(next_path),
                                                   &link_id);
        size_t body_len = web_html_extract_biquge_body(html, html_len,
                                                       html, html_len + 1);
        if (body_len == 0 || body_len == (size_t)-1) {
            free(html);
            chapter_writer_abort(&writer);
            set_state(WEB_SCRAPER_FAILED, "第 %d 章正文解析失败", chapter_number);
            return false;
        }
        if (!title[0]) {
            snprintf(title, sizeof(title), "第 %d 章", chapter_number);
        }
        if (!writer_started) {
            if (!chapter_writer_begin(&writer, chapter_number, title)) {
                free(html);
                set_state(WEB_SCRAPER_FAILED, "第 %d 章临时文件创建失败",
                          chapter_number);
                return false;
            }
            writer_started = true;
        }
        size_t needed = chapter_body_len + (chapter_body_len ? 1 : 0) + body_len;
        if (needed > CHAPTER_TEXT_CAP) {
            free(html);
            chapter_writer_abort(&writer);
            set_state(WEB_SCRAPER_FAILED, "第 %d 章正文过长", chapter_number);
            return false;
        }
        if (!chapter_writer_append(&writer, html, body_len,
                                   chapter_body_len > 0)) {
            free(html);
            chapter_writer_abort(&writer);
            set_state(WEB_SCRAPER_FAILED, "第 %d 章临时文件写入失败",
                      chapter_number);
            return false;
        }
        chapter_body_len = needed;
        free(html);
        html = NULL;

        if (!has_link) {
            chapter_finished = true;
            break;
        }
        if (link_id != chapter_id) {
            next_id = link_id;
            has_next = true;
            chapter_finished = true;
            break;
        }
        if (strcmp(path, next_path) == 0) {
            chapter_writer_abort(&writer);
            set_state(WEB_SCRAPER_FAILED, "第 %d 章分页循环", chapter_number);
            return false;
        }
        snprintf(path, sizeof(path), "%s", next_path);
    }
    if (!chapter_finished) {
        chapter_writer_abort(&writer);
        set_state(WEB_SCRAPER_FAILED, "第 %d 章分页超过上限", chapter_number);
        return false;
    }
    if (!writer_started || chapter_body_len == 0) {
        chapter_writer_abort(&writer);
        set_state(WEB_SCRAPER_FAILED, "第 %d 章写入失败", chapter_number);
        return false;
    }

    lock();
    int catalog_count = s_catalog_count;
    unlock();
    if (has_next && chapter_number == catalog_count) {
        if (!append_catalog_id(next_id)) {
            chapter_writer_abort(&writer);
            set_state(WEB_SCRAPER_FAILED, "章节索引写入失败");
            return false;
        }
    } else if (!has_next && chapter_number == catalog_count) {
        lock();
        s_catalog_complete = true;
        unlock();
        ESP_LOGI(TAG, "reached end of book at chapter %d", chapter_number);
    }
    if (!chapter_writer_finish(&writer)) {
        chapter_writer_abort(&writer);
        set_state(WEB_SCRAPER_FAILED, "第 %d 章写入失败", chapter_number);
        return false;
    }
    esp_err_t archive_err = sd_storage_archive_chapter(
        book_id, s_selected_book_title, chapter_number, writer.final_path);
    if (archive_err != ESP_OK) {
        ESP_LOGW(TAG, "TF archive skipped for chapter %d: %s",
                 chapter_number, esp_err_to_name(archive_err));
    }
    return true;
}

static void web_scraper_dynamic_task(uint32_t book_id)
{
    char url[160];
    snprintf(url, sizeof(url), "%s/%u/%u/", SEARCH_SCHEME_HOST,
             (unsigned)(book_id / 1000u), (unsigned)book_id);
    set_state(WEB_SCRAPER_GETTING_INDEX, "正在读取书籍信息");
    char *detail = NULL;
    size_t detail_len = 0;
    esp_err_t err = http_get_page(url, &detail, &detail_len);
    if (err != ESP_OK) {
        free(detail);
        set_state(WEB_SCRAPER_FAILED, "获取书籍信息失败: %s",
                  esp_err_to_name(err));
        return;
    }
    uint32_t first_id = 0;
    bool found = web_html_book_start_id(detail, detail_len, book_id, &first_id);
    free(detail);
    if (!found || first_id == 0) {
        set_state(WEB_SCRAPER_FAILED, "找不到第一章");
        return;
    }

    /* Text cache belongs to one active book, while each recent book keeps its
     * tiny four-byte-per-chapter ID index so reopening can resume directly. */
    if (s_cached_book_id == 0) {
        s_cached_book_id = read_cache_owner();
    }
    bool reuse_text_cache = s_cached_book_id == book_id;
    if (!reuse_text_cache) {
        prune_chapter_cache(0, 0);
        if (!write_cache_owner(book_id)) {
            set_state(WEB_SCRAPER_FAILED, "无法保存缓存归属");
            return;
        }
        s_cached_book_id = book_id;
    }

    int known_count = 0;
    struct stat catalog_stat;
    uint32_t saved_first_id = 0;
    if (stat(s_catalog_id_path, &catalog_stat) == 0 &&
        catalog_stat.st_size >= (off_t)sizeof(uint32_t) &&
        catalog_stat.st_size % (off_t)sizeof(uint32_t) == 0 &&
        catalog_id_at(1, &saved_first_id) && saved_first_id == first_id) {
        known_count = (int)(catalog_stat.st_size / sizeof(uint32_t));
    } else {
        if (!reset_id_catalog(first_id)) {
            set_state(WEB_SCRAPER_FAILED, "无法创建章节索引");
            return;
        }
        known_count = 1;
    }
    int start_chapter = s_selected_start_chapter;
    if (start_chapter < 1) {
        start_chapter = 1;
    }
    if (start_chapter > known_count) {
        start_chapter = known_count;
    }
    lock();
    s_catalog_count = known_count;
    s_status.total = known_count;
    s_catalog_complete = false;
    s_reading_chapter = start_chapter;
    unlock();

    bool reader_announced = false;
    int last_start = 0;
    int last_end = 0;
    while (true) {
        if (stop_requested()) {
            ESP_LOGI(TAG, "background cache paused");
            return;
        }
        int known = web_scraper_get_catalog_size();
        int center = web_scraper_get_reading_chapter();
        int keep_start = 0;
        int keep_end = 0;
        chapter_cache_window(known, center, &keep_start, &keep_end);

        if (last_start != keep_start || last_end != keep_end) {
            prune_chapter_cache(keep_start, keep_end);
            last_start = keep_start;
            last_end = keep_end;
        }
        if (!reader_announced && chapter_file_exists(center)) {
            set_state(WEB_SCRAPER_READY, "第 %d 章已就绪，正在后台缓存",
                      center);
            reader_announced = true;
        }

        int missing = choose_missing_chapter(center, keep_start, keep_end);
        if (missing != 0) {
            uint32_t chapter_id = 0;
            if (!catalog_id_at(missing, &chapter_id) ||
                !download_biquge_chapter(missing, book_id, chapter_id)) {
                if (stop_requested()) {
                    return;
                }
                if (!reader_announced) {
                    return;
                }
                set_state(WEB_SCRAPER_READY, "第 %d 章缓存失败，稍后重试",
                          missing);
                vTaskDelay(pdMS_TO_TICKS(3000));
                continue;
            }
            if (!reader_announced && missing == center) {
                set_state(WEB_SCRAPER_READY, "第 %d 章已就绪，正在后台缓存",
                          center);
                reader_announced = true;
            }
            continue;
        }

        /* Recover from a previous run that published the frontier chapter but
         * failed before committing its successor ID. Re-reading just that
         * frontier repairs the catalog (or confirms the end of the book). */
        known = web_scraper_get_catalog_size();
        if (!web_scraper_catalog_complete() && keep_end >= known &&
            chapter_file_exists(known)) {
            uint32_t frontier_id = 0;
            if (catalog_id_at(known, &frontier_id)) {
                (void)download_biquge_chapter(known, book_id, frontier_id);
                continue;
            }
        }

        /* If the window reaches the newest discovered chapter, downloading it
         * has already discovered the following ID. The next loop naturally
         * widens the window until exactly five text chapters are resident. */
        set_state(WEB_SCRAPER_READY, "已缓存第 %d-%d 章", keep_start, keep_end);
        vTaskDelay(pdMS_TO_TICKS(300));
    }
}

static void web_scraper_task(void *arg)
{
    (void)arg;
    char url[160];

    if (!ensure_spiffs()) {
        set_state(WEB_SCRAPER_FAILED, "SPIFFS 挂载失败");
        goto out;
    }

    lock();
    uint32_t selected_book_id = s_selected_book_id;
    unlock();
    if (selected_book_id != 0) {
        web_scraper_dynamic_task(selected_book_id);
        goto out;
    }

    set_state(WEB_SCRAPER_GETTING_INDEX, "正在获取目录");
    snprintf(url, sizeof(url), "%s%s", BASE_SCHEME_HOST, INDEX_PATH);
    char *idx = NULL;
    size_t idx_len = 0;
    esp_err_t err = http_get_page(url, &idx, &idx_len);
    if (err != ESP_OK) {
        set_state(WEB_SCRAPER_FAILED, "获取目录失败: %s", esp_err_to_name(err));
        free(idx);
        goto out;
    }

    int page_count = web_html_index_page_count(idx, idx_len);
    int first_count = web_html_parse_index(idx, idx_len, s_catalog_page,
                                           WEB_HTML_INDEX_PAGE_SIZE);
    free(idx);
    if (page_count <= 0 || first_count <= 0) {
        set_state(WEB_SCRAPER_FAILED, "目录解析失败或没有章节");
        goto out;
    }
    s_loaded_catalog_page = 1;
    s_loaded_catalog_count = first_count;

    int n = first_count;
    if (page_count > 1) {
        if (!load_catalog_page(page_count, 0)) {
            goto out;
        }
        n = (page_count - 1) * WEB_HTML_INDEX_PAGE_SIZE +
            s_loaded_catalog_count;
    }
    ESP_LOGI(TAG, "catalog has %d pages and %d chapters", page_count, n);

    lock();
    s_catalog_count = n;
    s_status.total = n;
    if (s_reading_chapter < 1) {
        s_reading_chapter = 1;
    } else if (s_reading_chapter > n) {
        s_reading_chapter = n;
    }
    unlock();

    bool reader_announced = false;
    int last_ready_start = 0;
    int last_ready_end = 0;
    int last_pruned_start = 0;
    int last_pruned_end = 0;
    while (true) {
        if (stop_requested()) {
            ESP_LOGI(TAG, "background cache paused");
            goto out;
        }
        int center = web_scraper_get_reading_chapter();
        int keep_start = 0;
        int keep_end = 0;
        chapter_cache_window(n, center, &keep_start, &keep_end);

        if (last_pruned_start != keep_start || last_pruned_end != keep_end) {
            prune_chapter_cache(keep_start, keep_end);
            last_pruned_start = keep_start;
            last_pruned_end = keep_end;
        }

        if (!reader_announced && chapter_file_exists(center)) {
            set_state(WEB_SCRAPER_READY,
                      "第 %d 章已就绪，后台缓存 %d-%d 章",
                      center, keep_start, keep_end);
            reader_announced = true;
            ESP_LOGI(TAG, "reader ready at chapter %d", center);
        }

        int missing = choose_missing_chapter(center, keep_start, keep_end);
        if (missing != 0) {
            const web_html_chapter_t *meta = catalog_chapter(missing, n);
            if (!meta || !download_one_chapter(missing, n, meta)) {
                if (!reader_announced) {
                    goto out;
                }
                set_state(WEB_SCRAPER_READY,
                          "第 %d 章缓存失败，稍后自动重试", missing);
                vTaskDelay(pdMS_TO_TICKS(3000));
                continue;
            }
            if (!reader_announced && missing == center) {
                set_state(WEB_SCRAPER_READY,
                          "第 %d 章已就绪，后台缓存 %d-%d 章",
                          center, keep_start, keep_end);
                reader_announced = true;
                ESP_LOGI(TAG, "reader ready at chapter %d", center);
            }
            continue;
        }

        if (last_ready_start != keep_start || last_ready_end != keep_end) {
            set_state(WEB_SCRAPER_READY, "已缓存第 %d-%d 章",
                      keep_start, keep_end);
            ESP_LOGI(TAG, "cache window %d-%d ready", keep_start, keep_end);
            last_ready_start = keep_start;
            last_ready_end = keep_end;
        }
        vTaskDelay(pdMS_TO_TICKS(300));
    }

out:
    lock();
    s_running = false;
    unlock();
    vTaskDelete(NULL);
}

esp_err_t web_scraper_start(void)
{
    lock();
    if (s_running || s_search_running) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    s_running = true;
    s_stop_requested = false;
    s_status.total = 0;
    s_status.current = 0;
    s_reader_ready = false;
    s_catalog_count = 0;
    s_reading_chapter = s_selected_book_id != 0 ?
                        s_selected_start_chapter : 1;
    s_loaded_catalog_page = 0;
    s_loaded_catalog_count = 0;
    s_catalog_complete = s_selected_book_id == 0;
    s_status.state = WEB_SCRAPER_GETTING_INDEX;
    snprintf(s_status.message, sizeof(s_status.message), "正在获取目录");
    unlock();

    BaseType_t ok = xTaskCreate(web_scraper_task, "web_scraper",
                                TASK_STACK_BYTES, NULL, 4, NULL);
    if (ok != pdPASS) {
        lock();
        s_running = false;
        s_status.state = WEB_SCRAPER_FAILED;
        snprintf(s_status.message, sizeof(s_status.message), "创建下载任务失败");
        unlock();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
