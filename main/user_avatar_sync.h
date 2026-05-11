#ifndef _USER_AVATAR_SYNC_H
#define _USER_AVATAR_SYNC_H

#include <string>

// Pulls the bound user's avatar PNG from `url`, decodes it via LVGL, and
// pushes it to the active LvglDisplay's avatar slot (overriding the
// compile-time-baked avatar_a).
//
// Caller invokes this on the main task; the actual HTTP + decode work runs
// in a detached FreeRTOS task so the OTA / boot path is not blocked.
//
// Mirrors the upstream `self.screen.preview_image` MCP tool pipeline
// (mcp_server.cc:246-284): HTTP GET → LvglAllocatedImage → SetUserAvatar.
namespace UserAvatarSync {
    void SyncFromUrl(const std::string& url);
}

#endif // _USER_AVATAR_SYNC_H
