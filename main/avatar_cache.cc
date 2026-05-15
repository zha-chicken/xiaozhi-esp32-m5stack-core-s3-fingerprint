#include "avatar_cache.h"

#include <cstring>

#include <esp_log.h>
#include <esp_partition.h>
#include <esp_heap_caps.h>

#define TAG "AvatarCache"

namespace AvatarCache {

namespace {

const esp_partition_t* GetPartition() {
    return esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, kPartitionLabel);
}

// Read + validate the header. Force-null-terminates the URL so subsequent
// std::string construction is safe even if the on-flash bytes are corrupt.
bool ReadHeader(const esp_partition_t* part, Header& hdr) {
    if (esp_partition_read(part, 0, &hdr, sizeof(hdr)) != ESP_OK) return false;
    if (hdr.magic != kMagic) return false;
    if (hdr.size == 0 || hdr.size > part->size - sizeof(Header)) return false;
    hdr.url[kMaxUrlLen - 1] = '\0';
    return true;
}

} // namespace

bool MatchesCachedUrl(const std::string& url) {
    auto* part = GetPartition();
    if (!part) return false;
    Header hdr;
    if (!ReadHeader(part, hdr)) return false;
    return url == hdr.url;
}

bool Load(Loaded& out) {
    auto* part = GetPartition();
    if (!part) {
        ESP_LOGW(TAG, "avatar partition not found");
        return false;
    }
    Header hdr;
    if (!ReadHeader(part, hdr)) return false;
    char* buf = static_cast<char*>(heap_caps_malloc(hdr.size, MALLOC_CAP_8BIT));
    if (!buf) {
        ESP_LOGE(TAG, "OOM loading %u bytes from cache", (unsigned)hdr.size);
        return false;
    }
    if (esp_partition_read(part, sizeof(Header), buf, hdr.size) != ESP_OK) {
        ESP_LOGE(TAG, "read body failed");
        heap_caps_free(buf);
        return false;
    }
    out.data = buf;
    out.size = hdr.size;
    out.url.assign(hdr.url);
    ESP_LOGI(TAG, "Loaded %u bytes for %s", (unsigned)hdr.size, hdr.url);
    return true;
}

bool LoadInto(char* data, size_t capacity, Loaded& out) {
    auto* part = GetPartition();
    if (!part) {
        ESP_LOGW(TAG, "avatar partition not found");
        return false;
    }
    Header hdr;
    if (!ReadHeader(part, hdr)) return false;
    if (hdr.size > capacity) {
        ESP_LOGE(TAG, "cached avatar too large (%u > %u)",
                 (unsigned)hdr.size, (unsigned)capacity);
        return false;
    }
    if (esp_partition_read(part, sizeof(Header), data, hdr.size) != ESP_OK) {
        ESP_LOGE(TAG, "read body failed");
        return false;
    }
    out.data = data;
    out.size = hdr.size;
    out.url.assign(hdr.url);
    ESP_LOGI(TAG, "Loaded %u bytes into static buffer for %s", (unsigned)hdr.size, hdr.url);
    return true;
}

bool Save(const std::string& url, const char* data, size_t size) {
    auto* part = GetPartition();
    if (!part) {
        ESP_LOGW(TAG, "avatar partition not found");
        return false;
    }
    if (url.size() >= kMaxUrlLen) {
        ESP_LOGE(TAG, "URL too long (%u >= %u)",
                 (unsigned)url.size(), (unsigned)kMaxUrlLen);
        return false;
    }
    if (size > part->size - sizeof(Header)) {
        ESP_LOGE(TAG, "Payload too big (%u > %u)",
                 (unsigned)size, (unsigned)(part->size - sizeof(Header)));
        return false;
    }
    if (esp_partition_erase_range(part, 0, part->size) != ESP_OK) {
        ESP_LOGE(TAG, "erase failed");
        return false;
    }
    Header hdr = {};
    hdr.magic = kMagic;
    hdr.size = static_cast<uint32_t>(size);
    std::strncpy(hdr.url, url.c_str(), kMaxUrlLen - 1);
    if (esp_partition_write(part, 0, &hdr, sizeof(hdr)) != ESP_OK) {
        ESP_LOGE(TAG, "write header failed");
        return false;
    }
    if (esp_partition_write(part, sizeof(Header), data, size) != ESP_OK) {
        ESP_LOGE(TAG, "write body failed");
        return false;
    }
    ESP_LOGI(TAG, "Saved %u bytes for %s", (unsigned)size, url.c_str());
    return true;
}

} // namespace AvatarCache
