/* SPDX-License-Identifier: MIT */

#include "sd_storage.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "sdmmc_cmd.h"

#define SD_HOST SPI3_HOST
#define PIN_SD_MOSI 23
#define PIN_SD_MISO 19
#define PIN_SD_CLK  18
#define PIN_SD_CS   5
#define SD_MOUNT_POINT "/sdcard"
#define SD_ONLINE_DIR   "/sdcard/online"
#define SD_INDEX_DIR    "/sdcard/.reader"
#define SD_INDEX_MAGIC  0x52494458u
#define SD_INDEX_VERSION 1u
#define SD_INTERNAL_PATH_SIZE 320

static const char *TAG = "sd_storage";
static SemaphoreHandle_t s_lock;
static bool s_bus_ready;
static bool s_mounted;
static sdmmc_card_t *s_card;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t key;
    uint32_t file_size;
} page_index_header_t;

typedef struct {
    int32_t chapter;
    uint32_t offset;
} sd_progress_t;

static void storage_lock(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void storage_unlock(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

static bool ensure_dir(const char *path)
{
    return mkdir(path, 0775) == 0 || errno == EEXIST;
}

static bool join_path(char *out, size_t capacity, const char *directory,
                      const char *name)
{
    size_t dir_len = strlen(directory);
    size_t name_len = strlen(name);
    if (dir_len + 1 + name_len + 1 > capacity) {
        return false;
    }
    memcpy(out, directory, dir_len);
    out[dir_len] = '/';
    memcpy(out + dir_len + 1, name, name_len + 1);
    return true;
}

static bool append_suffix(char *out, size_t capacity, const char *path,
                          const char *suffix)
{
    size_t path_len = strlen(path);
    size_t suffix_len = strlen(suffix);
    if (path_len + suffix_len + 1 > capacity) {
        return false;
    }
    memcpy(out, path, path_len);
    memcpy(out + path_len, suffix, suffix_len + 1);
    return true;
}

static esp_err_t mount_locked(void)
{
    if (s_mounted) {
        return ESP_OK;
    }
    if (!s_bus_ready) {
        spi_bus_config_t bus = {
            .mosi_io_num = PIN_SD_MOSI,
            .miso_io_num = PIN_SD_MISO,
            .sclk_io_num = PIN_SD_CLK,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = 4096,
        };
        esp_err_t err = spi_bus_initialize(SD_HOST, &bus, SPI_DMA_CH_AUTO);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(err));
            return err;
        }
        s_bus_ready = true;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_HOST;
    host.max_freq_khz = 10000;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = PIN_SD_CS;
    slot.host_id = SD_HOST;
    esp_vfs_fat_sdmmc_mount_config_t config = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };
    esp_err_t err = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot,
                                             &config, &s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "TF card unavailable: %s", esp_err_to_name(err));
        return err;
    }
    s_mounted = true;
    ensure_dir(SD_ONLINE_DIR);
    ensure_dir(SD_INDEX_DIR);
    uint64_t capacity = (uint64_t)s_card->csd.capacity * s_card->csd.sector_size;
    ESP_LOGI(TAG, "TF card mounted: %llu MiB",
             (unsigned long long)(capacity / (1024 * 1024)));
    return ESP_OK;
}

esp_err_t sd_storage_init(void)
{
    storage_lock();
    esp_err_t err = mount_locked();
    storage_unlock();
    return err;
}

bool sd_storage_ready(void)
{
    return s_mounted;
}

static bool has_txt_extension(const char *name)
{
    size_t len = name ? strlen(name) : 0;
    return len > 4 && strcasecmp(name + len - 4, ".txt") == 0;
}

static void title_from_filename(const char *name, char *out, size_t cap)
{
    snprintf(out, cap, "%s", name ? name : "TXT 小说");
    char *dot = strrchr(out, '.');
    if (dot) {
        *dot = '\0';
    }
}

static void scan_text_dir(const char *directory, sd_book_entry_t *out,
                          size_t capacity, size_t *count)
{
    DIR *dir = opendir(directory);
    if (!dir) {
        return;
    }
    struct dirent *entry;
    while (*count < capacity && (entry = readdir(dir)) != NULL) {
        if (!has_txt_extension(entry->d_name)) {
            continue;
        }
        char path[SD_STORAGE_PATH_SIZE];
        bool path_ok = join_path(path, sizeof(path), directory, entry->d_name);
        struct stat st;
        if (!path_ok ||
            stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size == 0) {
            continue;
        }
        sd_book_entry_t *book = &out[(*count)++];
        memset(book, 0, sizeof(*book));
        book->type = SD_BOOK_TEXT;
        title_from_filename(entry->d_name, book->title, sizeof(book->title));
        snprintf(book->path, sizeof(book->path), "%s", path);
    }
    closedir(dir);
}

