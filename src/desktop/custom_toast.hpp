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
//     system light/dark preference, with the accent color reduced to a bar
//     along the leading edge.
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
// queue is saturated. Never blocks on the render thread.
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

} // namespace acecode::desktop::custom_toast
