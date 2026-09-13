/* SPDX-License-Identifier: MIT */

#include "txt_download_ui.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gbk_codec.h"
#include "main_menu_ui.h"
#include "pinyin_dict.h"
#include "sd_storage.h"

LV_FONT_DECLARE(novel_font_16);

#define TXT_SEARCH_URL "https://wap.txt80.com/e/search/index.php"
#define TXT_SITE_URL "https://wap.txt80.com"
#define TXT_RESULT_MAX 8
#define SEARCH_PARSE_CAP (20 * 1024)
#define SEARCH_TMP_PATH "/sdcard/.reader/.txt-search.tmp"
#define DETAIL_TMP_PATH "/sdcard/.reader/.txt-detail.tmp"
#define DOWNLOAD_RAW_PATH "/sdcard/novels/.download.raw"
#define DOWNLOAD_TMP_PATH "/sdcard/novels/.download.tmp"

static const char *TAG = "txt_download";

typedef enum {
    TXT_STATE_IDLE = 0,
    TXT_STATE_SEARCHING,
    TXT_STATE_RESULTS,
    TXT_STATE_RESOLVING,
    TXT_STATE_DOWNLOADING,
    TXT_STATE_DONE,
    TXT_STATE_FAILED,
} txt_state_t;

typedef struct {
    char title[96];
    char author[48];
    char detail_path[112];
} txt_result_t;

typedef struct {
    txt_state_t state;
    bool running;
    size_t received;
    int64_t total;
    int count;
    unsigned generation;
    char message[128];
    txt_result_t results[TXT_RESULT_MAX];
} txt_status_t;

typedef struct {
    FILE *file;
    bool report_progress;
    bool transform_text;
    size_t received;
    size_t base_received;
    int64_t total;
    int text_encoding; /* 0 undecided, 1 UTF-8, 2 GBK */
    uint8_t pending_gbk;
    char location[192];
} http_sink_t;

typedef struct {
    char query[64];
} search_args_t;

typedef struct {
    txt_result_t book;
    int preferred_mirror;
} download_args_t;

static SemaphoreHandle_t s_lock;
static txt_status_t s_shared = {
    .state = TXT_STATE_IDLE,
    .message = "输入书名或作者搜索整本 TXT",
};

static lv_obj_t *s_query;
static lv_obj_t *s_search_button;
static lv_obj_t *s_source_dropdown;
static lv_obj_t *s_status_label;
static lv_obj_t *s_results_box;
static lv_obj_t *s_progress;
static lv_obj_t *s_keyboard;
static lv_obj_t *s_ime;
static lv_timer_t *s_timer;
static txt_result_t s_ui_results[TXT_RESULT_MAX];
static int s_ui_result_count;
static unsigned s_rendered_generation;

static void release_input_ui(void);

static void ensure_lock(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
}

static void shared_update(txt_state_t state, bool running, size_t received,
                          int64_t total, const char *message)
{
    ensure_lock();
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    s_shared.state = state;
    s_shared.running = running;
    s_shared.received = received;
    s_shared.total = total;
    if (message) {
        snprintf(s_shared.message, sizeof(s_shared.message), "%s", message);
    }
    if (s_lock) xSemaphoreGive(s_lock);
}

static void shared_snapshot(txt_status_t *out)
{
    ensure_lock();
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_shared;
    if (s_lock) xSemaphoreGive(s_lock);
}

static bool url_encode(const char *src, char *out, size_t cap,
                       const char *safe_chars)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t n = 0;
    if (!src || !out || cap == 0) return false;
    for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
        bool safe = isalnum(*p) || strchr(safe_chars, *p) != NULL;
        size_t need = safe ? 1 : 3;
        if (n + need + 1 > cap) return false;
        if (safe) {
            out[n++] = (char)*p;
        } else {
            out[n++] = '%';
            out[n++] = hex[*p >> 4];
            out[n++] = hex[*p & 0x0f];
        }
    }
    out[n] = '\0';
    return n > 0;
}

static esp_err_t http_event(esp_http_client_event_t *event)
{
    http_sink_t *sink = (http_sink_t *)event->user_data;
    if (!sink) return ESP_OK;
    if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0) {
        bool write_ok = sink->file != NULL;
        if (write_ok && sink->transform_text && sink->text_encoding == 0) {
            sink->text_encoding = text_looks_utf8(
                                      (const uint8_t *)event->data,
                                      (size_t)event->data_len) ? 1 : 2;
            ESP_LOGI(TAG, "TXT source encoding: %s",
                     sink->text_encoding == 1 ? "UTF-8" : "GBK");
        }
        if (write_ok && sink->transform_text && sink->text_encoding == 2) {
            write_ok = gbk_write_utf8(sink->file, &sink->pending_gbk,
                                      (const uint8_t *)event->data,
                                      (size_t)event->data_len);
        } else if (write_ok) {
            write_ok = fwrite(event->data, 1, (size_t)event->data_len,
                              sink->file) == (size_t)event->data_len;
        }
        if (!write_ok) {
            return ESP_FAIL;
        }
        sink->received += (size_t)event->data_len;
        if (sink->report_progress) {
            size_t cumulative = sink->base_received + sink->received;
            char message[96];
            snprintf(message, sizeof(message), "正在下载整本 TXT：%u KB",
                     (unsigned)(cumulative / 1024));
            shared_update(TXT_STATE_DOWNLOADING, true, cumulative,
                          sink->total, message);
        }
    } else if (event->event_id == HTTP_EVENT_ON_HEADER &&
               event->header_key && event->header_value) {
        if (strcasecmp(event->header_key, "Content-Length") == 0) {
            sink->total = (int64_t)sink->base_received +
                          strtoll(event->header_value, NULL, 10);
        } else if (strcasecmp(event->header_key, "Location") == 0) {
            snprintf(sink->location, sizeof(sink->location), "%s",
                     event->header_value);
        }
    }
    return ESP_OK;
}