static int count_archive_chapters(const char *directory)
{
    int count = 0;
    DIR *dir = opendir(directory);
    if (!dir) {
        return 0;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        int chapter = 0;
        char extra = '\0';
        if (sscanf(entry->d_name, "%d.txt%c", &chapter, &extra) == 1 &&
            chapter > 0) {
            count++;
        }
    }
    closedir(dir);
    return count;
}

static void scan_archives(sd_book_entry_t *out, size_t capacity, size_t *count)
{
    DIR *dir = opendir(SD_ONLINE_DIR);
    if (!dir) {
        return;
    }
    struct dirent *entry;
    while (*count < capacity && (entry = readdir(dir)) != NULL) {
        char *end = NULL;
        unsigned long id = strtoul(entry->d_name, &end, 10);
        if (!entry->d_name[0] || !end || *end != '\0' || id == 0 ||
            id > UINT32_MAX) {
            continue;
        }
        char directory[SD_INTERNAL_PATH_SIZE];
        if (!join_path(directory, sizeof(directory), SD_ONLINE_DIR,
                       entry->d_name)) {
            continue;
        }
        struct stat st;
        if (stat(directory, &st) != 0 || !S_ISDIR(st.st_mode)) {
            continue;
        }
        int chapters = count_archive_chapters(directory);
        size_t directory_len = strlen(directory);
        if (chapters <= 0 || directory_len >= sizeof(out[0].path)) {
            continue;
        }
        sd_book_entry_t *book = &out[(*count)++];
        memset(book, 0, sizeof(*book));
        book->type = SD_BOOK_ARCHIVE;
        book->book_id = (uint32_t)id;
        book->chapter_count = chapters;
        memcpy(book->path, directory, directory_len + 1);
        char meta_path[SD_INTERNAL_PATH_SIZE];
        if (!join_path(meta_path, sizeof(meta_path), directory, "book.meta")) {
            (*count)--;
            continue;
        }
        FILE *meta = fopen(meta_path, "rb");
        if (meta) {
            if (fgets(book->title, sizeof(book->title), meta)) {
                book->title[strcspn(book->title, "\r\n")] = '\0';
            }
            fclose(meta);
        }
        if (!book->title[0]) {
            snprintf(book->title, sizeof(book->title), "在线书籍 %lu", id);
        }
    }
    closedir(dir);
}

esp_err_t sd_storage_list_books(sd_book_entry_t *out, size_t capacity,
                                size_t *out_count)
{
    if (!out || capacity == 0 || !out_count) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_count = 0;
    storage_lock();
    esp_err_t err = mount_locked();
    if (err == ESP_OK) {
        scan_archives(out, capacity, out_count);
        scan_text_dir(SD_MOUNT_POINT, out, capacity, out_count);
        scan_text_dir("/sdcard/novels", out, capacity, out_count);
    }
    storage_unlock();
    return err;
}

static bool copy_file_atomic(const char *source, const char *destination)
{
    char temporary[SD_INTERNAL_PATH_SIZE];
    if (!append_suffix(temporary, sizeof(temporary), destination, ".tmp")) {
        return false;
    }
    FILE *in = fopen(source, "rb");
    FILE *out = in ? fopen(temporary, "wb") : NULL;
    if (!in || !out) {
        if (in) fclose(in);
        if (out) fclose(out);
        remove(temporary);
        return false;
    }
    char buffer[1024];
    bool ok = true;
    size_t got;
    while ((got = fread(buffer, 1, sizeof(buffer), in)) > 0) {
        if (fwrite(buffer, 1, got, out) != got) {
            ok = false;
            break;
        }
    }
    if (ferror(in) || fflush(out) != 0 || fsync(fileno(out)) != 0) {
        ok = false;
    }
    fclose(in);
    if (fclose(out) != 0) {
        ok = false;
    }
    if (ok) {
        (void)remove(destination);
        ok = rename(temporary, destination) == 0;
    }
    if (!ok) {
        remove(temporary);
    }
    return ok;
}

