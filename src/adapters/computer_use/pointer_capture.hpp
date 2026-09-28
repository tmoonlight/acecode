#pragma once

#include "pointer_overlay.hpp"

#ifdef _WIN32
#include <nlohmann/json.hpp>

namespace acecode::computer_use {

// Pure pixel operation: the caller supplies an already authorized sprite.
// Bounds and sprite positions are physical desktop pixels; the tightly packed
// capture has not yet been resized. Returns false without changing invalid input.
bool composite_pointer_sprite(const RECT& bounds, int width, int height,
                              std::vector<unsigned char>& bgra, const PointerSprite& sprite);

// Suppress the helper's overlay HWND before calling, while retaining its logical
// snapshot. A visible agent sprite takes precedence even when outside this target.
// x/y describe the hotspot relative to the native capture, before PNG resizing.
nlohmann::json composite_capture_pointer(HWND target, const RECT& bounds, int width, int height,
                                         std::vector<unsigned char>& bgra,
                                         const PointerSprite* agent = nullptr);

} // namespace acecode::computer_use
#endif
