#ifndef _AVATAR_CACHE_H_
#define _AVATAR_CACHE_H_

#include <cstdint>
#include <cstddef>
#include <string>

// Persists the user's avatar (raw RGB565 bytes, ready to feed LVGL) into a
// dedicated 64 KB flash partition keyed by source URL. Survives reboots and
// is read on boot before the first paint so the BadgeWatch avatar slot can
// render the user's image immediately — no "show baked default, then swap
// to user image 4 seconds later" flicker.
//
// Storage layout (partition `avatar`):
//   offset 0..3   : magic 'AVTR'
//   offset 4..7   : payload size in bytes (little-endian uint32)
//   offset 8..263 : null-terminated URL (up to 255 chars + \0)
//   offset 264..  : `size` bytes of RGB565 little-endian pixel data
//
// Partition is erased + rewritten atomically each Save() call.
namespace AvatarCache {

constexpr uint32_t kMagic = 0x52545641;       // 'AVTR' little-endian
constexpr size_t   kMaxUrlLen = 256;          // includes null terminator
constexpr const char* kPartitionLabel = "avatar";

// Header stored at the start of the partition. RGB565 payload follows.
struct __attribute__((packed)) Header {
    uint32_t magic;
    uint32_t size;
    char url[kMaxUrlLen];
};

// Result of a successful Load. `data` is heap_caps_malloc'd and ownership
// transfers to the caller (use heap_caps_free, or wrap in LvglAllocatedImage
// which frees in its destructor).
struct Loaded {
    char* data;
    size_t size;
    std::string url;
};

// True iff the partition has a valid cache entry whose stored URL equals
// `url`. Cheap (reads only the header).
bool MatchesCachedUrl(const std::string& url);

// Pull the cached avatar into a fresh heap buffer. Returns true on hit
// (caller owns out.data), false on miss / corruption / OOM.
bool Load(Loaded& out);

// Pull the cached avatar into caller-owned storage. Use this on boards without
// PSRAM so the displayed avatar can live in a static buffer instead of heap.
bool LoadInto(char* data, size_t capacity, Loaded& out);

// Replace cache contents atomically. Returns false on flash error or
// oversized payload / URL.
bool Save(const std::string& url, const char* data, size_t size);

} // namespace AvatarCache

#endif // _AVATAR_CACHE_H_