esp_err_t sd_storage_archive_chapter(uint32_t book_id, const char *title,
                                     int chapter_number,
                                     const char *source_path)
{
    if (book_id == 0 || !title || !source_path || chapter_number < 1) {
        return ESP_ERR_INVALID_ARG;
    }
    storage_lock();
    esp_err_t err = mount_locked();
    if (err != ESP_OK) {
        storage_unlock();
        return err;
    }
    char directory[SD_INTERNAL_PATH_SIZE];
    snprintf(directory, sizeof(directory), "%s/%lu", SD_ONLINE_DIR,
             (unsigned long)book_id);
    if (!ensure_dir(SD_ONLINE_DIR) || !ensure_dir(directory)) {
        storage_unlock();
        return ESP_FAIL;
    }
    char meta_path[SD_INTERNAL_PATH_SIZE];
    char meta_tmp[SD_INTERNAL_PATH_SIZE];
    if (!join_path(meta_path, sizeof(meta_path), directory, "book.meta") ||
        !join_path(meta_tmp, sizeof(meta_tmp), directory, ".book.meta.tmp")) {
        storage_unlock();
        return ESP_ERR_INVALID_SIZE;
    }
    FILE *meta = fopen(meta_tmp, "wb");
    bool meta_ok = meta && fprintf(meta, "%s\n", title) > 0 &&
                   fflush(meta) == 0 && fsync(fileno(meta)) == 0;
    if (meta && fclose(meta) != 0) {
        meta_ok = false;
    }
    if (meta_ok) {
        (void)remove(meta_path);
        meta_ok = rename(meta_tmp, meta_path) == 0;
    }
    if (!meta_ok) {
        remove(meta_tmp);
        storage_unlock();
        return ESP_FAIL;
    }
    char destination[SD_INTERNAL_PATH_SIZE];
    char chapter_name[24];
    snprintf(chapter_name, sizeof(chapter_name), "%06d.txt", chapter_number);
    if (!join_path(destination, sizeof(destination), directory, chapter_name)) {
        storage_unlock();
        return ESP_ERR_INVALID_SIZE;
    }
    bool copied = copy_file_atomic(source_path, destination);
    storage_unlock();
    if (!copied) {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "archived book %lu chapter %d", (unsigned long)book_id,
             chapter_number);
    return ESP_OK;
}

esp_err_t sd_storage_read(const char *path, uint32_t offset, void *buffer,
                          size_t capacity, size_t *out_read,
                          uint32_t *out_file_size)
{
    if (!path || !buffer || capacity == 0 || !out_read) {
        return ESP_ERR_INVALID_ARG;
    }
    storage_lock();
    struct stat st;
    if (!s_mounted || stat(path, &st) != 0 || st.st_size < 0 ||
        (uint64_t)st.st_size > UINT32_MAX || offset > (uint32_t)st.st_size) {
        storage_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    FILE *file = fopen(path, "rb");
    if (!file || fseek(file, (long)offset, SEEK_SET) != 0) {
        if (file) fclose(file);
        storage_unlock();
        return ESP_FAIL;
    }
    *out_read = fread(buffer, 1, capacity, file);
    bool ok = !ferror(file);
    fclose(file);
    if (out_file_size) {
        *out_file_size = (uint32_t)st.st_size;
    }
    storage_unlock();
    return ok ? ESP_OK : ESP_FAIL;
}

uint32_t sd_storage_book_key(const char *path)
{
    uint32_t hash = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)path; p && *p; p++) {
        hash = (hash ^ *p) * 16777619u;
    }
    return hash ? hash : 1u;
}

static void progress_key(const char *path, char key[12])
{
    snprintf(key, 12, "p%08lx", (unsigned long)sd_storage_book_key(path));
}

