#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "window_background.hpp"
#include "window_shape_shadow_win.hpp"

namespace acecode::desktop::detail {

// Native frame state is accessed only by the host GUI thread.
struct WindowShapeState {
    bool chamfer = false;
    std::optional<std::uint32_t> glow_rgb;
    bool region_set = false;
    int region_width = -1;
    int region_height = -1;
    int region_cut = -1;
};
extern WindowShapeState g_window_shape;
extern ChamferShadowWindows g_chamfer_shadow;
extern bool g_top_border_active;
extern WindowBackgroundColor g_top_border_background;

int dpi_scale(int value, UINT dpi);
int top_border_inset(HWND hwnd);
void invalidate_top_border(HWND hwnd);
bool paint_top_border(HWND hwnd);
void resize_webview_widget(HWND hwnd);
void apply_window_shape_region(HWND hwnd);
void update_window_shape_shadow(HWND hwnd);
void refresh_non_client_frame(HWND hwnd);

} // namespace acecode::desktop::detail
#endif