static esp_err_t http_to_file(const char *url, esp_http_client_method_t method,
                              const char *body, const char *path,
                              bool report_progress, size_t *received_out)
{
    FILE *file = fopen(path, "wb");
    if (!file) return ESP_FAIL;
    http_sink_t sink = {
        .file = file,
        .report_progress = report_progress,
        .transform_text = report_progress,
        .total = -1,
    };
    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .timeout_ms = report_progress ? 30000 : 20000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_event,
        .user_data = &sink,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .user_agent = "ESP32-TXT-Reader/2.0",
        .max_redirection_count = 5,
        .disable_auto_redirect = method == HTTP_METHOD_POST,
    };
    ESP_LOGI(TAG, "heap before HTTP: free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_err_t err = client ? ESP_OK : ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "HTTP %s %s", method == HTTP_METHOD_POST ? "POST" : "GET",
             url);
    if (err == ESP_OK) {
        esp_http_client_set_header(client, "Accept-Encoding", "identity");
        esp_http_client_set_header(client, "Accept-Language", "zh-CN,zh;q=0.9");
        if (method == HTTP_METHOD_POST) {
            esp_http_client_set_header(
                client, "Content-Type", "application/x-www-form-urlencoded");
            esp_http_client_set_post_field(client, body, (int)strlen(body));
        }
        err = esp_http_client_perform(client);
    }
    int status = client ? esp_http_client_get_status_code(client) : 0;
    if (err == ESP_OK && method == HTTP_METHOD_POST && status >= 300 &&
        status < 400 && sink.location[0]) {
        char redirect_url[320];
        if (strncmp(sink.location, "https://", 8) == 0 ||
            strncmp(sink.location, "http://", 7) == 0) {
            snprintf(redirect_url, sizeof(redirect_url), "%s", sink.location);
        } else if (sink.location[0] == '/') {
            snprintf(redirect_url, sizeof(redirect_url), "%s%s", TXT_SITE_URL,
                     sink.location);
        } else {
            snprintf(redirect_url, sizeof(redirect_url),
                     "%s/e/search/%s", TXT_SITE_URL, sink.location);
        }
        ESP_LOGI(TAG, "follow search redirect: %s", redirect_url);
        /* Keep this client alive. Besides avoiding a second expensive TLS
         * handshake, the search result requires the cookie from the POST. */
        if (sink.received > 0) {
            fclose(file);
            file = fopen(path, "wb");
            sink.file = file;
            if (!file) err = ESP_FAIL;
        }
        sink.received = 0;
        sink.total = -1;
        sink.location[0] = '\0';
        if (err == ESP_OK) err = esp_http_client_set_url(client, redirect_url);
        if (err == ESP_OK) err = esp_http_client_set_method(client,
                                                             HTTP_METHOD_GET);
        if (err == ESP_OK) {
            esp_http_client_set_post_field(client, NULL, 0);
            err = esp_http_client_perform(client);
            status = esp_http_client_get_status_code(client);
        }
    }
    if (client) esp_http_client_cleanup(client);
    if (err == ESP_OK && sink.report_progress && sink.text_encoding == 2 &&
        !gbk_write_utf8_finish(file, &sink.pending_gbk)) {
        err = ESP_FAIL;
    }
    if (file && fclose(file) != 0 && err == ESP_OK) err = ESP_FAIL;
    if (err == ESP_OK && status != 200) err = ESP_ERR_HTTP_CONNECT;
    if (err == ESP_OK && sink.received == 0) err = ESP_ERR_INVALID_SIZE;
    if (received_out) *received_out = sink.received;
    if (err != ESP_OK) remove(path);
    ESP_LOGI(TAG, "HTTP done status=%d bytes=%u result=%s", status,
             (unsigned)sink.received, esp_err_to_name(err));
    return err;
}

static char *read_file_prefix(const char *path, size_t cap, size_t *len_out)
{
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    char *buf = malloc(cap + 1);
    if (!buf) {
        fclose(file);
        return NULL;
    }
    size_t got = fread(buf, 1, cap, file);
    fclose(file);
    buf[got] = '\0';
    if (len_out) *len_out = got;
    return buf;
}

static void trim_ascii(char *text)
{
    if (!text) return;
    char *start = text;
    while (*start && isspace((unsigned char)*start)) start++;
    if (start != text) memmove(text, start, strlen(start) + 1);
    size_t n = strlen(text);
    while (n > 0 && isspace((unsigned char)text[n - 1])) text[--n] = '\0';
}

