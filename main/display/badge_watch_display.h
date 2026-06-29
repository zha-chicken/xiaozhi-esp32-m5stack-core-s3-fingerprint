#ifndef BADGE_WATCH_DISPLAY_H
#define BADGE_WATCH_DISPLAY_H

#include "lcd_display.h"
#include "device_state_event.h"
#include "user_avatar_sync.h"

#include <lvgl.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_timer.h>

#include <array>
#include <memory>
#include <string>

/* ─────────────────────────────────────────────────────────────────────
 * Hao Lab "badge-watch" custom GUI, extracted from the Waveshare
 * ESP32-C6-LCD-1.69 board file into a shared, board-parametrized base.
 *
 * Mockup source-of-truth: tmp/badge-watch.html.
 *
 * Composition (outer rim inward):
 *   • cream surface (+ optional 4×4 ink grid bg via config.bg_tile)
 *   • 60-tick dial: cardinal/major/minor backgrounds + clock/progress overlays
 *   • halo: always-on color circle behind the avatar, breathing intensity
 *   • central circular badge zone: 7 pre-built layers, swap by visibility
 *       1) avatar  2) boot  3) wifi-config  4) activating
 *       5) pairing 6) upgrading  7) error
 *   • 4 corner status slots: TL Wi-Fi (opacity steps), TR battery, BL/BR hidden
 *   • optional bottom ticker (config.show_ticker): static or marquee
 *
 * Everything board-specific (geometry, the baked default avatar, the grid
 * tile texture, ticker/version-label presence) is supplied via
 * BadgeWatchConfig at construction time. The class references NEITHER a
 * board-local avatar asset NOR a board-local bg tile directly — both arrive
 * through the config, nullptr-safe. The text font font_puhui_16_4 and the
 * lv_font_montserrat_28/36/44 fonts are built-in / managed-component fonts
 * (board-agnostic) and are referenced directly.
 *
 * The kLightweightBadgeUi target check (true on ESP32-C6: no halo animation,
 * "..." instead of a real spinner) stays a compile-time decision inside the
 * shared class — see badge_watch_display.cc.
 * ────────────────────────────────────────────────────────────────────── */
struct BadgeWatchConfig {
    int badge_radius;          // central badge (avatar circle) radius
    int ring_radius;           // dial tick outer tip
    int halo_radius;           // breathing halo
    int center_y_offset;       // vertical shift of composition from geometric center
    int tick_len_cardinal;     // dial tick lengths
    int tick_len_major;
    int tick_len_minor;
    int hour_mark_w;
    int hour_mark_h;
    bool show_ticker;          // bottom chat/transcript ticker strip
    bool show_version_label;   // keep the inherited bottom "Hao Lab v.." label
    const lv_img_dsc_t* default_avatar; // nullptr => no baked default (spinner holds)
    const lv_img_dsc_t* bg_tile;        // nullptr => solid cream, no grid texture
};

class BadgeWatchDisplay : public SpiLcdDisplay {
public:
    BadgeWatchDisplay(esp_lcd_panel_io_handle_t io_handle,
                      esp_lcd_panel_handle_t panel_handle,
                      int width,
                      int height,
                      int offset_x,
                      int offset_y,
                      bool mirror_x,
                      bool mirror_y,
                      bool swap_xy,
                      const BadgeWatchConfig& config);

    // ── Public overrides ─────────────────────────────────────────────
    virtual void SetEmotion(const char* emotion) override;
    virtual void SetUserAvatar(std::unique_ptr<LvglImage> image) override;
    virtual void UpdateStatusBar(bool update_all = false) override;
    virtual void SetPowerSaveMode(bool on) override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void SetSecurityLock(bool locked) override;
    void ShowMemoList(const char* text);
    bool HideMemoList();

private:
    // ── Tiny extractors used by SetChatMessage routing ────────────────
    static std::string ExtractSixDigits(const std::string& s);
    static std::string ExtractAfter(const std::string& s, const char* marker);
    static std::string ExtractIp(const std::string& s);

