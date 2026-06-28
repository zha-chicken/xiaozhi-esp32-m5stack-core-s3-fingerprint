#include "display/badge_watch_display.h"

#include "wifi_station.h"
#include <font_awesome.h>
#include <esp_app_desc.h>
#include <sdkconfig.h>

#include <cmath>
#include <ctime>
#include <cstring>
#include <cstdio>
#include <cctype>

// Smaller CJK text font for badge layers — the board's global text font
// (puhui_basic_20_4 on both C6-1.69 and CoreS3) is too dense inside the
// badge. puhui_basic_16_4 / font_puhui_16_4 is shipped by
// managed_components/78__xiaozhi-fonts; we extern-pull the board-agnostic
// font_puhui_16_4 here so every BadgeWatch board uses it without each board
// re-declaring the symbol.
extern "C" const lv_font_t font_puhui_16_4;

#define TAG "BadgeWatchDisplay"

#if CONFIG_IDF_TARGET_ESP32C6
constexpr bool kLightweightBadgeUi = true;
#else
constexpr bool kLightweightBadgeUi = false;
#endif

namespace {
// ── Cream-cafe palette (matches tmp/badge-watch.html) ─────────────
constexpr uint32_t COLOR_CREAM       = 0xE8E1D3;
constexpr uint32_t COLOR_INK         = 0x1A1816;
constexpr uint32_t COLOR_CORAL       = 0xE8533A;
constexpr uint32_t COLOR_PEACH       = 0xF7CFB3;
constexpr uint32_t COLOR_SAGE        = 0x9BA27B;
constexpr uint32_t COLOR_RED         = 0xC8341A;
constexpr uint32_t COLOR_INK_FAINT   = 0xC9C2B5;  // ink @ ~22 % over cream
}  // namespace

BadgeWatchDisplay::BadgeWatchDisplay(esp_lcd_panel_io_handle_t io_handle,
                                     esp_lcd_panel_handle_t panel_handle,
                                     int width,
                                     int height,
                                     int offset_x,
                                     int offset_y,
                                     bool mirror_x,
                                     bool mirror_y,
                                     bool swap_xy,
                                     const BadgeWatchConfig& config)
    : SpiLcdDisplay(io_handle, panel_handle,
                    width, height, offset_x, offset_y, mirror_x, mirror_y, swap_xy),
      config_(config) {
    badge_radius_          = config_.badge_radius;
    ring_radius_           = config_.ring_radius;
    halo_radius_           = config_.halo_radius;
    tick_bg_len_cardinal_  = config_.tick_len_cardinal;
    tick_bg_len_major_     = config_.tick_len_major;
    tick_bg_len_minor_     = config_.tick_len_minor;
    hour_mark_w_           = config_.hour_mark_w;
    hour_mark_h_           = config_.hour_mark_h;

    DisplayLockGuard lock(this);
    BuildBadgeLayout();
}

// ── Public overrides ─────────────────────────────────────────────
// SetEmotion's job in the badge-watch UI is "pick a special-case badge
// layer". Application::Alert() uses one of: "link" (pairing code),
// "gear" (wifi config), "download" (OTA), "circle_xmark" (error). All
// other emotions ("neutral" / "happy" / etc.) fall through and the
// device-state callback decides the layer + halo.
void BadgeWatchDisplay::SetEmotion(const char* emotion) {
    DisplayLockGuard lock(this);
    current_emotion_ = emotion ? emotion : "";
    if (current_emotion_ == "link") {
        ShowBadgeLayer(kBfPairing);
        UpdateHalo(COLOR_CORAL, 128, 242, 1800);
        SetDialMode(kDialOff);
    } else if (current_emotion_ == "gear") {
        ShowBadgeLayer(kBfWifi);
        UpdateHalo(COLOR_PEACH, 115, 204, 3000);
        SetDialMode(kDialOff);
    } else if (current_emotion_ == "download") {
        ShowBadgeLayer(kBfUpgrading);
        UpdateHalo(COLOR_PEACH, 140, 242, 1400);
        SetDialMode(kDialProgress);
    } else if (current_emotion_ == "circle_xmark") {
        ShowBadgeLayer(kBfError);
        UpdateHalo(COLOR_RED, 153, 255, 700);
        SetDialMode(kDialOff);
    }
    // For all other emotions: don't touch the badge layer; device-state
    // dispatcher owns it. Don't call base — base loads emoji glyphs/GIFs
    // into emoji_label_, which is permanently hidden in this layout.
}

// Replace the baked-in default avatar on the badge avatar slot with a
// runtime image (typically the PNG/RGB565 fetched by UserAvatarSync after
// OTA). We retain ownership of the LvglImage so its underlying buffer
// outlives the lv_image_dsc_t pointed at by lv_image_set_src.
void BadgeWatchDisplay::SetUserAvatar(std::unique_ptr<LvglImage> image) {
    if (!image || !image->image_dsc()) return;
    DisplayLockGuard lock(this);
    if (!bf_layers_[kBfAvatar]) return;
    lv_image_set_src(bf_layers_[kBfAvatar], image->image_dsc());
    lv_obj_center(bf_layers_[kBfAvatar]);
    user_avatar_image_ = std::move(image);
    const bool was_ready = user_avatar_ready_;
    user_avatar_ready_ = true;
    // If we were parked on the boot layer waiting for the avatar to
    // arrive (cache miss path), flip to the avatar layer now that we
    // have something real to show. ApplyState handles routing on every
    // other state transition.
    if (!was_ready &&
        (current_state_ == kDeviceStateIdle ||
         current_state_ == kDeviceStateConnecting ||
         current_state_ == kDeviceStateUnknown)) {
        ShowBadgeLayer(kBfAvatar);
    }
}