static void copy_html_text(const char *start, const char *end,
                           char *out, size_t cap)
{
    size_t n = 0;
    bool in_tag = false;
    if (!out || cap == 0) return;
    while (start && start < end && *start && n + 1 < cap) {
        if (*start == '<') {
            in_tag = true;
            start++;
            continue;
        }
        if (*start == '>') {
            in_tag = false;
            start++;
            continue;
        }
        if (in_tag) {
            start++;
            continue;
        }
        if (strncmp(start, "&amp;", 5) == 0) {
            out[n++] = '&';
            start += 5;
        } else if (strncmp(start, "&quot;", 6) == 0) {
            out[n++] = '"';
            start += 6;
        } else if (strncmp(start, "&lt;", 4) == 0) {
            out[n++] = '<';
            start += 4;
        } else if (strncmp(start, "&gt;", 4) == 0) {
            out[n++] = '>';
            start += 4;
        } else if (strncmp(start, "&nbsp;", 6) == 0) {
            out[n++] = ' ';
            start += 6;
        } else {
            out[n++] = *start++;
        }
    }
    out[n] = '\0';
    trim_ascii(out);
}

static int parse_search_results(const char *html, txt_result_t *out, int cap)
{
    int count = 0;
    const char *p = strstr(html, "class=\"imgtextlist\"");
    if (!p) p = html;
    while (count < cap && (p = strstr(p, "<p class=\"title\"")) != NULL) {
        const char *href = strstr(p, "href=\"");
        const char *open_end = href ? strchr(href + 6, '>') : NULL;
        const char *close = open_end ? strstr(open_end + 1, "</a>") : NULL;
        if (!href || !open_end || !close) break;
        href += 6;
        const char *href_end = strchr(href, '"');
        if (!href_end || href_end > open_end) {
            p = close + 4;
            continue;
        }
        size_t path_len = (size_t)(href_end - href);
        if (path_len == 0 || path_len >= sizeof(out[count].detail_path) ||
            strstr(href, "txt") == NULL) {
            p = close + 4;
            continue;
        }
        memcpy(out[count].detail_path, href, path_len);
        out[count].detail_path[path_len] = '\0';
        copy_html_text(open_end + 1, close, out[count].title,
                       sizeof(out[count].title));

        const char *item_end = strstr(close, "</li>");
        const char *author = strstr(close, "<p class=\"author\">");
        if (author && item_end && author < item_end) {
            author = strchr(author, '>');
            const char *author_end = author ? strchr(author + 1, '<') : NULL;
            if (author && author_end) {
                author++;
                copy_html_text(author, author_end, out[count].author,
                               sizeof(out[count].author));
                char *colon = strstr(out[count].author, "：");
                if (colon) {
                    size_t prefix = (size_t)((colon + strlen("：")) -
                                             out[count].author);
                    memmove(out[count].author, out[count].author + prefix,
                            strlen(out[count].author + prefix) + 1);
                }
            }
        }
        if (out[count].title[0]) count++;
        p = close + 4;
    }
    return count;
}

static bool extract_mirror_url(const char *html, int mirror,
                               char *out, size_t cap)
{
    const char *host = mirror == 0 ? "https://d.txt80.la" :
                                     "https://d.txt80.com";
    const char *p = strstr(html, host);
    if (!p) return false;
    const char *end = p;
    while (*end && *end != '"' && *end != '\'' && *end != '<' &&
           !isspace((unsigned char)*end)) {
        end++;
    }
    char raw[320];
    size_t n = (size_t)(end - p);
    if (n == 0 || n >= sizeof(raw)) return false;
    memcpy(raw, p, n);
    raw[n] = '\0';
    return url_encode(raw, out, cap, ":/._~-%");
}

static bool extract_download_page(const char *html, char *out, size_t cap)
{
    const char *p = strstr(html, "href=\"/down/");
    if (!p) return false;
    p += strlen("href=\"");
    const char *end = strchr(p, '"');
    size_t n = end ? (size_t)(end - p) : 0;
    if (n == 0 || n >= cap) return false;
    memcpy(out, p, n);
    out[n] = '\0';
    return true;
}

static bool safe_filename(const char *input, char *out, size_t cap)
{
    if (!input || !input[0] || !out || cap < 6) return false;
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)input;
         *p && n + 1 < cap; p++) {
        unsigned char c = *p;
        if (c < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' ||
            c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            c = '_';
        }
        out[n++] = (char)c;
    }
    out[n] = '\0';
    trim_ascii(out);
    n = strlen(out);
    if (n < 4 || strcasecmp(out + n - 4, ".txt") != 0) {
        if (n + 4 >= cap) return false;
        memcpy(out + n, ".txt", 5);
    }
    return true;
}

static void search_task(void *arg)
{
    search_args_t *args = (search_args_t *)arg;
    char encoded[192];
    char body[280];
    txt_result_t parsed[TXT_RESULT_MAX] = {0};
    mkdir("/sdcard/.reader", 0775);

    if (!url_encode(args->query, encoded, sizeof(encoded), "-_.~")) {
        shared_update(TXT_STATE_FAILED, false, 0, 0, "搜索词过长");
        goto done;
    }
    snprintf(body, sizeof(body),
             "show=title%%2Csoftsay%%2Csoftwriter&keyboard=%s&tbname=download&tempid=1",
             encoded);
    esp_err_t err = http_to_file(TXT_SEARCH_URL, HTTP_METHOD_POST, body,
                                 SEARCH_TMP_PATH, false, NULL);
    if (err != ESP_OK) {
        char message[96];
        snprintf(message, sizeof(message), "TXT 网站搜索失败：%s",
                 esp_err_to_name(err));
        shared_update(TXT_STATE_FAILED, false, 0, 0, message);
        goto done;
    }
    size_t len = 0;
    char *html = read_file_prefix(SEARCH_TMP_PATH, SEARCH_PARSE_CAP, &len);
    remove(SEARCH_TMP_PATH);
    if (!html || len == 0) {
        free(html);
        shared_update(TXT_STATE_FAILED, false, 0, 0, "搜索结果内存不足");
        goto done;
    }
    int count = parse_search_results(html, parsed, TXT_RESULT_MAX);
    free(html);
    for (int i = 0; i < count; i++) {
        ESP_LOGI(TAG, "TXTRESULT %d title=%s author=%s path=%s", i + 1,
                 parsed[i].title, parsed[i].author, parsed[i].detail_path);
    }

    ensure_lock();
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(s_shared.results, 0, sizeof(s_shared.results));
    if (count > 0) {
        memcpy(s_shared.results, parsed, (size_t)count * sizeof(parsed[0]));
    }
    s_shared.count = count;
    s_shared.state = TXT_STATE_RESULTS;
    s_shared.running = false;
    s_shared.received = 0;
    s_shared.total = 0;
    s_shared.generation++;
    if (count > 0) {
        snprintf(s_shared.message, sizeof(s_shared.message),
                 "找到 %d 本整本 TXT，点击书名下载", count);
    } else {
        snprintf(s_shared.message, sizeof(s_shared.message),
                 "没有找到整本 TXT，请换个关键词");
    }
    if (s_lock) xSemaphoreGive(s_lock);

done:
    free(args);
    vTaskDelete(NULL);
}