esp_err_t sd_storage_load_progress(const char *path, int *chapter,
                                   uint32_t *offset)
{
    if (!path || !chapter || !offset) {
        return ESP_ERR_INVALID_ARG;
    }
    *chapter = 1;
    *offset = 0;
    char key[12];
    progress_key(path, key);
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open("sd_reader", NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    sd_progress_t progress;
    size_t size = sizeof(progress);
    err = nvs_get_blob(nvs, key, &progress, &size);
    nvs_close(nvs);
    if (err == ESP_OK && size == sizeof(progress)) {
        *chapter = progress.chapter > 0 ? progress.chapter : 1;
        *offset = progress.offset;
    }
    return err;
}

esp_err_t sd_storage_save_progress(const char *path, int chapter,
                                   uint32_t offset)
{
    if (!path || !path[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    char key[12];
    progress_key(path, key);
    sd_progress_t progress = {
        .chapter = chapter > 0 ? chapter : 1,
        .offset = offset,
    };
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open("sd_reader", NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_blob(nvs, key, &progress, sizeof(progress));
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    if (nvs) {
        nvs_close(nvs);
    }
    return err;
}

static void index_path(uint32_t key, char path[64])
{
    snprintf(path, 64, SD_INDEX_DIR "/%08lx.idx", (unsigned long)key);
}

esp_err_t sd_storage_prepare_page_index(const char *path, uint32_t file_size,
                                        uint32_t *out_key,
                                        uint32_t *out_page_count)
{
    if (!path || !out_key || !out_page_count) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t key = sd_storage_book_key(path);
    char idx_path[64];
    index_path(key, idx_path);
    page_index_header_t expected = {
        .magic = SD_INDEX_MAGIC,
        .version = SD_INDEX_VERSION,
        .key = key,
        .file_size = file_size,
    };
    storage_lock();
    ensure_dir(SD_INDEX_DIR);
    bool valid = false;
    uint32_t count = 0;
    struct stat st;
    FILE *file = fopen(idx_path, "rb");
    if (file && stat(idx_path, &st) == 0 &&
        st.st_size >= (off_t)(sizeof(expected) + sizeof(uint32_t)) &&
        (st.st_size - (off_t)sizeof(expected)) % sizeof(uint32_t) == 0) {
        page_index_header_t header;
        uint32_t first = 1;
        valid = fread(&header, sizeof(header), 1, file) == 1 &&
                fread(&first, sizeof(first), 1, file) == 1 && first == 0 &&
                memcmp(&header, &expected, sizeof(header)) == 0;
        if (valid) {
            count = (uint32_t)((st.st_size - sizeof(header)) /
                               sizeof(uint32_t));
        }
    }
    if (file) fclose(file);
    if (!valid) {
        file = fopen(idx_path, "wb");
        uint32_t first = 0;
        valid = file && fwrite(&expected, sizeof(expected), 1, file) == 1 &&
                fwrite(&first, sizeof(first), 1, file) == 1 &&
                fflush(file) == 0 && fsync(fileno(file)) == 0;
        if (file && fclose(file) != 0) valid = false;
        count = valid ? 1 : 0;
    }
    storage_unlock();
    if (!valid) {
        return ESP_FAIL;
    }
    *out_key = key;
    *out_page_count = count;
    return ESP_OK;
}

esp_err_t sd_storage_page_offset(uint32_t key, uint32_t page,
                                 uint32_t *out_offset)
{
    if (!out_offset) return ESP_ERR_INVALID_ARG;
    char path[64];
    index_path(key, path);
    storage_lock();
    FILE *file = fopen(path, "rb");
    long position = (long)sizeof(page_index_header_t) +
                    (long)page * (long)sizeof(uint32_t);
    bool ok = file && fseek(file, position, SEEK_SET) == 0 &&
              fread(out_offset, sizeof(*out_offset), 1, file) == 1;
    if (file) fclose(file);
    storage_unlock();
    return ok ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t sd_storage_append_page_offset(uint32_t key, uint32_t page,
                                        uint32_t offset)
{
    char path[64];
    index_path(key, path);
    storage_lock();
    struct stat st;
    FILE *file = stat(path, &st) == 0 ? fopen(path, "r+b") : NULL;
    uint32_t count = file && st.st_size >= (off_t)sizeof(page_index_header_t) ?
        (uint32_t)((st.st_size - sizeof(page_index_header_t)) /
                   sizeof(uint32_t)) : 0;
    bool ok = false;
    if (file && page < count) {
        uint32_t saved = 0;
        long position = (long)sizeof(page_index_header_t) +
                        (long)page * (long)sizeof(uint32_t);
        ok = fseek(file, position, SEEK_SET) == 0 &&
             fread(&saved, sizeof(saved), 1, file) == 1 && saved == offset;
    } else if (file && page == count) {
        ok = fseek(file, 0, SEEK_END) == 0 &&
             fwrite(&offset, sizeof(offset), 1, file) == 1 &&
             fflush(file) == 0 && fsync(fileno(file)) == 0;
    }
    if (file) fclose(file);
    storage_unlock();
    return ok ? ESP_OK : ESP_FAIL;
}

esp_err_t sd_storage_find_page(uint32_t key, uint32_t offset,
                               uint32_t *out_page,
                               uint32_t *out_offset)
{
    if (!out_page || !out_offset) return ESP_ERR_INVALID_ARG;
    char path[64];
    index_path(key, path);
    storage_lock();
    FILE *file = fopen(path, "rb");
    if (!file || fseek(file, sizeof(page_index_header_t), SEEK_SET) != 0) {
        if (file) fclose(file);
        storage_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    uint32_t value = 0;
    uint32_t page = 0;
    uint32_t best_page = 0;
    uint32_t best_offset = 0;
    while (fread(&value, sizeof(value), 1, file) == 1) {
        if (value > offset) break;
        best_page = page;
        best_offset = value;
        if (value == offset) break;
        page++;
    }
    fclose(file);
    storage_unlock();
    *out_page = best_page;
    *out_offset = best_offset;
    return ESP_OK;
}
