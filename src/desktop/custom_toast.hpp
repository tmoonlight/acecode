#pragma once

// Self-drawn Windows notification toasts.
//
// Modeled on Electron's `win32_desktop_notifications` — the renderer Electron
// used before it moved to WinRT toasts. See LICENSES/MIT-electron.txt.
// The shape of the solution is the same: one hidden controller window owning a
// 15 ms animation timer, a bottom-right stack of layered `WS_POPUP` windows
// drawn with GDI into a memory DC and pushed to the compositor with
// `UpdateLayeredWindow`, an ease-in on appearance, an alpha ease-out on
// dismissal, and a collapse animation that closes the gap when a toast in the
// middle of the stack goes away.
//
// Two deliberate departures from Electron:
//
//  1. Electron drives the stack from the browser UI thread. ACECode has to
//     serve the FTXUI terminal UI (no Win32 message pump at all) and the
//     desktop shell from the same API, so the controller owns a dedicated
//     thread with its own message loop and `show()` is an async post.
//  2. Electron paints an opaque rectangle tinted from the DWM accent color.
//     ACECode paints a rounded card with per-pixel alpha, following the
//     system light/dark preference (no leading accent stripe).
//
// Everything below the platform surface is pure arithmetic so it can be unit
// tested on Linux/macOS CI where no Win32 renderer exists.

#include "notifications.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace acecode::desktop::custom_toast {

// ---------------------------------------------------------------------------
// Platform surface. Non-Windows builds compile safe no-ops.
// ---------------------------------------------------------------------------

struct InitOptions {
    std::string app_name = "ACECode";
    // Used to pick the monitor the stack is anchored to. May be null.
    void* activation_window = nullptr;
    // Play the system notification sound on each toast.
    bool play_sound = true;
};

// Starts the controller thread. Safe to call repeatedly; only the first call
// does work. Returns false when the renderer could not be started.
bool initialize(const InitOptions& options);

bool is_available();

// Queues one toast. Returns false when the renderer is not running or the
// controller message could not be posted. Never blocks on the render thread;
// an already saturated controller backlog may still drop the toast.
bool show(const NotifyPayload& payload);

// Dismisses everything on screen and stops the controller thread.
void shutdown();

// ---------------------------------------------------------------------------
// Pure layout / animation logic.
// ---------------------------------------------------------------------------

inline constexpr unsigned kBaseDpi = 96;
// Toasts beyond this stay queued until a visible slot frees up.
inline constexpr std::size_t kMaxVisibleToasts = 3;
inline constexpr std::size_t kMaxQueuedToasts = 32;

inline constexpr std::uint32_t kEaseInDurationMs = 320;
inline constexpr std::uint32_t kEaseOutDurationMs = 160;
inline constexpr std::uint32_t kStackCollapseDurationMs = 400;

int scale_for_dpi(int value, unsigned dpi);

// Decelerating exponential ease, matching Electron's curve. Returns 0 at
// `elapsed_ms == 0` and 1 once the duration has elapsed.
float ease_in_position(std::uint32_t elapsed_ms, std::uint32_t duration_ms);
// Accelerating circle ease used for the fade-out alpha ramp.
float ease_out_position(std::uint32_t elapsed_ms, std::uint32_t duration_ms);
// Same curve as the ease-in; drives the stack closing its gaps.
float stack_collapse_position(std::uint32_t elapsed_ms,
                              std::uint32_t duration_ms);

// SPI_GETMESSAGEDURATION is an accessibility setting and can be absurd in
// either direction; clamp it to something a notification can live with.
unsigned clamp_auto_dismiss_seconds(unsigned raw_seconds);

struct ToastRect {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
};

struct ToastPlacement {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

// Anchors a toast to the bottom-right corner of `work_area`, `vertical_offset`
// pixels above the corner. `ease_in` in [0,1] shrinks the width while keeping
// the right edge pinned, which reads as a slide-in from the screen edge.
ToastPlacement compute_toast_placement(const ToastRect& work_area,
                                       int margin_x,
                                       int margin_y,
                                       int width,
                                       int height,
                                       int vertical_offset,
                                       float ease_in);

// Bottom-up stack offsets: element i is the distance between the bottom of the
// work area and the bottom of toast i.
std::vector<int> compute_stack_offsets(const std::vector<int>& heights,
                                       int margin);

// Number of body lines that fit into `available_height` given `line_height`,
// capped at `max_lines`.
int fit_body_lines(int available_height, int line_height, int max_lines);

// Soft drop shadow chrome, matching the tray popup look (blur + slight
// downward offset + transparent padding around the card surface).
inline constexpr int kShadowBlurDip = 12;
inline constexpr int kShadowOffsetYDip = 2;
inline constexpr int kShadowMaxAlpha = 46;
inline constexpr int kChromeInsetDip = 16;

struct ToastChromeGeometry {
    int window_x = 0;
    int window_y = 0;
    int window_width = 0;
    int window_height = 0;
    // Surface origin is relative to the window bitmap (usually == chrome_inset).
    int surface_left = 0;
    int surface_top = 0;
    int surface_width = 0;
    int surface_height = 0;
};

// Expands a surface placement by `chrome_inset` so the layered window has room
// for the soft drop shadow while the visible card stays pinned to the same
// bottom-right anchor.
ToastChromeGeometry compute_toast_chrome_geometry(int surface_x,
                                                  int surface_y,
                                                  int surface_width,
                                                  int surface_height,
                                                  int chrome_inset);

// Signed distance to a rounded rectangle centered on the surface bounds.
// Negative inside, zero on the edge, positive outside.
double toast_rounded_rect_distance(double x,
                                   double y,
                                   int width,
                                   int height,
                                   int radius);

// Coverage 0..255 for antialiased surface edges from a signed distance.
std::uint8_t toast_surface_coverage(double signed_distance);

// Soft shadow alpha 0..max from a signed distance outside the card silhouette.
// `signed_distance` should already include any vertical shadow offset.
std::uint8_t toast_shadow_alpha(double signed_distance,
                                int blur,
                                int max_alpha);

} // namespace acecode::desktop::custom_toast