bool txt_download_debug_search(const char *query)
{
    if (!query || !query[0] || !sd_storage_ready()) return false;
    txt_status_t status;
    shared_snapshot(&status);
    if (status.running) return false;
    search_args_t *args = calloc(1, sizeof(*args));
    if (!args) return false;
    snprintf(args->query, sizeof(args->query), "%s", query);
    shared_update(TXT_STATE_SEARCHING, true, 0, 0,
                  "串口调试：正在搜索 TXT...");
    if (xTaskCreate(search_task, "txt_search", 10 * 1024, args, 4, NULL) !=
        pdPASS) {
        free(args);
        shared_update(TXT_STATE_FAILED, false, 0, 0,
                      "串口搜索任务启动失败");
        return false;
    }
    return true;
}

static size_t local_file_size(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && st.st_size > 0 ? (size_t)st.st_size : 0;
}

static esp_err_t download_raw_once(const char *url, size_t offset,
                                   const char *mirror_name,
                                   size_t *cumulative_out)
{
    FILE *file = fopen(DOWNLOAD_RAW_PATH, offset > 0 ? "ab" : "wb");
    if (!file) return ESP_FAIL;
    http_sink_t sink = {
        .file = file,
        .report_progress = true,
        .transform_text = false,
        .base_received = offset,
        .total = -1,
    };
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 45000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_event,
        .user_data = &sink,
        .buffer_size = 2048,
        .buffer_size_tx = 512,
        .user_agent = "ESP32-TXT-Reader/2.1",
        .max_redirection_count = 2,
#if CONFIG_MBEDTLS_DYNAMIC_BUFFER
        /* The Cloudflare mirror is reachable from the device but ignores
         * Range. Hold its RX block for the whole response so heap
         * fragmentation cannot interrupt a long book. The nginx mirror keeps
         * dynamic RX because it supports byte-range resume. */
        .tls_dyn_buf_strategy = strstr(url, "d.txt80.la")
                                    ? HTTP_TLS_DYN_BUF_RX_STATIC
                                    : 0,
#endif
    };
    char message[112];
    if (offset > 0) {
        snprintf(message, sizeof(message), "%s断点续传：已保存 %u KB",
                 mirror_name, (unsigned)(offset / 1024));
    } else {
        snprintf(message, sizeof(message), "连接%s，准备整本下载...",
                 mirror_name);
    }
    shared_update(TXT_STATE_DOWNLOADING, true, offset, -1, message);
    ESP_LOGI(TAG, "raw download %s offset=%u heap free=%u largest=%u",
             mirror_name, (unsigned)offset,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_err_t err = client ? ESP_OK : ESP_ERR_NO_MEM;
    if (err == ESP_OK) {
        esp_http_client_set_header(client, "Accept-Encoding", "identity");
        if (offset > 0) {
            char range[48];
            snprintf(range, sizeof(range), "bytes=%u-", (unsigned)offset);
            esp_http_client_set_header(client, "Range", range);
        }
        err = esp_http_client_perform(client);
    }
    int status = client ? esp_http_client_get_status_code(client) : 0;
    if (client) esp_http_client_cleanup(client);
    if (fclose(file) != 0 && err == ESP_OK) err = ESP_FAIL;

    size_t cumulative = offset + sink.received;
    if (cumulative_out) *cumulative_out = cumulative;
    if (err == ESP_OK && offset == 0 && status != 200) {
        err = ESP_ERR_HTTP_CONNECT;
    } else if (err == ESP_OK && offset > 0 && status != 206) {
        /* A 200 response means the server ignored Range and appending it
         * would corrupt the book. The caller will restart or change source. */
        err = ESP_ERR_NOT_SUPPORTED;
        remove(DOWNLOAD_RAW_PATH);
        cumulative = 0;
        if (cumulative_out) *cumulative_out = 0;
    }
    if (err == ESP_OK && sink.received == 0) err = ESP_ERR_INVALID_SIZE;
    ESP_LOGI(TAG, "raw done status=%d chunk=%u cumulative=%u result=%s",
             status, (unsigned)sink.received, (unsigned)cumulative,
             esp_err_to_name(err));
    return err;
}