    enum DialMode { kDialOff, kDialClock, kDialProgress };

    // Indices into bf_layers_; matches order in BuildBadgeLayers().
    enum BadgeLayer {
        kBfAvatar = 0,
        kBfBoot,
        kBfWifi,
        kBfActivating,
        kBfPairing,
        kBfUpgrading,
        kBfError,
        kBfLocked,
        kBfCount
    };

    // ── Build pipeline ────────────────────────────────────────────────
    void BuildBadgeLayout();
    void BuildBackground();
    void BuildHalo();
    static void HaloOpaCb(void* var, int32_t value);
    void UpdateHalo(uint32_t color, uint8_t opa_min, uint8_t opa_max, uint32_t period_ms);
    void FreezeHalo(uint8_t opa);
    void BuildDial();
    void UpdateHourMark(int hour, int minute);
    void SetDialMode(DialMode mode);
    void RefreshClock();
    void UpdateProgress(int pct);
    void BuildBadgeLayers();
    void ShowBadgeLayer(BadgeLayer idx);
    void BuildCorners();
    void BuildTicker();
    void StartClockTimer();
    void SetTickerText(const char* text);
    const char* AppVersionString();
    void ApplyState(DeviceState state);

    // ── Board-specific config (resolved at construction) ──────────────
    BadgeWatchConfig config_{};

    // ── Geometry instance members (initialized from config_) ──────────
    int badge_radius_ = 0;
    int ring_radius_ = 0;
    int halo_radius_ = 0;
    int tick_bg_len_cardinal_ = 0;
    int tick_bg_len_major_ = 0;
    int tick_bg_len_minor_ = 0;
    int hour_mark_w_ = 0;
    int hour_mark_h_ = 0;

    // ── State + widgets (populated by Build*) ─────────────────────────
    DeviceState current_state_ = kDeviceStateUnknown;
    DialMode    current_dial_mode_ = kDialOff;
    // Sentinel so the very first ShowBadgeLayer() call always toggles
    // visibility.
    BadgeLayer  current_badge_layer_ = kBfCount;
    int         progress_pct_ = 0;
    bool        low_battery_active_ = false;
    bool        security_locked_ = false;

    // Dial overlays
    lv_obj_t*   hour_mark_   = nullptr;
    lv_obj_t*   minute_arc_  = nullptr;

    // Halo (single-color breathing circle)
    lv_obj_t*   halo_        = nullptr;
    lv_anim_t   halo_anim_;
    bool        halo_anim_running_ = false;

    // Pre-built badge layers — siblings inside emoji_box_
    std::array<lv_obj_t*, kBfCount> bf_layers_ = {};
    std::unique_ptr<LvglImage> user_avatar_image_ = nullptr;
    bool user_avatar_ready_ = false;
    // Sub-widgets needed for runtime updates
    lv_obj_t*   bf_pairing_code_   = nullptr;
    lv_obj_t*   bf_upgrading_pct_  = nullptr;
    lv_obj_t*   bf_upgrading_size_ = nullptr;
    lv_obj_t*   bf_wifi_ap_   = nullptr;
    lv_obj_t*   bf_wifi_ip_   = nullptr;
    lv_obj_t*   bf_error_text_ = nullptr;
    lv_obj_t*   memo_panel_ = nullptr;
    lv_obj_t*   memo_text_ = nullptr;
    bool        memo_panel_visible_ = false;

    // Periodic clock refresh (1-minute resolution)
    esp_timer_handle_t clock_timer_ = nullptr;

    // Last seen emotion string (drives bf-layer routing in SetChatMessage).
    std::string current_emotion_;

    // Center coords (resolved from runtime panel size in BuildBadgeLayout).
    int center_x_ = 0;
    int center_y_ = 0;
};

#endif // BADGE_WATCH_DISPLAY_H