// Step 7: status-bar overlay. Base updates the network/battery glyphs;
// we layer Cream-Cafe semantics on top:
//   • Wi-Fi opacity → "off" / "config" / "on" tiers per device state
//   • Battery low (≤QUARTER) tints the cell red + flips the bottom popup
//     from a centered toast into a thin ticker-warning, and we engage
//     the lowbat halo via ApplyState.
void BadgeWatchDisplay::UpdateStatusBar(bool update_all) {
    SpiLcdDisplay::UpdateStatusBar(update_all);
    DisplayLockGuard lock(this);

    if (network_label_) {
        lv_opa_t op = LV_OPA_30;
        const auto state = current_state_;
        if (state == kDeviceStateWifiConfiguring) {
            op = LV_OPA_40;
        } else if (WifiStation::GetInstance().IsConnected()) {
            op = LV_OPA_COVER;
        }
        lv_obj_set_style_text_opa(network_label_, op, 0);
    }

    // Detect low battery from the icon pointer base just set.
    const bool low =
        battery_icon_ &&
        (std::strcmp(battery_icon_, FONT_AWESOME_BATTERY_EMPTY) == 0 ||
         std::strcmp(battery_icon_, FONT_AWESOME_BATTERY_QUARTER) == 0);
    if (battery_label_) {
        lv_obj_set_style_text_color(battery_label_,
            lv_color_hex(low ? COLOR_RED : COLOR_INK), 0);
    }
    if (low_battery_popup_ && low_battery_label_) {
        // Override the popup's default red bg/white text with a Cream-
        // Cafe friendly thin warning bar.
        lv_obj_set_style_bg_color(low_battery_popup_, lv_color_hex(COLOR_CREAM), 0);
        lv_obj_set_style_bg_opa(low_battery_popup_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(low_battery_popup_, lv_color_hex(COLOR_RED), 0);
        lv_obj_set_style_border_width(low_battery_popup_, 1, 0);
        lv_obj_set_style_text_color(low_battery_label_, lv_color_hex(COLOR_RED), 0);
    }
    if (low_battery_active_ != low) {
        low_battery_active_ = low;
        // Re-dispatch the state so the halo flips to red (or restores).
        ApplyState(current_state_);
    }
}

// Step 8: power-save mode. Default base behavior is "show sleepy emoji",
// but emoji_label_ is permanently hidden in this layout. Instead we
// freeze the halo, kill the dial, and dim the ticker — gives a calm
// "device sleeping" feel without flicker.
void BadgeWatchDisplay::SetPowerSaveMode(bool on) {
    DisplayLockGuard lock(this);
    if (on) {
        FreezeHalo(LV_OPA_10);
        SetDialMode(kDialOff);
        if (chat_message_label_) {
            lv_obj_set_style_text_opa(chat_message_label_, LV_OPA_30, 0);
        }
    } else {
        if (chat_message_label_) {
            lv_obj_set_style_text_opa(chat_message_label_, LV_OPA_60, 0);
        }
        // Re-paint everything from current state — this restores the
        // halo animation, the dial mode, and any badge layer routing.
        ApplyState(current_state_);
    }
}

void BadgeWatchDisplay::SetChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    const std::string r = role ? role : "";
    const std::string c = content ? content : "";

    if (r == "system") {
        // Special-route by current emotion: parse the message into the
        // active bf-layer and DON'T update the ticker (ticker keeps
        // showing the version / state default chosen by ApplyState).
        if (current_emotion_ == "link") {
            std::string code = ExtractSixDigits(c);
            if (!code.empty() && bf_pairing_code_) {
                lv_label_set_text(bf_pairing_code_, code.c_str());
            }
            return;
        }
        if (current_emotion_ == "gear") {
            std::string ap = ExtractAfter(c, "热点 ");
            std::string ip = ExtractIp(c);
            if (!ap.empty() && bf_wifi_ap_) lv_label_set_text(bf_wifi_ap_, ap.c_str());
            if (!ip.empty() && bf_wifi_ip_) lv_label_set_text(bf_wifi_ip_, ip.c_str());
            return;
        }
        if (current_emotion_ == "download") {
            int pct = -1;
            if (std::sscanf(c.c_str(), "%d%%", &pct) >= 1 && pct >= 0) {
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%d%%", pct);
                if (bf_upgrading_pct_) lv_label_set_text(bf_upgrading_pct_, buf);
                UpdateProgress(pct);
            }
            if (bf_upgrading_size_) lv_label_set_text(bf_upgrading_size_, c.c_str());
            return;
        }
        if (current_emotion_ == "circle_xmark") {
            if (bf_error_text_ && !c.empty()) {
                lv_label_set_text(bf_error_text_, c.c_str());
            }
            return;
        }
        // Default system message → ticker. During boot/setup phases
        // Application::Start pushes the user agent string here
        // ("waveshare-c6-lcd-1.69/1.0.2"); ignore it and keep the
        // current tagline. Empty content → version (post-dismiss /
        // post-channel-close cleanup).
        const bool boot_phase =
            current_state_ == kDeviceStateStarting ||
            current_state_ == kDeviceStateActivating ||
            current_state_ == kDeviceStateWifiConfiguring;
        if (boot_phase) return;
        SetTickerText(c.empty() ? AppVersionString() : c.c_str());
        return;
    }

    // user / assistant: live STT/TTS transcript. SetTickerText auto-
    // selects centered (short) vs left+marquee (long). Empty content
    // is a no-op so transient post-turn clears don't blank the strip.
    if (c.empty()) return;
    SetTickerText(c.c_str());
}

void BadgeWatchDisplay::SetSecurityLock(bool locked) {
    DeviceState state = kDeviceStateUnknown;
    {
        DisplayLockGuard lock(this);
        if (security_locked_ == locked) {
            return;
        }
        security_locked_ = locked;
        state = current_state_;
        current_badge_layer_ = kBfCount;
    }
    ApplyState(state);
}