static esp_err_t convert_raw_book(const char *final_path)
{
    FILE *input = fopen(DOWNLOAD_RAW_PATH, "rb");
    if (!input) return ESP_FAIL;
    FILE *output = fopen(DOWNLOAD_TMP_PATH, "wb");
    if (!output) {
        fclose(input);
        return ESP_FAIL;
    }
    uint8_t buffer[512];
    size_t probe = fread(buffer, 1, sizeof(buffer), input);
    bool utf8 = text_looks_utf8(buffer, probe);
    rewind(input);
    ESP_LOGI(TAG, "TXT source encoding: %s", utf8 ? "UTF-8" : "GBK");

    size_t raw_size = local_file_size(DOWNLOAD_RAW_PATH);
    size_t converted = 0;
    uint8_t pending_gbk = 0;
    esp_err_t err = ESP_OK;
    while (err == ESP_OK) {
        size_t got = fread(buffer, 1, sizeof(buffer), input);
        if (got == 0) {
            if (ferror(input)) err = ESP_FAIL;
            break;
        }
        bool ok = utf8
                      ? fwrite(buffer, 1, got, output) == got
                      : gbk_write_utf8(output, &pending_gbk, buffer, got);
        if (!ok) {
            err = ESP_FAIL;
            break;
        }
        converted += got;
        if ((converted & 0xffff) < sizeof(buffer)) {
            char message[96];
            snprintf(message, sizeof(message), "正在转换文本编码：%u/%u KB",
                     (unsigned)(converted / 1024),
                     (unsigned)(raw_size / 1024));
            shared_update(TXT_STATE_DOWNLOADING, true, converted,
                          (int64_t)raw_size, message);
        }
    }
    if (err == ESP_OK && !utf8 &&
        !gbk_write_utf8_finish(output, &pending_gbk)) {
        err = ESP_FAIL;
    }
    if (fclose(input) != 0 && err == ESP_OK) err = ESP_FAIL;
    if (fclose(output) != 0 && err == ESP_OK) err = ESP_FAIL;
    if (err == ESP_OK) {
        remove(final_path);
        if (rename(DOWNLOAD_TMP_PATH, final_path) != 0) err = ESP_FAIL;
    }
    if (err == ESP_OK) remove(DOWNLOAD_RAW_PATH);
    else remove(DOWNLOAD_TMP_PATH);
    return err;
}

static void download_task(void *arg)
{
    download_args_t *args = (download_args_t *)arg;
    char detail_url[256];
    char download_page[160] = "";
    char *html = NULL;
    char mirror_urls[2][384] = {{0}};
    char filename[112];
    char final_path[180];
    mkdir("/sdcard/.reader", 0775);
    mkdir("/sdcard/novels", 0775);
    remove(DOWNLOAD_RAW_PATH);
    remove(DOWNLOAD_TMP_PATH);

    snprintf(detail_url, sizeof(detail_url), "%s%s", TXT_SITE_URL,
             args->book.detail_path);
    shared_update(TXT_STATE_RESOLVING, true, 0, 0,
                  "正在获取整本 TXT 下载地址...");
    esp_err_t err = http_to_file(detail_url, HTTP_METHOD_GET, NULL,
                                 DETAIL_TMP_PATH, false, NULL);
    if (err != ESP_OK) goto failed;
    size_t len = 0;
    html = read_file_prefix(DETAIL_TMP_PATH, SEARCH_PARSE_CAP, &len);
    remove(DETAIL_TMP_PATH);
    if (!html || len == 0) {
        err = ESP_ERR_NO_MEM;
        goto failed;
    }
    (void)extract_mirror_url(html, 0, mirror_urls[0], sizeof(mirror_urls[0]));
    (void)extract_mirror_url(html, 1, mirror_urls[1], sizeof(mirror_urls[1]));
    if (!mirror_urls[0][0] && !mirror_urls[1][0]) {
        (void)extract_download_page(html, download_page,
                                    sizeof(download_page));
    }
    free(html);
    html = NULL;
    if (!mirror_urls[0][0] && !mirror_urls[1][0] && download_page[0]) {
        snprintf(detail_url, sizeof(detail_url), "%s%s", TXT_SITE_URL,
                 download_page);
        shared_update(TXT_STATE_RESOLVING, true, 0, 0,
                      "正在解析两个整本 TXT 下载源...");
        err = http_to_file(detail_url, HTTP_METHOD_GET, NULL,
                           DETAIL_TMP_PATH, false, NULL);
        if (err != ESP_OK) goto failed;
        html = read_file_prefix(DETAIL_TMP_PATH, SEARCH_PARSE_CAP, &len);
        remove(DETAIL_TMP_PATH);
        if (!html || len == 0) {
            err = ESP_ERR_NO_MEM;
            goto failed;
        }
        (void)extract_mirror_url(html, 0, mirror_urls[0],
                                 sizeof(mirror_urls[0]));
        (void)extract_mirror_url(html, 1, mirror_urls[1],
                                 sizeof(mirror_urls[1]));
        free(html);
        html = NULL;
    }
    if (!mirror_urls[0][0] && !mirror_urls[1][0]) {
        err = ESP_ERR_NOT_FOUND;
        goto failed;
    }
    if (!safe_filename(args->book.title, filename, sizeof(filename))) {
        err = ESP_ERR_INVALID_ARG;
        goto failed;
    }
    snprintf(final_path, sizeof(final_path), "/sdcard/novels/%s", filename);

    int first = args->preferred_mirror == 1 ? 1 : 0;
    int second = 1 - first;
    int active = mirror_urls[first][0] ? first : second;
    size_t cumulative = 0;
    err = mirror_urls[active][0]
              ? download_raw_once(mirror_urls[active], 0,
                                  active == 0 ? "下载源1" : "下载源2",
                                  &cumulative)
              : ESP_ERR_NOT_FOUND;

    /* Source 2 supports byte ranges. If source 1 or the network drops, keep
     * the raw bytes already on TF and continue from that exact offset. */
    for (int retry = 1; err != ESP_OK && retry <= 8; retry++) {
        int resume_source = mirror_urls[1][0] ? 1 : second;
        size_t offset = local_file_size(DOWNLOAD_RAW_PATH);
        if (resume_source == 0 && offset > 0) {
            remove(DOWNLOAD_RAW_PATH);
            offset = 0;
        }
        char retry_message[112];
        snprintf(retry_message, sizeof(retry_message),
                 "网络中断，第%d次续传（已保存 %u KB）", retry,
                 (unsigned)(offset / 1024));
        shared_update(TXT_STATE_RESOLVING, true, offset, -1, retry_message);
        vTaskDelay(pdMS_TO_TICKS(800));
        err = mirror_urls[resume_source][0]
                  ? download_raw_once(mirror_urls[resume_source], offset,
                                      resume_source == 0 ? "下载源1" :
                                                           "下载源2",
                                      &cumulative)
                  : ESP_ERR_NOT_FOUND;
    }
    if (err == ESP_OK) {
        shared_update(TXT_STATE_DOWNLOADING, true, 0,
                      (int64_t)cumulative, "下载完成，正在转换文本编码...");
        err = convert_raw_book(final_path);
    }
    if (err == ESP_OK) {
        size_t final_size = local_file_size(final_path);
        char message[128];
        snprintf(message, sizeof(message), "下载完成：%.70s（%u KB）",
                 filename, (unsigned)(final_size / 1024));
        shared_update(TXT_STATE_DONE, false, final_size,
                      (int64_t)final_size, message);
        ESP_LOGI(TAG, "saved %s (%u bytes)", final_path,
                 (unsigned)final_size);
        free(args);
        vTaskDelete(NULL);
        return;
    }

failed:
    free(html);
    remove(DETAIL_TMP_PATH);
    remove(DOWNLOAD_TMP_PATH);
    char message[112];
    snprintf(message, sizeof(message), "整本 TXT 下载失败：%s",
             esp_err_to_name(err));
    shared_update(TXT_STATE_FAILED, false, 0, 0, message);
    ESP_LOGE(TAG, "%s", message);
    free(args);
    vTaskDelete(NULL);
}

