#pragma once

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <memory>
#include <string>
#include <vector>

namespace acecode::computer_use {

struct PointerSprite {
    bool visible = false;
    POINT position{};
    int width = 0;
    int height = 0;
    int hotspot_x = 0;
    int hotspot_y = 0;
    // Premultiplied BGRA, with tightly packed rows.
    std::vector<unsigned char> bgra;
};

// A helper-owned visual indicator. It never changes the system cursor, takes
// foreground focus, or injects input. All HWND access belongs to its UI thread.
class PointerOverlay {
public:
    PointerOverlay();
    ~PointerOverlay();
    PointerOverlay(const PointerOverlay&) = delete;
    PointerOverlay& operator=(const PointerOverlay&) = delete;

    void show(POINT target, UINT dpi = 96, bool pressed = false);
    void configure(const std::string& style, const std::string& color);
    void hide();
    // Synchronous and nestable. Suppression hides only the HWND; snapshot()
    // retains the logical sprite so captures can composite it exactly once.
    void suppress(bool suppressed);
    void settle();
    PointerSprite snapshot() const;
    static bool owns_window(HWND window);
    HWND window() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace acecode::computer_use
#endif