// ── Tiny extractors used by SetChatMessage routing ────────────────
std::string BadgeWatchDisplay::ExtractSixDigits(const std::string& s) {
    for (size_t i = 0; i + 6 <= s.size(); ++i) {
        bool all = true;
        for (int j = 0; j < 6; ++j) {
            if (!std::isdigit(static_cast<unsigned char>(s[i + j]))) {
                all = false;
                break;
            }
        }
        if (all) {
            // Make sure surrounding chars aren't digits (avoid grabbing
            // 6 of a longer run by accident).
            bool left_ok  = (i == 0) || !std::isdigit(static_cast<unsigned char>(s[i - 1]));
            bool right_ok = (i + 6 == s.size()) ||
                            !std::isdigit(static_cast<unsigned char>(s[i + 6]));
            if (left_ok && right_ok) return s.substr(i, 6);
        }
    }
    return "";
}

// Read the token starting after `marker`, terminated by Chinese comma,
// ASCII comma, space, or end of string.
std::string BadgeWatchDisplay::ExtractAfter(const std::string& s, const char* marker) {
    const auto p = s.find(marker);
    if (p == std::string::npos) return "";
    const auto start = p + std::strlen(marker);
    size_t end = start;
    while (end < s.size()) {
        const unsigned char ch = static_cast<unsigned char>(s[end]);
        // Chinese comma "，" is 3 bytes (0xEF 0xBC 0x8C); ascii ","; space; "\n".
        if (ch == ',' || ch == ' ' || ch == '\n' || ch == '\r') break;
        if (end + 2 < s.size() &&
            ch == 0xEF &&
            static_cast<unsigned char>(s[end + 1]) == 0xBC &&
            static_cast<unsigned char>(s[end + 2]) == 0x8C) break;
        ++end;
    }
    return s.substr(start, end - start);
}

// Pull a dotted IPv4 quad out of the message (handles plain IP or URL).
std::string BadgeWatchDisplay::ExtractIp(const std::string& s) {
    for (size_t i = 0; i < s.size(); ++i) {
        int a, b, c, d, n;
        if (std::sscanf(s.c_str() + i, "%d.%d.%d.%d%n", &a, &b, &c, &d, &n) == 4 &&
            a >= 0 && a <= 255 && b >= 0 && b <= 255 &&
            c >= 0 && c <= 255 && d >= 0 && d <= 255) {
            return s.substr(i, n);
        }
    }
    return "";
}

// ── Build pipeline ────────────────────────────────────────────────
void BadgeWatchDisplay::BuildBadgeLayout() {
    center_x_ = LV_HOR_RES / 2;
    // Pull the badge composition up from geometric center (config-driven;
    // C6 uses -8) so the bottom-most cardinal tick clears the ticker line.
    center_y_ = LV_VER_RES / 2 + config_.center_y_offset;
    BuildBackground();
    BuildHalo();
    BuildDial();
    BuildBadgeLayers();
    BuildCorners();
    BuildTicker();
    StartClockTimer();

    // Initial paint. SetDeviceState(kDeviceStateStarting) fires from
    // Application::Start() shortly after we exit this constructor and
    // re-runs this dispatch via the event subscription below — but we
    // need a frame on screen during the gap.
    ApplyState(kDeviceStateStarting);

    DeviceStateEventManager::GetInstance().RegisterStateChangeCallback(
        [this](DeviceState /*prev*/, DeviceState curr) {
            struct AsyncCtx { BadgeWatchDisplay* self; DeviceState s; };
            auto* ctx = new AsyncCtx{this, curr};
            lv_async_call([](void* arg) {
                auto* c = static_cast<AsyncCtx*>(arg);
                c->self->ApplyState(c->s);
                delete c;
            }, ctx);
        });
}