bool txt_download_debug_book(const char *detail_path, const char *title,
                             int preferred_mirror)
{
    if (!detail_path || detail_path[0] != '/' || !title || !title[0] ||
        !sd_storage_ready()) {
        return false;
    }
    txt_status_t status;
    shared_snapshot(&status);
    if (status.running) return false;
    download_args_t *args = calloc(1, sizeof(*args));
    if (!args) return false;
    snprintf(args->book.detail_path, sizeof(args->book.detail_path), "%s",
             detail_path);
    snprintf(args->book.title, sizeof(args->book.title), "%s", title);
    args->preferred_mirror = preferred_mirror == 1 ? 1 : 0;
    release_input_ui();
    if (s_results_box) {
        lv_obj_clean(s_results_box);
        s_ui_result_count = 0;
    }
    shared_update(TXT_STATE_RESOLVING, true, 0, 0,
                  "串口调试：正在解析整本 TXT...");
    if (xTaskCreate(download_task, "txt_download", 10 * 1024, args, 4,
                    NULL) != pdPASS) {
        free(args);
        shared_update(TXT_STATE_FAILED, false, 0, 0,
                      "串口下载任务启动失败");
        return false;
    }
    return true;
}

static void hide_keyboard(void)
{
    if (s_keyboard) lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
#if LV_USE_IME_PINYIN
    if (s_ime) {
        lv_obj_t *cand = lv_ime_pinyin_get_cand_panel(s_ime);
        if (cand) lv_obj_add_flag(cand, LV_OBJ_FLAG_HIDDEN);
    }
#endif
    if (s_status_label) lv_obj_remove_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);
    if (s_results_box) lv_obj_remove_flag(s_results_box, LV_OBJ_FLAG_HIDDEN);
    if (s_progress) lv_obj_remove_flag(s_progress, LV_OBJ_FLAG_HIDDEN);
}

static void show_keyboard(void)
{
    if (!s_keyboard) return;
    lv_keyboard_set_textarea(s_keyboard, s_query);
    lv_obj_remove_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
#if LV_USE_IME_PINYIN
    if (s_ime) {
        lv_obj_t *cand = lv_ime_pinyin_get_cand_panel(s_ime);
        if (cand) {
            lv_obj_remove_flag(cand, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(cand);
        }
    }
#endif
    lv_obj_move_foreground(s_keyboard);
    lv_obj_add_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_results_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_progress, LV_OBJ_FLAG_HIDDEN);
}

static void release_input_ui(void)
{
#if LV_USE_IME_PINYIN
    if (s_ime) {
        /* The IME destructor also deletes its keyboard and candidate panel. */
        lv_obj_delete(s_ime);
        s_ime = NULL;
        s_keyboard = NULL;
        return;
    }
#endif
    if (s_keyboard) {
        lv_obj_delete(s_keyboard);
        s_keyboard = NULL;
    }
}

