#include "user_avatar_sync.h"

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "board.h"
#include "display/lvgl_display/lvgl_display.h"
#include "display/lvgl_display/lvgl_image.h"

#define TAG "UserAvatarSync"

namespace {

struct SyncCtx {
    std::string url;
};

// Mirrors the upstream `self.screen.preview_image` MCP tool pipeline
// (mcp_server.cc:246-284): HTTP GET → LvglAllocatedImage → display setter.
// Runs on its own FreeRTOS task so the OTA / boot path is not blocked, and
// so a slow link does not stall LVGL.
void DownloadTask(void* arg) {
    std::unique_ptr<SyncCtx> ctx(static_cast<SyncCtx*>(arg));
    const std::string& url = ctx->url;

    auto* display = dynamic_cast<LvglDisplay*>(Board::GetInstance().GetDisplay());
    if (!display) {
        ESP_LOGW(TAG, "No LvglDisplay available, skipping avatar sync");
        vTaskDelete(nullptr);
        return;
    }

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
    if (content_length == 0) {
        ESP_LOGE(TAG, "Empty avatar body for %s", url.c_str());
        http->Close();
        vTaskDelete(nullptr);
        return;
    }
    char* data = static_cast<char*>(heap_caps_malloc(content_length, MALLOC_CAP_8BIT));
    if (!data) {
        ESP_LOGE(TAG, "OOM allocating %u bytes for avatar", (unsigned)content_length);
        http->Close();
        vTaskDelete(nullptr);
        return;
    }
    size_t total_read = 0;
    while (total_read < content_length) {
        int ret = http->Read(data + total_read, content_length - total_read);
        if (ret < 0) {
            ESP_LOGE(TAG, "Avatar download read error after %u bytes", (unsigned)total_read);
            heap_caps_free(data);
            http->Close();
            vTaskDelete(nullptr);
            return;
        }
        if (ret == 0) break;
        total_read += ret;
    }
    http->Close();
    ESP_LOGI(TAG, "Downloaded %u avatar bytes from %s", (unsigned)total_read, url.c_str());

    // LvglAllocatedImage takes ownership of `data` and calls heap_caps_free in
    // its destructor (lvgl_image.cc:59). On decode failure image_dsc() returns
    // a zeroed header so we reject before swapping.
    auto image = std::make_unique<LvglAllocatedImage>(data, total_read);
    if (!image->image_dsc() || image->image_dsc()->header.w == 0) {
        ESP_LOGE(TAG, "LVGL failed to decode avatar (check CONFIG_LV_USE_PNG)");
        vTaskDelete(nullptr);
        return;
    }
    display->SetUserAvatar(std::move(image));
    ESP_LOGI(TAG, "Avatar slot updated");

    vTaskDelete(nullptr);
}

} // namespace

namespace UserAvatarSync {

void SyncFromUrl(const std::string& url) {
    if (url.empty()) return;

    auto* ctx = new SyncCtx{ url };
    BaseType_t ok = xTaskCreate(&DownloadTask, "avatar_sync", 6 * 1024, ctx,
                                tskIDLE_PRIORITY + 1, nullptr);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to spawn avatar_sync task");
        delete ctx;
    }
}

} // namespace UserAvatarSync