// ── Layer 0: cream surface + optional grid texture ────────────────
void BadgeWatchDisplay::BuildBackground() {
    lv_obj_t* screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(COLOR_CREAM), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    // nullptr bg_tile => solid cream only (no grid texture).
    if (config_.bg_tile) {
        lv_obj_set_style_bg_image_src(screen, config_.bg_tile, 0);
        lv_obj_set_style_bg_image_tiled(screen, true, 0);
    }

    // Disable parent-built flex layouts so we can hand-place children.
    if (container_) {
        lv_obj_set_layout(container_, LV_LAYOUT_NONE);
        lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
        lv_obj_set_style_bg_opa(container_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_opa(container_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_all(container_, 0, 0);
    }
    if (status_bar_) {
        lv_obj_set_size(status_bar_, 0, 0);
        lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_opa(status_bar_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_all(status_bar_, 0, 0);
    }
    if (content_) {
        lv_obj_set_layout(content_, LV_LAYOUT_NONE);
        lv_obj_set_style_bg_opa(content_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_opa(content_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_all(content_, 0, 0);
        lv_obj_set_size(content_, LV_HOR_RES, LV_VER_RES);
        lv_obj_set_pos(content_, 0, 0);
    }
    if (status_label_)       lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
    if (notification_label_) lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
    // version_label_: both current BadgeWatch boards run with show_ticker=true
    // + show_version_label=false, so the ticker shows the version string and we
    // hide the inherited bottom label. (If a future board sets
    // show_version_label=true it must also lift version_label_ onto `screen` and
    // re-pin it, or it renders behind the screen-level badge composition — see
    // git history's LiftVersionLabel for that path.)
    if (version_label_ && !config_.show_version_label) {
        lv_obj_add_flag(version_label_, LV_OBJ_FLAG_HIDDEN);
    }
}

// ── Halo: solid color circle, behind ticks + avatar, breathing opacity.
void BadgeWatchDisplay::BuildHalo() {
    lv_obj_t* screen = lv_screen_active();
    halo_ = lv_obj_create(screen);
    lv_obj_remove_style_all(halo_);
    lv_obj_set_size(halo_, halo_radius_ * 2, halo_radius_ * 2);
    lv_obj_set_pos(halo_, center_x_ - halo_radius_, center_y_ - halo_radius_);
    lv_obj_set_style_radius(halo_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(halo_, lv_color_hex(COLOR_INK), 0);
    lv_obj_set_style_bg_opa(halo_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(halo_, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(halo_, LV_OBJ_FLAG_IGNORE_LAYOUT);
    // Push to bottom of screen's child list so dial ticks + avatar both
    // render above. lv_obj_move_background = "first child" = lowest z.
    lv_obj_move_background(halo_);
}

void BadgeWatchDisplay::HaloOpaCb(void* var, int32_t value) {
    lv_obj_set_style_bg_opa(static_cast<lv_obj_t*>(var),
                             static_cast<lv_opa_t>(value), 0);
}

// Re-parameterise the halo: color + breathing range + period.
// Period is the full min→max→min cycle in ms.
void BadgeWatchDisplay::UpdateHalo(uint32_t color, uint8_t opa_min, uint8_t opa_max,
                                   uint32_t period_ms) {
    if (!halo_) return;
    lv_obj_set_style_bg_color(halo_, lv_color_hex(color), 0);
    lv_anim_delete(halo_, HaloOpaCb);

    if (kLightweightBadgeUi) {
        lv_obj_set_style_bg_opa(halo_, static_cast<lv_opa_t>((opa_min + opa_max) / 2), 0);
        halo_anim_running_ = false;
        return;
    }

    lv_anim_init(&halo_anim_);
    lv_anim_set_var(&halo_anim_, halo_);
    lv_anim_set_exec_cb(&halo_anim_, HaloOpaCb);
    lv_anim_set_values(&halo_anim_, opa_min, opa_max);
    lv_anim_set_time(&halo_anim_, period_ms / 2);
    lv_anim_set_playback_time(&halo_anim_, period_ms / 2);
    lv_anim_set_repeat_count(&halo_anim_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&halo_anim_, lv_anim_path_ease_in_out);
    lv_anim_start(&halo_anim_);
    halo_anim_running_ = true;
}

// Freeze the halo at a fixed opacity (used for power-save mode).
void BadgeWatchDisplay::FreezeHalo(uint8_t opa) {
    if (!halo_) return;
    lv_anim_delete(halo_, HaloOpaCb);
    lv_obj_set_style_bg_opa(halo_, opa, 0);
    halo_anim_running_ = false;
}

// ── Dial: 60 background ticks (cardinal/major/minor) ──────────────
void BadgeWatchDisplay::BuildDial() {
    lv_obj_t* screen = lv_screen_active();
    for (int i = 0; i < 60; i++) {
        const bool is_cardinal = (i % 15 == 0);
        const bool is_major    = (i % 5 == 0);
        const int  len = is_cardinal ? tick_bg_len_cardinal_
                       : is_major    ? tick_bg_len_major_
                                     : tick_bg_len_minor_;
        const int  w   = is_cardinal ? 2 : 1;
        const lv_opa_t opa = is_cardinal ? LV_OPA_70
                          : is_major    ? LV_OPA_40
                                        : LV_OPA_30;

        const float ang = (i * 6.0f) * (3.14159265f / 180.0f);
        const float sx = sinf(ang), cx = cosf(ang);
        const int outer_x = center_x_ + (int)(sx * ring_radius_);
        const int outer_y = center_y_ - (int)(cx * ring_radius_);
        const int inner_x = center_x_ + (int)(sx * (ring_radius_ - len));
        const int inner_y = center_y_ - (int)(cx * (ring_radius_ - len));
        const int mx = (outer_x + inner_x) / 2;
        const int my = (outer_y + inner_y) / 2;

        lv_obj_t* t = lv_obj_create(screen);
        lv_obj_remove_style_all(t);
        lv_obj_set_size(t, w, len);
        lv_obj_set_style_bg_color(t, lv_color_hex(COLOR_INK), 0);
        lv_obj_set_style_bg_opa(t, opa, 0);
        lv_obj_set_style_radius(t, 0, 0);
        lv_obj_set_style_transform_angle(t, i * 60, 0);
        lv_obj_set_style_transform_pivot_x(t, w / 2, 0);
        lv_obj_set_style_transform_pivot_y(t, len / 2, 0);
        lv_obj_set_pos(t, mx - w / 2, my - len / 2);
        lv_obj_add_flag(t, LV_OBJ_FLAG_IGNORE_LAYOUT);
    }

    // ── minute_arc_: single peach arc, doubles for clock + progress ──
    // Sized to outer radius ring_radius_ so its stroke lands in the same
    // band as the tick ring. emoji_box_ (re-parented later in
    // BuildBadgeLayers) is appended after this widget, so it sits on top
    // and clips the inner half — only the stroke in the tick zone shows.
    minute_arc_ = lv_arc_create(screen);
    lv_obj_remove_style(minute_arc_, nullptr, LV_PART_KNOB);
    lv_obj_set_size(minute_arc_, ring_radius_ * 2, ring_radius_ * 2);
    lv_obj_set_pos(minute_arc_, center_x_ - ring_radius_, center_y_ - ring_radius_);
    lv_arc_set_rotation(minute_arc_, 270);   // 0° = 12 o'clock
    lv_arc_set_bg_angles(minute_arc_, 0, 360);
    lv_arc_set_angles(minute_arc_, 0, 0);
    lv_obj_set_style_arc_color(minute_arc_, lv_color_hex(COLOR_PEACH), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(minute_arc_, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(minute_arc_, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(minute_arc_, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(minute_arc_, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(minute_arc_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(minute_arc_, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(minute_arc_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(minute_arc_, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_add_flag(minute_arc_, LV_OBJ_FLAG_HIDDEN);

    // ── hour_mark_: coral bar at the dial radius, repositioned per minute ──
    hour_mark_ = lv_obj_create(screen);
    lv_obj_remove_style_all(hour_mark_);
    lv_obj_set_size(hour_mark_, hour_mark_w_, hour_mark_h_);
    lv_obj_set_style_radius(hour_mark_, 1, 0);
    lv_obj_set_style_bg_color(hour_mark_, lv_color_hex(COLOR_CORAL), 0);
    lv_obj_set_style_bg_opa(hour_mark_, LV_OPA_COVER, 0);
    lv_obj_add_flag(hour_mark_, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_add_flag(hour_mark_, LV_OBJ_FLAG_HIDDEN);
}

// Reposition the hour_mark_ rectangle so its center sits at the angle
// matching the current local time. Angle is measured clockwise from 12 o'clock.
void BadgeWatchDisplay::UpdateHourMark(int hour, int minute) {
    if (!hour_mark_) return;
    // Each hour spans 30° (12 hours = 360°). Plus minute interpolation.
    const float deg = ((hour % 12) + minute / 60.0f) * 30.0f;
    const float rad = deg * 3.14159265f / 180.0f;
    const float sx = sinf(rad), cx = cosf(rad);
    // Place center at radius (ring_radius_ - hour_mark_h_/2) so the outer
    // edge sits flush with the dial and the inner edge dives into the
    // avatar zone (where it gets clipped by emoji_box_ on top).
    const int r = ring_radius_ - hour_mark_h_ / 2;
    const int mx = center_x_ + (int)(sx * r);
    const int my = center_y_ - (int)(cx * r);
    lv_obj_set_pos(hour_mark_, mx - hour_mark_w_ / 2, my - hour_mark_h_ / 2);
    // 0.1° units; rotate around the rectangle's own center.
    lv_obj_set_style_transform_pivot_x(hour_mark_, hour_mark_w_ / 2, 0);
    lv_obj_set_style_transform_pivot_y(hour_mark_, hour_mark_h_ / 2, 0);
    lv_obj_set_style_transform_angle(hour_mark_, (int32_t)(deg * 10.0f), 0);
}

// Switch dial overlays per state.
void BadgeWatchDisplay::SetDialMode(DialMode mode) {
    current_dial_mode_ = mode;
    if (mode == kDialOff) {
        if (minute_arc_) lv_obj_add_flag(minute_arc_, LV_OBJ_FLAG_HIDDEN);
        if (hour_mark_)  lv_obj_add_flag(hour_mark_,  LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (mode == kDialClock) {
        // Peach minute fill + coral hour bar.
        if (minute_arc_) {
            lv_obj_set_style_arc_color(minute_arc_, lv_color_hex(COLOR_PEACH),
                                         LV_PART_INDICATOR);
            lv_obj_clear_flag(minute_arc_, LV_OBJ_FLAG_HIDDEN);
        }
        if (hour_mark_) lv_obj_clear_flag(hour_mark_, LV_OBJ_FLAG_HIDDEN);
        RefreshClock();
        return;
    }
    // kDialProgress
    if (minute_arc_) {
        lv_obj_set_style_arc_color(minute_arc_, lv_color_hex(COLOR_CORAL),
                                     LV_PART_INDICATOR);
        lv_obj_clear_flag(minute_arc_, LV_OBJ_FLAG_HIDDEN);
    }
    if (hour_mark_) lv_obj_add_flag(hour_mark_, LV_OBJ_FLAG_HIDDEN);
    UpdateProgress(progress_pct_);
}

// Read system clock and snap minute_arc_ + hour_mark_ to wall-time.
// No-op until SNTP has set system time (year >= 2025).
void BadgeWatchDisplay::RefreshClock() {
    if (current_dial_mode_ != kDialClock) return;
    time_t now = time(nullptr);
    struct tm tm_local;
    localtime_r(&now, &tm_local);
    if (tm_local.tm_year < 2025 - 1900) {
        // Time not yet set — show empty dial (no fill, hour mark at 12).
        if (minute_arc_) lv_arc_set_angles(minute_arc_, 0, 0);
        UpdateHourMark(0, 0);
        return;
    }
    if (minute_arc_) {
        lv_arc_set_angles(minute_arc_, 0, tm_local.tm_min * 6);
    }
    UpdateHourMark(tm_local.tm_hour, tm_local.tm_min);
}

// Drive the progress arc from upgrade-percent (0..100).
void BadgeWatchDisplay::UpdateProgress(int pct) {
    progress_pct_ = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
    if (current_dial_mode_ == kDialProgress && minute_arc_) {
        lv_arc_set_angles(minute_arc_, 0, (int32_t)(progress_pct_ * 3.6f));
    }
}

// ── Central badge: cream circle + 7 pre-built layers ──────────────
void BadgeWatchDisplay::BuildBadgeLayers() {
    lv_obj_t* screen = lv_screen_active();
    if (emoji_box_) {
        lv_obj_set_parent(emoji_box_, screen);
        lv_obj_set_size(emoji_box_, badge_radius_ * 2, badge_radius_ * 2);
        lv_obj_set_style_radius(emoji_box_, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(emoji_box_, lv_color_hex(COLOR_CREAM), 0);
        lv_obj_set_style_bg_opa(emoji_box_, LV_OPA_COVER, 0);
        // No hairline ring around the avatar — the breathing halo IS the
        // outer ring. Border kept transparent on every side.
        lv_obj_set_style_border_opa(emoji_box_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(emoji_box_, 0, 0);
        lv_obj_set_style_clip_corner(emoji_box_, true, 0);
        lv_obj_set_pos(emoji_box_, center_x_ - badge_radius_, center_y_ - badge_radius_);
        lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_IGNORE_LAYOUT);
        // Children are scroll-disabled — content fits, no scroll desired.
        lv_obj_clear_flag(emoji_box_, LV_OBJ_FLAG_SCROLLABLE);
    }
    if (emoji_label_) {
        lv_obj_set_style_text_color(emoji_label_, lv_color_hex(COLOR_INK), 0);
        lv_obj_center(emoji_label_);
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }
    if (!emoji_box_) return;

    // ── Layer 0: avatar (default visible) ─────────────────────────
    // Try the persisted user avatar first — if flash has it, we render
    // the user's image from the very first paint. Cache miss falls back
    // to the baked config_.default_avatar (if the board has one). When
    // default_avatar is nullptr (e.g. CoreS3 first pass) we leave the
    // image src empty; the boot/spinner layer holds via user_avatar_ready_
    // gating until SetUserAvatar fires. An empty lv_image (no src) is
    // harmless — it is hidden under the boot layer anyway.
    {
        lv_obj_t* avatar = lv_image_create(emoji_box_);
        auto cached = UserAvatarSync::LoadCachedImage();
        if (cached) {
            lv_image_set_src(avatar, cached->image_dsc());
            user_avatar_image_ = std::move(cached);
            user_avatar_ready_ = true;
        } else if (config_.default_avatar) {
            lv_image_set_src(avatar, config_.default_avatar);
        }
        lv_obj_center(avatar);
        bf_layers_[kBfAvatar] = avatar;
    }

    // Helper: make a flex-column layer that fills emoji_box_, hidden by default.
    auto make_layer = [&](BadgeLayer idx, int row_pad) -> lv_obj_t* {
        lv_obj_t* layer = lv_obj_create(emoji_box_);
        lv_obj_remove_style_all(layer);
        lv_obj_set_size(layer, badge_radius_ * 2, badge_radius_ * 2);
        lv_obj_set_pos(layer, 0, 0);
        lv_obj_set_flex_flow(layer, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(layer, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                               LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(layer, 8, 0);
        lv_obj_set_style_pad_row(layer, row_pad, 0);
        lv_obj_clear_flag(layer, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(layer, LV_OBJ_FLAG_HIDDEN);
        bf_layers_[idx] = layer;
        return layer;
    };

    // Helper: text label child with font + color + opacity.
    // When `font == nullptr` we fall back to the smaller 16 pt CJK
    // font (instead of inheriting the screen's 20 pt global) so badge
    // text fits comfortably inside the badge circle.
    auto add_label = [](lv_obj_t* parent, const char* text,
                         const lv_font_t* font, uint32_t color, lv_opa_t opa) -> lv_obj_t* {
        lv_obj_t* lbl = lv_label_create(parent);
        lv_label_set_text(lbl, text);
        lv_obj_set_style_text_font(lbl, font ? font : &font_puhui_16_4, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);
        lv_obj_set_style_text_opa(lbl, opa, 0);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        return lbl;
    };

    // Helper: spinner (1.2 s rotation, coral arc) sized to 26 px.
    auto add_spinner = [](lv_obj_t* parent) -> lv_obj_t* {
        if (kLightweightBadgeUi) {
            lv_obj_t* s = lv_label_create(parent);
            lv_label_set_text(s, "...");
            lv_obj_set_style_text_font(s, &lv_font_montserrat_28, 0);
            lv_obj_set_style_text_color(s, lv_color_hex(COLOR_CORAL), 0);
            return s;
        }

        lv_obj_t* s = lv_spinner_create(parent);
        lv_obj_set_size(s, 26, 26);
        lv_spinner_set_anim_params(s, 1200, 60);
        lv_obj_set_style_arc_color(s, lv_color_hex(COLOR_INK_FAINT),
                                    LV_PART_MAIN);
        lv_obj_set_style_arc_color(s, lv_color_hex(COLOR_CORAL),
                                    LV_PART_INDICATOR);
        lv_obj_set_style_arc_width(s, 3, LV_PART_MAIN);
        lv_obj_set_style_arc_width(s, 3, LV_PART_INDICATOR);
        return s;
    };

    // ── Layer 1: boot ─────────────────────────────────────────────
    // Spinner above + "启动中" below. Acts as both the startup screen
    // and the avatar-not-yet-cached holding screen (UserAvatarSync
    // parks here until SetUserAvatar fires for cache-miss boots).
    {
        lv_obj_t* l = make_layer(kBfBoot, 8);
        add_spinner(l);
        add_label(l, "启动中", nullptr, COLOR_INK, LV_OPA_COVER);
    }

    // ── Layer 2: wifi-config (2 lines) ────────────────────────────
    // Phone OSes auto-pop the captive-portal page after joining the AP,
    // so we don't need to instruct the user to open 192.168.4.1 manually.
    {
        lv_obj_t* l = make_layer(kBfWifi, 6);
        add_label(l, "手机 Wi-Fi 搜索连接", nullptr, COLOR_INK, LV_OPA_70);
        // AP name placeholder — populated at runtime by SetChatMessage.
        bf_wifi_ap_ = add_label(l, "HaoLab-XXXX", nullptr, COLOR_CORAL, LV_OPA_COVER);
        // bf_wifi_ip_ kept as nullptr; SetChatMessage's IP write is a no-op.
    }

    // ── Layer 3: activating ───────────────────────────────────────
    {
        lv_obj_t* l = make_layer(kBfActivating, 6);
        add_spinner(l);
        add_label(l, "激活中", nullptr, COLOR_INK, LV_OPA_COVER);
    }

    // ── Layer 4: pairing code ─────────────────────────────────────
    {
        lv_obj_t* l = make_layer(kBfPairing, 4);
        add_label(l, "配对码", nullptr, COLOR_INK, LV_OPA_60);
        bf_pairing_code_ = add_label(l, "------", &lv_font_montserrat_28,
                                      COLOR_CORAL, LV_OPA_COVER);
        add_label(l, "在平台输入", nullptr, COLOR_INK, LV_OPA_60);
    }

    // ── Layer 5: upgrading ────────────────────────────────────────
    {
        lv_obj_t* l = make_layer(kBfUpgrading, 4);
        bf_upgrading_pct_ = add_label(l, "0%", &lv_font_montserrat_36,
                                        COLOR_CORAL, LV_OPA_COVER);
        add_label(l, "固件升级中", nullptr, COLOR_INK, LV_OPA_COVER);
        bf_upgrading_size_ = add_label(l, "", nullptr, COLOR_INK, LV_OPA_60);
    }

    // ── Layer 6: error ────────────────────────────────────────────
    {
        lv_obj_t* l = make_layer(kBfError, 4);
        add_label(l, "!", &lv_font_montserrat_44, COLOR_CORAL, LV_OPA_COVER);
        bf_error_text_ = add_label(l, "出错了", nullptr, COLOR_INK, LV_OPA_COVER);
        add_label(l, "稍后重试", nullptr, COLOR_INK, LV_OPA_60);
    }

    // ── Layer 7: fingerprint lock ─────────────────────────────────
    {
        lv_obj_t* l = make_layer(kBfLocked, 4);
        add_label(l, "LOCK", &lv_font_montserrat_28, COLOR_INK, LV_OPA_COVER);
        add_label(l, "指纹解锁", nullptr, COLOR_CORAL, LV_OPA_COVER);
        add_label(l, "通过后可对话", nullptr, COLOR_INK, LV_OPA_60);
    }
}

// ── Toggle the visible layer; cheap because all layers are pre-built.
void BadgeWatchDisplay::ShowBadgeLayer(BadgeLayer idx) {
    if (idx == current_badge_layer_) return;
    for (int i = 0; i < kBfCount; ++i) {
        if (!bf_layers_[i]) continue;
        if (i == static_cast<int>(idx)) {
            lv_obj_clear_flag(bf_layers_[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(bf_layers_[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    current_badge_layer_ = idx;
}

// ── 4-corner status icons ─────────────────────────────────────────
// Margins: 16 px from edges. The Waveshare module case covers ~6–8 px
// of the LCD perimeter, so anything tighter than 14 sits visibly under
// the bezel.
void BadgeWatchDisplay::BuildCorners() {
    lv_obj_t* screen = lv_screen_active();
    if (network_label_) {
        lv_obj_set_parent(network_label_, screen);
        lv_obj_set_style_text_color(network_label_, lv_color_hex(COLOR_INK), 0);
        lv_obj_align(network_label_, LV_ALIGN_TOP_LEFT, 16, 14);
    }
    if (battery_label_) {
        lv_obj_set_parent(battery_label_, screen);
        lv_obj_set_style_text_color(battery_label_, lv_color_hex(COLOR_INK), 0);
        lv_obj_align(battery_label_, LV_ALIGN_TOP_RIGHT, -16, 14);
    }
    if (mute_label_) {
        lv_obj_set_parent(mute_label_, screen);
        lv_obj_set_style_text_color(mute_label_, lv_color_hex(COLOR_INK), 0);
        lv_obj_align(mute_label_, LV_ALIGN_TOP_LEFT, 38, 14);
    }
    if (low_battery_popup_) {
        lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, -22);
    }
}

// ── Bottom ticker (chat_message_label_ in marquee mode) ───────────
// When the board has no ticker (config_.show_ticker == false) we leave
// chat_message_label_ where the base SetupUI put it but hide it, and
// SetTickerText degrades to a no-op (see below). The fill-height badge on
// such boards leaves no room for a bottom strip.
void BadgeWatchDisplay::BuildTicker() {
    if (!chat_message_label_) return;
    if (!config_.show_ticker) {
        lv_obj_add_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_set_parent(chat_message_label_, lv_screen_active());
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(chat_message_label_, LV_HOR_RES - 32);
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(chat_message_label_, lv_color_hex(COLOR_INK), 0);
    lv_obj_set_style_text_opa(chat_message_label_, LV_OPA_60, 0);
    lv_obj_set_style_text_font(chat_message_label_, &font_puhui_16_4, 0);
    // 8 px from the bottom edge. Combined with the upward shift of the
    // dial above, this gives the ticker label clearance from the
    // bottommost cardinal tick.
    lv_obj_align(chat_message_label_, LV_ALIGN_BOTTOM_MID, 0, -8);
}

// ── Periodic refresh for clock-mode dial (1-minute resolution) ────
void BadgeWatchDisplay::StartClockTimer() {
    const esp_timer_create_args_t args = {
        .callback = [](void* arg) {
            lv_async_call([](void* a) {
                static_cast<BadgeWatchDisplay*>(a)->RefreshClock();
            }, arg);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "haolab_clock",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &clock_timer_));
    ESP_ERROR_CHECK(esp_timer_start_periodic(clock_timer_, 60ULL * 1000 * 1000));
}

// ── Ticker convenience (auto-fit: center if it fits, scroll if not) ─
// Measures the rendered width of `text` against the ticker's content
// width (LV_HOR_RES - 32). Fits → DOT mode + CENTER align (static,
// visually balanced). Doesn't fit → SCROLL_CIRCULAR + LEFT align
// (LVGL only scrolls non-CENTER text). Width is re-asserted every
// call in case some upstream layout pass clobbered it.
//
// On ticker-less boards (config_.show_ticker == false) this degrades to a
// no-op so transcript / state routing in ApplyState / SetChatMessage stays
// safe without touching the hidden label.
void BadgeWatchDisplay::SetTickerText(const char* text) {
    if (!config_.show_ticker) return;
    if (!chat_message_label_) return;
    const char* s = text ? text : "";
    const int32_t max_w = LV_HOR_RES - 32;

    lv_point_t size = {0, 0};
    if (*s) {
        lv_text_get_size(&size, s, &font_puhui_16_4, 0, 0,
                          LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    }
    const bool fits = size.x <= max_w;
    lv_obj_set_width(chat_message_label_, max_w);
    lv_label_set_long_mode(chat_message_label_,
        fits ? LV_LABEL_LONG_DOT : LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(chat_message_label_,
        fits ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_text(chat_message_label_, s);
}

// App version string ("v1.0.2 by Hao Lab") cached after first call.
// Always-on bottom-line tagline when the ticker has nothing else to say
// (idle, listening before STT, speaking before TTS).
const char* BadgeWatchDisplay::AppVersionString() {
    // "v" + version[32] + " by Hao Lab" + null.
    static char ver_str[56] = {0};
    if (ver_str[0] == 0) {
        const esp_app_desc_t* desc = esp_app_get_description();
        if (desc) snprintf(ver_str, sizeof(ver_str), "v%s by Hao Lab", desc->version);
        else      std::strcpy(ver_str, "v? by Hao Lab");
    }
    return ver_str;
}

// ── Central state → UI dispatcher ────────────────────────────────
// Called from device-state events (lv_async bounce, LVGL task) and
// from the constructor as an initial paint. Other overrides
// (SetEmotion / SetChatMessage) may have already routed the badge
// layer to a more specific view (pairing / upgrading / error /
// wifi-config); we respect that override unless the new state moves
// out of its parent state (e.g. Activating → Idle clears pairing).
void BadgeWatchDisplay::ApplyState(DeviceState state) {
    DisplayLockGuard lock(this);
    const DeviceState prev = current_state_;
    current_state_ = state;

    // When stepping out of an emotion-driven override layer (pairing /
    // upgrading / error), invalidate the cached current_badge_layer_ so
    // the ShowBadgeLayer() call later in this dispatch actually executes
    // its hide/show toggle.
    if (prev != state) {
        const bool leaving_activating = (prev == kDeviceStateActivating);
        const bool leaving_upgrading  = (prev == kDeviceStateUpgrading);
        if ((leaving_activating && current_badge_layer_ == kBfPairing) ||
            (leaving_upgrading  && current_badge_layer_ == kBfUpgrading) ||
            (current_badge_layer_ == kBfError && state != kDeviceStateUnknown)) {
            current_badge_layer_ = kBfCount;   // sentinel — forces re-toggle
        }
    }

    if (security_locked_ &&
        (state == kDeviceStateIdle || state == kDeviceStateUnknown)) {
        ShowBadgeLayer(kBfLocked);
        UpdateHalo(COLOR_INK, 38, 115, 3600);
        SetDialMode(kDialClock);
        SetTickerText("请按指纹解锁");
        if (low_battery_active_) {
            UpdateHalo(COLOR_RED, 102, 217, 2400);
            SetTickerText("电量不足，请尽快充电");
        }
        return;
    }

    switch (state) {
        case kDeviceStateStarting:
            ShowBadgeLayer(kBfBoot);
            UpdateHalo(COLOR_PEACH, 102, 191, 2600);   // 40 → 75 %
            SetDialMode(kDialOff);
            SetTickerText(AppVersionString());
            break;
        case kDeviceStateWifiConfiguring:
            if (current_badge_layer_ != kBfWifi) ShowBadgeLayer(kBfWifi);
            UpdateHalo(COLOR_PEACH, 115, 204, 3000);   // 45 → 80 %
            SetDialMode(kDialOff);
            SetTickerText(AppVersionString());
            break;
        case kDeviceStateActivating:
            // Visually a continuation of boot — upstream xiaozhi exposes
            // 激活中 only as a status-text change, never as a separate
            // screen. We keep the bf-boot layer up so the OTA round-trip
            // doesn't flash a new screen at the user. SetEmotion("link")
            // (fired from ShowActivationCode) is the only thing that
            // promotes us to the bf-pairing layer.
            if (current_badge_layer_ != kBfPairing) {
                ShowBadgeLayer(kBfBoot);
            }
            UpdateHalo(COLOR_PEACH, 115, 217, 2000);   // 45 → 85 %
            SetDialMode(kDialOff);
            SetTickerText(AppVersionString());
            break;
        case kDeviceStateConnecting:
            // Show the avatar layer if we have *something* to show: the user's
            // downloaded image, a baked default (config_.default_avatar), or
            // the lightweight target. Only boards with neither a cached user
            // avatar nor a baked default park on the boot/spinner layer until
            // SetUserAvatar fires.
            ShowBadgeLayer((user_avatar_ready_ || config_.default_avatar != nullptr || kLightweightBadgeUi) ? kBfAvatar : kBfBoot);
            UpdateHalo(COLOR_SAGE, 140, 255, 900);     // 55 → 100 %
            SetDialMode(kDialClock);
            SetTickerText("正在接通…");
            break;
        case kDeviceStateIdle:
        case kDeviceStateUnknown:
            ShowBadgeLayer((user_avatar_ready_ || config_.default_avatar != nullptr || kLightweightBadgeUi) ? kBfAvatar : kBfBoot);
            // Coral is our brand red, always-breathing — no gray ring.
            UpdateHalo(COLOR_CORAL, 51, 140, 5000);    // 20 → 55 %, slow & calm
            SetDialMode(kDialClock);
            SetTickerText(AppVersionString());
            break;
        case kDeviceStateListening:
            ShowBadgeLayer(kBfAvatar);
            UpdateHalo(COLOR_CORAL, 128, 230, 1600);   // 50 → 90 %, attentive
            SetDialMode(kDialClock);
            // When coming from Speaking (TTS just finished), let the
            // assistant's last sentence linger on the ticker so the user
            // has time to read it. SetChatMessage("user", text) on first
            // STT will replace. For other entry paths (idle/connecting/
            // unknown) show the "倾听中" placeholder.
            if (prev != kDeviceStateSpeaking) {
                SetTickerText("倾听中");
            }
            break;
        case kDeviceStateSpeaking:
            ShowBadgeLayer(kBfAvatar);
            UpdateHalo(COLOR_CORAL, 178, 255, 1100);   // 70 → 100 %, vocal
            SetDialMode(kDialClock);
            // Don't reset the ticker — let the previous STT text linger
            // until the assistant's first sentence_start arrives via
            // SetChatMessage("assistant", ...). Avoids visible flicker
            // back to version line between user-speak and bot-speak.
            break;
        case kDeviceStateUpgrading:
            if (current_badge_layer_ != kBfUpgrading) ShowBadgeLayer(kBfUpgrading);
            UpdateHalo(COLOR_PEACH, 140, 242, 1400);   // 55 → 95 %
            SetDialMode(kDialProgress);
            break;
        case kDeviceStateAudioTesting:
            ShowBadgeLayer(kBfAvatar);
            UpdateHalo(COLOR_INK, 140, 255, 1600);
            SetDialMode(kDialOff);
            SetTickerText("音频测试中");
            break;
        case kDeviceStateFatalError:
        default:
            ShowBadgeLayer(kBfError);
            UpdateHalo(COLOR_RED, 153, 255, 700);
            SetDialMode(kDialOff);
            break;
    }

    // Low-battery overlay wins over normal halo color.
    if (low_battery_active_) {
        UpdateHalo(COLOR_RED, 102, 217, 2400);
        SetTickerText("电量不足，请尽快充电");
    }
}
