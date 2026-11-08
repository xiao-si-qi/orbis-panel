/**
 * @file bg_cache.c
 * @brief 背景图缓存实现：挂载 assets FAT 分区，按索引+URL 存取 JPEG。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "esp_partition.h"
#include "wear_levelling.h"

#include "bg_cache.h"

static const char *TAG = "bgcache";

#define CACHE_MOUNT   "/assets"
#define CACHE_MAX     (512 * 1024)
#define URL_MAX       256

static bool s_mounted = false;
static wl_handle_t s_wl = WL_INVALID_HANDLE;

static void cache_path(int index, const char *ext, char *out, size_t len)
{
    snprintf(out, len, "%s/bg_%d.%s", CACHE_MOUNT, index, ext);
}

bool bg_cache_ready(void)
{
    return s_mounted;
}

void bg_cache_init(void)
{
    if (s_mounted) {
        return;
    }
    const esp_vfs_fat_mount_config_t mount = {
        .format_if_mount_failed = true,
        .max_files = 4,
        .allocation_unit_size = 4096,
    };
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(CACHE_MOUNT, "assets", &mount, &s_wl);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "挂载 %s 失败: %s（背景图将每次都重新下载）", CACHE_MOUNT, esp_err_to_name(err));
        s_mounted = false;
        return;
    }
    s_mounted = true;
    ESP_LOGI(TAG, "缓存分区已挂载: %s", CACHE_MOUNT);
}

bool bg_cache_get(int index, const char *url, uint8_t **out, size_t *len)
{
    if (!s_mounted || url == NULL) {
        return false;
    }
    char path[64];
    cache_path(index, "url", path, sizeof(path));

    char cached_url[URL_MAX] = {0};
    FILE *fh = fopen(path, "rb");
    if (fh == NULL) {
        return false;
    }
    size_t n = fread(cached_url, 1, sizeof(cached_url) - 1, fh);
    fclose(fh);
    cached_url[n] = '\0';
    if (strcmp(cached_url, url) != 0) {
        return false;   /* URL 变了：缓存作废 */
    }

    cache_path(index, "jpg", path, sizeof(path));
    fh = fopen(path, "rb");
    if (fh == NULL) {
        return false;
    }
    fseek(fh, 0, SEEK_END);
    const long size = ftell(fh);
    fseek(fh, 0, SEEK_SET);
    if (size <= 0 || size > CACHE_MAX) {
        fclose(fh);
        return false;
    }

    uint8_t *buf = heap_caps_malloc((size_t)size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        fclose(fh);
        return false;
    }
    const size_t got = fread(buf, 1, (size_t)size, fh);
    fclose(fh);
    if (got != (size_t)size) {
        free(buf);
        return false;
    }

    *out = buf;
    *len = got;
    ESP_LOGI(TAG, "命中缓存 bg_%d.jpg (%d bytes)", index, (int)got);
    return true;
}

void bg_cache_put(int index, const char *url, const uint8_t *data, size_t len)
{
    if (!s_mounted || url == NULL || data == NULL || len == 0 || len > CACHE_MAX) {
        return;
    }
    char path[64];
    cache_path(index, "jpg", path, sizeof(path));
    FILE *fh = fopen(path, "wb");
    if (fh == NULL) {
        ESP_LOGW(TAG, "写缓存失败: %s", path);
        return;
    }
    const size_t written = fwrite(data, 1, len, fh);
    fclose(fh);
    if (written != len) {
        ESP_LOGW(TAG, "缓存写入不完整 (%d/%d)", (int)written, (int)len);
        return;
    }

    cache_path(index, "url", path, sizeof(path));
    fh = fopen(path, "wb");
    if (fh != NULL) {
        fwrite(url, 1, strlen(url), fh);
        fclose(fh);
    }
    ESP_LOGI(TAG, "已缓存 bg_%d.jpg (%d bytes)", index, (int)len);
}

void bg_cache_drop(int index)
{
    if (!s_mounted) {
        return;
    }
    char path[64];
    cache_path(index, "jpg", path, sizeof(path));
    remove(path);
    cache_path(index, "url", path, sizeof(path));
    remove(path);
}
