#ifndef _USER_AVATAR_SYNC_H
#define _USER_AVATAR_SYNC_H

#include <memory>
#include <string>

class LvglImage;

// Avatar sync pipeline: server returns a board-tailored RGB565 raw byte
// stream via OTA, firmware caches it to a dedicated flash partition keyed
// by URL, and rehydrates the cache on boot so the BadgeWatch avatar slot
// shows the user's image from the very first paint.
//
// Two entry points:
//
//   * `LoadCachedImage()` — synchronous, called by the BadgeWatch board
//     while it builds the avatar slot. Returns a LvglImage hydrated from
//     flash if a valid cache entry exists, else nullptr (caller falls
//     back to the baked `avatar_a`).
//
//   * `SyncFromUrl(url)` — called from the main task after each OTA poll.
//     No-ops if the cache already has this URL; otherwise spawns a
//     detached FreeRTOS task that downloads the bytes, updates the cache,
//     and swaps the slot via `display->SetUserAvatar`.
namespace UserAvatarSync {

void SyncFromUrl(const std::string& url);

// Returns nullptr if no valid cache entry exists.
std::unique_ptr<LvglImage> LoadCachedImage();

} // namespace UserAvatarSync

#endif // _USER_AVATAR_SYNC_H
