#include "user_avatar_sync.h"

#include <sdkconfig.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <memory>
#include <lvgl.h>

#include "avatar_cache.h"
#include "application.h"
#include "board.h"
#include "display/lvgl_display/lvgl_display.h"
#include "display/lvgl_display/lvgl_image.h"

#define TAG "UserAvatarSync"

namespace {

// Wait after OTA returns avatar_url before opening a second HTTPS fetch.
// Lets TLS stacks / WebSocket / audio settle — avoids contention with CheckNewVersion().
#if CONFIG_IDF_TARGET_ESP32C6
constexpr uint32_t kAvatarSyncDeferMs = 30000;
constexpr uint32_t kMaintenanceRetryMs = 5000;
constexpr int kMaintenanceMaxAttempts = 24;
#else
constexpr uint32_t kAvatarSyncDeferMs = 4000;
#endif

// BadgeWatch avatar slot fixed dimensions. MUST equal the board's badge
// diameter (BadgeWatchConfig.badge_radius*2) AND the platform-side
// DEVICE_AVATAR_WIDTH / ALLOWED_SIZES for that board. The server (OTA route)
// returns a raw little-endian RGB565 byte stream sized for these dimensions,
// sidestepping LVGL lodepng (which always decodes to ARGB8888 and would OOM
// on SRAM-tight boards). See docs/research/firmware-mods/avatar-rgb565-pipeline.md.
//
// Per-target compile-time constant (NOT a runtime display query): the boot
// cache-load runs during display construction, before Board::GetDisplay() is
// set, so we cannot ask the display for its slot size.
#if CONFIG_IDF_TARGET_ESP32C6
constexpr int kAvatarWidth = 172;   // waveshare-c6-lcd-1.69 badge slot
constexpr int kAvatarHeight = 172;
#else
constexpr int kAvatarWidth = 220;   // m5stack-core-s3 badge slot; must match
constexpr int kAvatarHeight = 220;  // BadgeWatchConfig badge_radius*2 + platform DEVICE_AVATAR_WIDTH
#endif
constexpr int kAvatarStride = kAvatarWidth * 2; // RGB565: 2 bytes/pixel
constexpr size_t kExpectedRgb565Size = kAvatarWidth * kAvatarHeight * 2;

// ESP32-C6 has no PSRAM. It still supports personalized avatars, but only as a
// low-frequency maintenance task: load/display cached RGB565 from static RAM,
// then fetch a changed avatar after boot only while the app is idle.
#if CONFIG_IDF_TARGET_ESP32C6
constexpr bool kStaticAvatarBufferEnabled = true;
constexpr bool kAvatarMaintenanceWindowEnabled = true;
#else
constexpr bool kStaticAvatarBufferEnabled = false;
constexpr bool kAvatarMaintenanceWindowEnabled = false;
#endif

// After WiFi/TLS the heap often has enough total free SRAM but largest
// contiguous block still below the slot size (fragmentation). On the C6 (no
// PSRAM) the OTA-downloaded RGB565 is staged in BSS;
// LvglAllocatedImage(..., take_ownership=false) avoids heap_caps_free.
// Single buffer: only one avatar_sync task at a time.
//
// Non-C6 boards (e.g. CoreS3) have PSRAM and allocate the buffer from PSRAM in
// DownloadTaskBody instead, so the big 220×220 BSS buffer is NOT compiled in.
#if CONFIG_IDF_TARGET_ESP32C6
alignas(4) static uint8_t s_avatar_rgb565[kExpectedRgb565Size];
#endif

struct SyncCtx {
    std::string url;
};

std::unique_ptr<LvglAllocatedImage> WrapAsAllocatedImage(char* data, size_t size,
                                                         bool take_ownership = true) {
    return std::make_unique<LvglAllocatedImage>(
        data, size, kAvatarWidth, kAvatarHeight, kAvatarStride,
        LV_COLOR_FORMAT_RGB565, take_ownership);
}

void RestorePowerSaveIfIdle(Board& board) {
    if (Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
        board.SetPowerSaveMode(true);
    }
}

// HTTP GET → validate size → LvglAllocatedImage(data, size, w, h, stride,
// RGB565) → AvatarCache::Save → display->SetUserAvatar. Runs on its own
// FreeRTOS task so the OTA / boot path is not blocked, and so a slow link
// does not stall LVGL.
void DownloadTaskBody(std::unique_ptr<SyncCtx> ctx) {
    const std::string& url = ctx->url;

#if !CONFIG_IDF_TARGET_ESP32C6
    auto* display = dynamic_cast<LvglDisplay*>(Board::GetInstance().GetDisplay());
    if (!display) {
        ESP_LOGW(TAG, "No LvglDisplay available, skipping avatar sync");
        vTaskDelete(nullptr);
        return;
    }
#endif

    auto& board = Board::GetInstance();
    board.SetPowerSaveMode(false);

    auto http = board.GetNetwork()->CreateHttp(3);
    if (!http->Open("GET", url)) {
        ESP_LOGE(TAG, "Failed to open URL: %s", url.c_str());
        RestorePowerSaveIfIdle(board);
        vTaskDelete(nullptr);
        return;
    }
    int status_code = http->GetStatusCode();
    if (status_code != 200) {
        ESP_LOGE(TAG, "Avatar fetch HTTP %d for %s", status_code, url.c_str());
        http->Close();
        RestorePowerSaveIfIdle(board);
        vTaskDelete(nullptr);
        return;
    }

    size_t content_length = http->GetBodyLength();
    if (content_length != kExpectedRgb565Size) {
        ESP_LOGE(TAG, "Unexpected avatar body size %u (want %u bytes RGB565)",
                 (unsigned)content_length, (unsigned)kExpectedRgb565Size);
        http->Close();
        RestorePowerSaveIfIdle(board);
        vTaskDelete(nullptr);
        return;
    }

    // Acquire the RGB565 staging buffer now that the size is validated.
    //   C6   : fixed BSS buffer (no PSRAM); never freed (single-shot, then restart).
    //   else : PSRAM (CoreS3 etc.); ownership handed to LvglAllocatedImage so it
    //          is heap_caps_free'd when the avatar slot is later replaced.
#if CONFIG_IDF_TARGET_ESP32C6
    char* data = reinterpret_cast<char*>(s_avatar_rgb565);
#else
    char* data = reinterpret_cast<char*>(
        heap_caps_malloc(kExpectedRgb565Size, MALLOC_CAP_SPIRAM));
    if (!data) {
        ESP_LOGE(TAG, "OOM allocating %u bytes (PSRAM) for avatar",
                 (unsigned)kExpectedRgb565Size);
        http->Close();
        RestorePowerSaveIfIdle(board);
        vTaskDelete(nullptr);
        return;
    }
#endif
    // Free the staging buffer on any post-allocation error path. No-op on C6
    // (the BSS buffer is not heap-owned).
    auto release_buffer = [&]() {
#if !CONFIG_IDF_TARGET_ESP32C6
        heap_caps_free(data);
#endif
    };

    size_t total_read = 0;
    while (total_read < content_length) {
        int ret = http->Read(data + total_read, content_length - total_read);
        if (ret < 0) {
            ESP_LOGE(TAG, "Avatar download read error after %u bytes", (unsigned)total_read);
            http->Close();
            release_buffer();
            RestorePowerSaveIfIdle(board);
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
        release_buffer();
        RestorePowerSaveIfIdle(board);
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI(TAG, "Downloaded %u avatar bytes (RGB565) from %s",
             (unsigned)total_read, url.c_str());

    // Persist before swapping. A failed Save() is logged but non-fatal; the
    // user sees the fresh avatar now and the next OTA check can retry storage.
    AvatarCache::Save(url, data, total_read);

#if CONFIG_IDF_TARGET_ESP32C6
    ESP_LOGI(TAG, "Avatar cache updated; restarting to apply on ESP32-C6");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
#else
    // take_ownership=true: LvglAllocatedImage now owns the PSRAM buffer and
    // heap_caps_free's it when the slot is replaced (or the image is dropped).
    auto image = WrapAsAllocatedImage(data, total_read, true);
    display->SetUserAvatar(std::move(image));
    ESP_LOGI(TAG, "Avatar slot updated");

    RestorePowerSaveIfIdle(board);
    vTaskDelete(nullptr);
#endif
}

void DelayedAvatarSyncTask(void* arg) {
    std::unique_ptr<SyncCtx> ctx(static_cast<SyncCtx*>(arg));
    vTaskDelay(pdMS_TO_TICKS(kAvatarSyncDeferMs));

#if CONFIG_IDF_TARGET_ESP32C6
    if (kAvatarMaintenanceWindowEnabled) {
        for (int attempt = 0; attempt < kMaintenanceMaxAttempts; ++attempt) {
            if (Application::GetInstance().CanEnterSleepMode()) {
                break;
            }
            ESP_LOGI(TAG, "Avatar sync waiting for idle maintenance window");
            vTaskDelay(pdMS_TO_TICKS(kMaintenanceRetryMs));
        }
        if (!Application::GetInstance().CanEnterSleepMode()) {
            ESP_LOGW(TAG, "Avatar sync skipped; device stayed busy");
            vTaskDelete(nullptr);
            return;
        }
    }
#endif

    DownloadTaskBody(std::move(ctx));
}

} // namespace

namespace UserAvatarSync {

void SyncFromUrl(const std::string& url) {
    if (url.empty()) return;

    // Boot-time LoadCachedImage() already populated the slot with the matching
    // cached bytes, so re-downloading would be wasted RAM, flash wear, and
    // bandwidth. Compare and bail.
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
    // The s_avatar_rgb565 BSS buffer only exists on C6 (see its #if guard);
    // on PSRAM boards LoadInto is never used, so reference it only under the
    // same guard so the symbol is not required on non-C6 builds.
#if CONFIG_IDF_TARGET_ESP32C6
    if (kStaticAvatarBufferEnabled) {
        char* data = reinterpret_cast<char*>(s_avatar_rgb565);
        if (!AvatarCache::LoadInto(data, kExpectedRgb565Size, cached)) return nullptr;
    } else
#endif
    {
        if (!AvatarCache::Load(cached)) return nullptr;
    }
    if (cached.size != kExpectedRgb565Size) {
        ESP_LOGW(TAG, "Cached avatar wrong size %u, ignoring", (unsigned)cached.size);
        if (!kStaticAvatarBufferEnabled) {
            heap_caps_free(cached.data);
        }
        return nullptr;
    }
    return WrapAsAllocatedImage(cached.data, cached.size, !kStaticAvatarBufferEnabled);
}

} // namespace UserAvatarSync
