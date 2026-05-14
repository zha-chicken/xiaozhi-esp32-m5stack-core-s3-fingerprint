#include "user_avatar_sync.h"

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <memory>
#include <lvgl.h>

#include "avatar_cache.h"
#include "board.h"
#include "display/lvgl_display/lvgl_display.h"
#include "display/lvgl_display/lvgl_image.h"

#define TAG "UserAvatarSync"

namespace {

// Wait after OTA returns avatar_url before opening a second HTTPS fetch.
// Lets TLS stacks / WebSocket / audio settle — avoids contention with CheckNewVersion().
constexpr uint32_t kAvatarSyncDeferMs = 4000;

// BadgeWatch avatar slot fixed dimensions. The server (OTA route) picks the
// pixel layout per board — for waveshare-esp32-c6-lcd-1.69 it returns a raw
// little-endian RGB565 byte stream sized for these dimensions, sidestepping
// LVGL lodepng (which always decodes to ARGB8888 = 172×172×4 ≈ 118 KB and
// would OOM on the C6's ~112 KB free SRAM). See
// docs/research/firmware-mods/avatar-rgb565-pipeline.md.
constexpr int kAvatarWidth = 172;
constexpr int kAvatarHeight = 172;
constexpr int kAvatarStride = kAvatarWidth * 2; // RGB565: 2 bytes/pixel
constexpr size_t kExpectedRgb565Size = kAvatarWidth * kAvatarHeight * 2;

// After WiFi/TLS the heap often has enough total free SRAM but largest
// contiguous block still below 59168 (fragmentation). OTA-downloaded RGB565 is
// staged in BSS; LvglAllocatedImage(..., take_ownership=false) avoids heap_caps_free.
// Single buffer: only one avatar_sync task at a time.
alignas(4) static uint8_t s_avatar_rgb565[kExpectedRgb565Size];

struct SyncCtx {
    std::string url;
};

std::unique_ptr<LvglAllocatedImage> WrapAsAllocatedImage(char* data, size_t size,
                                                         bool take_ownership = true) {
    return std::make_unique<LvglAllocatedImage>(
        data, size, kAvatarWidth, kAvatarHeight, kAvatarStride,
        LV_COLOR_FORMAT_RGB565, take_ownership);
}

// HTTP GET → validate size → LvglAllocatedImage(data, size, w, h, stride,
// RGB565) → AvatarCache::Save → display->SetUserAvatar. Runs on its own
// FreeRTOS task so the OTA / boot path is not blocked, and so a slow link
// does not stall LVGL.
void DownloadTaskBody(std::unique_ptr<SyncCtx> ctx) {
    const std::string& url = ctx->url;

    auto* display = dynamic_cast<LvglDisplay*>(Board::GetInstance().GetDisplay());
    if (!display) {
        ESP_LOGW(TAG, "No LvglDisplay available, skipping avatar sync");
        vTaskDelete(nullptr);
        return;
    }

    char* data = reinterpret_cast<char*>(s_avatar_rgb565);

    auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
    if (!http->Open("GET", url)) {
        ESP_LOGE(TAG, "Failed to open URL: %s", url.c_str());
        vTaskDelete(nullptr);
        return;
    }
    int status_code = http->GetStatusCode();
    if (status_code != 200) {
        ESP_LOGE(TAG, "Avatar fetch HTTP %d for %s", status_code, url.c_str());
        http->Close();
        vTaskDelete(nullptr);
        return;
    }

    size_t content_length = http->GetBodyLength();
    if (content_length != kExpectedRgb565Size) {
        ESP_LOGE(TAG, "Unexpected avatar body size %u (want %u bytes RGB565)",
                 (unsigned)content_length, (unsigned)kExpectedRgb565Size);
        http->Close();
        vTaskDelete(nullptr);
        return;
    }

    size_t total_read = 0;
    while (total_read < content_length) {
        int ret = http->Read(data + total_read, content_length - total_read);
        if (ret < 0) {
            ESP_LOGE(TAG, "Avatar download read error after %u bytes", (unsigned)total_read);
            http->Close();
            vTaskDelete(nullptr);
            return;
        }
        if (ret == 0) break;
        total_read += ret;
    }
    http->Close();
    if (total_read != content_length) {
        ESP_LOGE(TAG, "Short avatar download: got %u of %u bytes",
                 (unsigned)total_read, (unsigned)content_length);
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI(TAG, "Downloaded %u avatar bytes (RGB565) from %s",
             (unsigned)total_read, url.c_str());

    // Persist before swapping. A failed Save() is logged but non-fatal —
    // we'd rather show the new avatar now and re-download next boot than
    // skip the slot update because flash misbehaved.
    AvatarCache::Save(url, data, total_read);

    auto image = WrapAsAllocatedImage(data, total_read, false);
    display->SetUserAvatar(std::move(image));
    ESP_LOGI(TAG, "Avatar slot updated");

    vTaskDelete(nullptr);
}

void DelayedAvatarSyncTask(void* arg) {
    std::unique_ptr<SyncCtx> ctx(static_cast<SyncCtx*>(arg));
    vTaskDelay(pdMS_TO_TICKS(kAvatarSyncDeferMs));
    DownloadTaskBody(std::move(ctx));
}

} // namespace

namespace UserAvatarSync {

void SyncFromUrl(const std::string& url) {
    if (url.empty()) return;

    // Boot-time LoadCachedImage() already populated the slot with the
    // matching cached bytes, so re-downloading would be wasted RAM, flash
    // wear, and bandwidth. Compare and bail.
    if (AvatarCache::MatchesCachedUrl(url)) {
        ESP_LOGI(TAG, "Avatar cache already matches OTA URL, skipping download");
        return;
    }

    auto* ctx = new SyncCtx{ url };
    BaseType_t ok =
        xTaskCreate(&DelayedAvatarSyncTask, "avatar_sync", 6 * 1024, ctx,
                    tskIDLE_PRIORITY + 1, nullptr);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to spawn avatar_sync task");
        delete ctx;
    }
}

std::unique_ptr<LvglImage> LoadCachedImage() {
    AvatarCache::Loaded cached;
    if (!AvatarCache::Load(cached)) return nullptr;
    if (cached.size != kExpectedRgb565Size) {
        ESP_LOGW(TAG, "Cached avatar wrong size %u, ignoring", (unsigned)cached.size);
        heap_caps_free(cached.data);
        return nullptr;
    }
    return WrapAsAllocatedImage(cached.data, cached.size, true);
}

} // namespace UserAvatarSync
