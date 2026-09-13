/* SPDX-FileCopyrightText: 2025 */
/* SPDX-License-Identifier: MIT */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "wifi_loader.h"

static const char *TAG = "wifi_loader";

#define WIFI_AP_SSID      "ESP32-Reader"
#define WIFI_AP_PASS      "12345678"
#define PC_SERVER_IP      "192.168.4.2"
#define PC_SERVER_PORT    8000
#define HTTP_TIMEOUT_MS   5000

static char *http_get_body(const char *url, size_t *out_len)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return NULL;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "open %s failed: %s", url, esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return NULL;
    }

    int content_length = esp_http_client_fetch_headers(client);
    if (content_length <= 0) {
        ESP_LOGW(TAG, "bad content length for %s", url);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return NULL;
    }

    char *buf = malloc((size_t)content_length + 1);
    if (!buf) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return NULL;
    }

    int total = 0;
    while (total < content_length) {
        int rd = esp_http_client_read(client, buf + total, (size_t)(content_length - total));
        if (rd <= 0) {
            break;
        }
        total += rd;
    }
    buf[total] = '\0';

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (total <= 0) {
        free(buf);
        return NULL;
    }

    if (out_len) {
        *out_len = (size_t)total;
    }
    return buf;
}

static void mount_spiffs_if_needed(void)
{
    if (esp_spiffs_mounted("spiffs")) {
        return;
    }

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "spiffs",
        .max_files = 4,
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "SPIFFS mounted for online download");
    }
}

static void start_ap(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = WIFI_AP_SSID,
            .password = WIFI_AP_PASS,
            .max_connection = 1,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "AP started: %s / %s", WIFI_AP_SSID, WIFI_AP_PASS);
}

esp_err_t wifi_loader_try_load_first_n(int max_chapters)
{
    /* First start AP. The PC must connect to this AP and run:
       python tools/serve_novel.py --port 8000 */
    start_ap();

    /* Wait a bit for clients to associate */
    vTaskDelay(pdMS_TO_TICKS(3000));

    char url[128];
    snprintf(url, sizeof(url), "http://%s:%d/health", PC_SERVER_IP, PC_SERVER_PORT);
    size_t ignored = 0;
    char *health = http_get_body(url, &ignored);
    if (!health) {
        ESP_LOGW(TAG, "PC server not reachable at %s; keeping local SPIFFS chapters", url);
        return ESP_ERR_NOT_FOUND;
    }
    free(health);

    mount_spiffs_if_needed();

    int downloaded = 0;
    for (int i = 1; i <= max_chapters; i++) {
        snprintf(url, sizeof(url), "http://%s:%d/chapter?n=%d", PC_SERVER_IP, PC_SERVER_PORT, i);
        size_t len = 0;
        char *body = http_get_body(url, &len);
        if (!body) {
            ESP_LOGW(TAG, "Failed to download chapter %d", i);
            break;
        }

        char path[32];
        snprintf(path, sizeof(path), "/spiffs/%03d.txt", i);
        FILE *f = fopen(path, "wb");
        if (f) {
            fwrite(body, 1, len, f);
            fclose(f);
            downloaded++;
        } else {
            ESP_LOGE(TAG, "Cannot write %s", path);
        }
        free(body);
    }

    ESP_LOGI(TAG, "Downloaded %d chapters from PC server", downloaded);
    return downloaded > 0 ? ESP_OK : ESP_FAIL;
}