static void query_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) show_keyboard();
}

static void start_search(void)
{
    txt_status_t status;
    shared_snapshot(&status);
    if (status.running) return;
    const char *query = lv_textarea_get_text(s_query);
    if (!query || !query[0]) {
        lv_label_set_text(s_status_label, "请输入书名或作者");
        return;
    }
    if (!sd_storage_ready()) {
        lv_label_set_text(s_status_label, "未检测到 TF 卡");
        return;
    }
    search_args_t *args = calloc(1, sizeof(*args));
    if (!args) {
        lv_label_set_text(s_status_label, "内存不足，无法搜索");
        return;
    }
    snprintf(args->query, sizeof(args->query), "%s", query);
    hide_keyboard();
    lv_obj_clean(s_results_box);
    s_rendered_generation = 0;
    shared_update(TXT_STATE_SEARCHING, true, 0, 0,
                  "正在 TXT 下载网站搜索...");
    if (xTaskCreate(search_task, "txt_search", 10 * 1024, args, 4, NULL) !=
        pdPASS) {
        free(args);
        shared_update(TXT_STATE_FAILED, false, 0, 0,
                      "内存不足，无法启动搜索");
    }
}

static void search_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) start_search();
}

static void keyboard_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_READY) {
        start_search();
    } else if (lv_event_get_code(e) == LV_EVENT_CANCEL) {
        hide_keyboard();
    }
}

static void result_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    txt_status_t status;
    shared_snapshot(&status);
    if (status.running) return;
    intptr_t index = (intptr_t)lv_event_get_user_data(e);
    if (index < 0 || index >= s_ui_result_count) return;
    if (!sd_storage_ready()) {
        lv_label_set_text(s_status_label, "未检测到 TF 卡");
        return;
    }
    download_args_t *args = calloc(1, sizeof(*args));
    if (!args) {
        lv_label_set_text(s_status_label, "内存不足，无法下载");
        return;
    }
    args->book = s_ui_results[index];
    args->preferred_mirror = (int)lv_dropdown_get_selected(s_source_dropdown);
    release_input_ui();
    /* The result buttons are no longer needed during a whole-book download.
     * Releasing them before TLS setup leaves a larger contiguous RX block. */
    lv_obj_clean(s_results_box);
    s_ui_result_count = 0;
    shared_update(TXT_STATE_RESOLVING, true, 0, 0,
                  "正在获取整本 TXT 下载地址...");
    if (xTaskCreate(download_task, "txt_download", 10 * 1024, args, 4, NULL) !=
        pdPASS) {
        free(args);
        shared_update(TXT_STATE_FAILED, false, 0, 0,
                      "内存不足，无法启动下载");
    }
}

static void render_results(const txt_status_t *status)
{
    lv_obj_clean(s_results_box);
    s_ui_result_count = status->count;
    memcpy(s_ui_results, status->results, sizeof(s_ui_results));
    for (int i = 0; i < status->count && i < TXT_RESULT_MAX; i++) {
        lv_obj_t *button = lv_button_create(s_results_box);
        lv_obj_set_width(button, LV_PCT(100));
        lv_obj_set_height(button, 42);
        lv_obj_set_style_radius(button, 8, 0);
        lv_obj_set_style_bg_color(button, lv_color_hex(0xfffdf7), 0);
        lv_obj_set_style_border_width(button, 1, 0);
        lv_obj_set_style_border_color(button, lv_color_hex(0xe4dccd), 0);
        lv_obj_set_style_shadow_width(button, 0, 0);
        lv_obj_add_event_cb(button, result_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        lv_obj_t *label = lv_label_create(button);
        lv_obj_set_style_text_font(label, &novel_font_16, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(0x29231d), 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(label, 278);
        char line[152];
        if (status->results[i].author[0]) {
            snprintf(line, sizeof(line), "%s · %s",
                     status->results[i].title, status->results[i].author);
        } else {
            snprintf(line, sizeof(line), "%s", status->results[i].title);
        }
        lv_label_set_text(label, line);
        lv_obj_center(label);
    }
}

static void home_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_keyboard();
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    main_menu_ui_show(lv_display_get_default());
}

static void poll_cb(lv_timer_t *timer)
{
    (void)timer;
    txt_status_t status;
    shared_snapshot(&status);
    if (s_status_label) lv_label_set_text(s_status_label, status.message);
    if (s_search_button) {
        if (status.running) lv_obj_add_state(s_search_button, LV_STATE_DISABLED);
        else lv_obj_remove_state(s_search_button, LV_STATE_DISABLED);
    }
    int progress = 0;
    if (status.total > 0) {
        progress = (int)(status.received * 100 / (size_t)status.total);
        if (progress > 100) progress = 100;
    }
    if (status.state == TXT_STATE_DONE) progress = 100;
    if (s_progress) lv_bar_set_value(s_progress, progress, LV_ANIM_ON);
    if (status.state == TXT_STATE_RESULTS &&
        status.generation != s_rendered_generation) {
        render_results(&status);
        s_rendered_generation = status.generation;
    }
}

void txt_download_ui_show(lv_display_t *disp)
{
    (void)disp;
    txt_status_t initial;
    shared_snapshot(&initial);
    s_rendered_generation = 0;
    s_ui_result_count = 0;

    lv_obj_t *root = lv_obj_create(NULL);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, 320, 240);
    lv_obj_set_style_bg_color(root, lv_color_hex(0xf4efe5), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    lv_obj_t *header = lv_obj_create(root);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, 320, 38);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x7c3f1d), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);

    lv_obj_t *home = lv_button_create(header);
    lv_obj_set_pos(home, 6, 5);
    lv_obj_set_size(home, 52, 28);
    lv_obj_set_style_radius(home, 8, 0);
    lv_obj_add_event_cb(home, home_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *home_label = lv_label_create(home);
    lv_obj_set_style_text_font(home_label, &novel_font_16, 0);
    lv_label_set_text(home_label, "主页");
    lv_obj_center(home_label);

    lv_obj_t *title = lv_label_create(header);
    lv_obj_set_style_text_font(title, &novel_font_16, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_label_set_text(title, "搜索整本 TXT");
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 68, 0);

    s_source_dropdown = lv_dropdown_create(header);
    lv_obj_set_size(s_source_dropdown, 82, 28);
    lv_obj_align(s_source_dropdown, LV_ALIGN_RIGHT_MID, -6, 0);
    lv_obj_set_style_text_font(s_source_dropdown, &novel_font_16, 0);
    lv_obj_set_style_pad_top(s_source_dropdown, 4, 0);
    lv_dropdown_set_options(s_source_dropdown, "源1\n源2");

    s_query = lv_textarea_create(root);
    lv_obj_set_pos(s_query, 10, 46);
    lv_obj_set_size(s_query, 229, 38);
    lv_obj_set_style_text_font(s_query, &novel_font_16, 0);
    lv_obj_set_style_radius(s_query, 9, 0);
    lv_obj_set_style_bg_color(s_query, lv_color_hex(0xfffdf7), 0);
    lv_obj_set_style_border_color(s_query, lv_color_hex(0xcfc4b2), 0);
    lv_obj_set_style_pad_top(s_query, 7, 0);
    lv_textarea_set_one_line(s_query, true);
    lv_textarea_set_max_length(s_query, 63);
    lv_textarea_set_placeholder_text(s_query, "输入书名或作者（支持拼音）");
    lv_obj_add_event_cb(s_query, query_cb, LV_EVENT_CLICKED, NULL);

    s_search_button = lv_button_create(root);
    lv_obj_set_pos(s_search_button, 246, 46);
    lv_obj_set_size(s_search_button, 64, 38);
    lv_obj_set_style_radius(s_search_button, 9, 0);
    lv_obj_set_style_bg_color(s_search_button, lv_color_hex(0xc9783b), 0);
    lv_obj_add_event_cb(s_search_button, search_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *search_label = lv_label_create(s_search_button);
    lv_obj_set_style_text_font(search_label, &novel_font_16, 0);
    lv_obj_set_style_text_color(search_label, lv_color_white(), 0);
    lv_label_set_text(search_label, "搜索");
    lv_obj_center(search_label);

    s_status_label = lv_label_create(root);
    lv_obj_set_style_text_font(s_status_label, &novel_font_16, 0);
    lv_obj_set_pos(s_status_label, 12, 88);
    lv_obj_set_width(s_status_label, 296);
    lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_CLIP);
    lv_label_set_text(s_status_label, initial.message);

    s_progress = lv_bar_create(root);
    lv_obj_set_pos(s_progress, 10, 108);
    lv_obj_set_size(s_progress, 300, 5);
    lv_bar_set_range(s_progress, 0, 100);

    s_results_box = lv_obj_create(root);
    lv_obj_remove_style_all(s_results_box);
    lv_obj_set_pos(s_results_box, 8, 117);
    lv_obj_set_size(s_results_box, 304, 118);
    lv_obj_set_flex_flow(s_results_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_results_box, 5, 0);
    lv_obj_set_style_pad_hor(s_results_box, 2, 0);
    lv_obj_set_scroll_dir(s_results_box, LV_DIR_VER);

    s_keyboard = lv_keyboard_create(root);
    lv_obj_set_size(s_keyboard, 320, 122);
    lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(s_keyboard, &novel_font_16, 0);
    lv_keyboard_set_textarea(s_keyboard, s_query);
    lv_obj_add_event_cb(s_keyboard, keyboard_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_keyboard, keyboard_cb, LV_EVENT_CANCEL, NULL);
#if LV_USE_IME_PINYIN
    s_ime = lv_ime_pinyin_create(root);
    lv_obj_set_style_text_font(s_ime, &novel_font_16, 0);
    lv_ime_pinyin_set_dict(s_ime, (lv_pinyin_dict_t *)novel_pinyin_dict);
    lv_ime_pinyin_set_keyboard(s_ime, s_keyboard);
    lv_ime_pinyin_set_mode(s_ime, LV_IME_PINYIN_MODE_K26);
    lv_obj_t *cand = lv_ime_pinyin_get_cand_panel(s_ime);
    lv_obj_set_size(cand, 320, 32);
    lv_obj_align_to(cand, s_keyboard, LV_ALIGN_OUT_TOP_MID, 0, 0);
    lv_obj_set_style_text_font(cand, &novel_font_16, 0);
#else
    s_ime = NULL;
#endif
    hide_keyboard();
    if (initial.state == TXT_STATE_RESULTS) {
        render_results(&initial);
        s_rendered_generation = initial.generation;
    }

    if (s_timer) lv_timer_delete(s_timer);
    s_timer = lv_timer_create(poll_cb, 250, NULL);
    poll_cb(s_timer);
    lv_obj_t *old = lv_screen_active();
    lv_screen_load(root);
    if (old && old != root) lv_obj_delete_async(old);
}
