#include "pointer_capture.hpp"

#ifdef _WIN32
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>

namespace acecode::computer_use {
namespace {

bool valid_buffer(int width, int height, std::size_t bytes) {
    if (width <= 0 || height <= 0) return false;
    const auto pixels = static_cast<std::uint64_t>(width) * height;
    return pixels <= (std::numeric_limits<std::size_t>::max)() / 4 && bytes == pixels * 4;
}

bool valid_capture(const RECT& bounds, int width, int height, std::size_t bytes) {
    return static_cast<std::int64_t>(bounds.right) - bounds.left == width
        && static_cast<std::int64_t>(bounds.bottom) - bounds.top == height
        && valid_buffer(width, height, bytes);
}

struct Placement {
    int x = 0, y = 0;
    int left = 0, top = 0, width = 0, height = 0;
    int source_x = 0, source_y = 0;
};

std::optional<Placement> place(const RECT& bounds, int width, int height, POINT position,
                                int sprite_width, int sprite_height, int hotspot_x, int hotspot_y) {
    if (sprite_width <= 0 || sprite_height <= 0 || hotspot_x < 0 || hotspot_x >= sprite_width
        || hotspot_y < 0 || hotspot_y >= sprite_height) return std::nullopt;
    const auto x = static_cast<std::int64_t>(position.x) - bounds.left;
    const auto y = static_cast<std::int64_t>(position.y) - bounds.top;
    // A partially overlapping glyph whose hotspot is outside this surface does
    // not authorize copying a pointer from another window into its screenshot.
    if (x < 0 || y < 0 || x >= width || y >= height) return std::nullopt;
    const auto left = x - hotspot_x, top = y - hotspot_y;
    const auto clipped_left = (std::max)(std::int64_t{0}, left);
    const auto clipped_top = (std::max)(std::int64_t{0}, top);
    const auto right = (std::min)(static_cast<std::int64_t>(width), left + sprite_width);
    const auto bottom = (std::min)(static_cast<std::int64_t>(height), top + sprite_height);
    if (right <= clipped_left || bottom <= clipped_top) return std::nullopt;
    return Placement{static_cast<int>(x), static_cast<int>(y),
        static_cast<int>(clipped_left), static_cast<int>(clipped_top),
        static_cast<int>(right - clipped_left), static_cast<int>(bottom - clipped_top),
        static_cast<int>(clipped_left - left), static_cast<int>(clipped_top - top)};
}

bool owns_hotspot(HWND target, const RECT& bounds, POINT position) {
    if (!target || !PtInRect(&bounds, position)) return false;
    const auto hit = WindowFromPoint(position);
    return hit && (hit == target || IsChild(target, hit));
}

nlohmann::json metadata(const char* source, const Placement& placement,
                        int width, int height, int hotspot_x, int hotspot_y) {
    return {{"visible", true}, {"source", source}, {"x", placement.x}, {"y", placement.y},
        {"hotspot_x", hotspot_x}, {"hotspot_y", hotspot_y}, {"width", width}, {"height", height}};
}

struct Icon {
    HICON value = nullptr;
    ~Icon() { if (value) DestroyIcon(value); }
};

struct IconBitmaps {
    ICONINFO value{};
    ~IconBitmaps() {
        if (value.hbmColor) DeleteObject(value.hbmColor);
        if (value.hbmMask) DeleteObject(value.hbmMask);
    }
};

struct Dib {
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previous = nullptr;
    unsigned char* bytes = nullptr;
    ~Dib() {
        if (previous && previous != HGDI_ERROR) SelectObject(dc, previous);
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
    }
    bool create(int width, int height) {
        dc = CreateCompatibleDC(nullptr);
        if (!dc) return false;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* data = nullptr;
        bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &data, nullptr, 0);
        if (!bitmap || !data) return false;
        bytes = static_cast<unsigned char*>(data);
        previous = SelectObject(dc, bitmap);
        return previous && previous != HGDI_ERROR;
    }
};

bool bitmap_pixels(HBITMAP bitmap, int width, int height, std::vector<unsigned char>& bytes) {
    // Cursor resources are untrusted application data. Bound the temporary copy
    // independently from the already bounded screenshot buffer (16 MiB maximum).
    if (width <= 0 || height <= 0 || static_cast<std::uint64_t>(width) * height > 4 * 1024 * 1024)
        return false;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    Dib context;
    context.dc = CreateCompatibleDC(nullptr);
    if (!context.dc) return false;
    bytes.resize(static_cast<std::size_t>(width) * height * 4);
    return GetDIBits(context.dc, bitmap, 0, static_cast<UINT>(height), bytes.data(), &info, DIB_RGB_COLORS) == height;
}

// Alpha cursors use AC_SRC_OVER. Legacy and monochrome cursors instead combine
// AND/XOR masks, including inverted pixels, so DrawIconEx must see the actual
// screenshot background. Extract coverage only to repair GDI's unused alpha byte.
bool cursor_coverage(const ICONINFO& icon, int width, int height, const BITMAP& color,
                     std::vector<unsigned char>& alpha) {
    std::vector<unsigned char> pixels;
    const auto count = static_cast<std::size_t>(width) * height;
    if (icon.hbmColor && color.bmBitsPixel == 32) {
        if (!bitmap_pixels(icon.hbmColor, width, height, pixels)) return false;
        bool has_alpha = false;
        for (std::size_t index = 3; index < pixels.size(); index += 4)
            if (pixels[index]) { has_alpha = true; break; }
        if (has_alpha) {
            alpha.resize(count);
            for (std::size_t index = 0; index < count; ++index) alpha[index] = pixels[index * 4 + 3];
            return true;
        }
    }
    BITMAP mask{};
    if (!icon.hbmMask || GetObjectW(icon.hbmMask, sizeof(mask), &mask) != sizeof(mask)
        || mask.bmWidth != width || mask.bmHeight < height
        || !bitmap_pixels(icon.hbmMask, mask.bmWidth, mask.bmHeight, pixels)) return false;
    alpha.resize(count);
    // A monochrome resource stores AND above XOR. A white AND pixel preserves
    // destination alpha even if its XOR pixel inverts the destination color.
    for (std::size_t index = 0; index < count; ++index) alpha[index] = pixels[index * 4] ? 0 : 255;
    return true;
}

bool draw_cursor(HICON icon, const Placement& p, int icon_width, int icon_height,
                 const std::vector<unsigned char>& alpha, int width, std::vector<unsigned char>& bgra) {
    Dib patch;
    if (!patch.create(p.width, p.height)) return false;
    for (int row = 0; row < p.height; ++row) {
        const auto destination = (static_cast<std::size_t>(p.top + row) * width + p.left) * 4;
        std::memcpy(patch.bytes + static_cast<std::size_t>(row) * p.width * 4,
                    bgra.data() + destination, static_cast<std::size_t>(p.width) * 4);
    }
    if (!DrawIconEx(patch.dc, -p.source_x, -p.source_y, icon, icon_width, icon_height,
                    0, nullptr, DI_NORMAL | DI_NOMIRROR)) return false;
    GdiFlush();
    for (int row = 0; row < p.height; ++row) {
        for (int column = 0; column < p.width; ++column) {
            const auto destination = (static_cast<std::size_t>(p.top + row) * width + p.left + column) * 4;
            const auto patch_offset = (static_cast<std::size_t>(row) * p.width + column) * 4;
            const auto coverage = alpha[static_cast<std::size_t>(p.source_y + row) * icon_width + p.source_x + column];
            for (int channel = 0; channel < 3; ++channel) bgra[destination + channel] = patch.bytes[patch_offset + channel];
            bgra[destination + 3] = static_cast<unsigned char>(coverage +
                (static_cast<unsigned>(bgra[destination + 3]) * (255 - coverage) + 127) / 255);
        }
    }
    return true;
}

} // namespace

bool composite_pointer_sprite(const RECT& bounds, int width, int height,
                              std::vector<unsigned char>& bgra, const PointerSprite& sprite) {
    if (!sprite.visible || !valid_capture(bounds, width, height, bgra.size())
        || !valid_buffer(sprite.width, sprite.height, sprite.bgra.size())) return false;
    const auto p = place(bounds, width, height, sprite.position, sprite.width, sprite.height, sprite.hotspot_x, sprite.hotspot_y);
    if (!p) return false;
    for (int row = 0; row < p->height; ++row) {
        for (int column = 0; column < p->width; ++column) {
            const auto destination = (static_cast<std::size_t>(p->top + row) * width + p->left + column) * 4;
            const auto source = (static_cast<std::size_t>(p->source_y + row) * sprite.width + p->source_x + column) * 4;
            const auto inverse_alpha = 255 - sprite.bgra[source + 3];
            for (int channel = 0; channel < 4; ++channel) {
                const auto blended = sprite.bgra[source + channel] +
                    (static_cast<unsigned>(bgra[destination + channel]) * inverse_alpha + 127) / 255;
                bgra[destination + channel] = static_cast<unsigned char>((std::min)(255U, blended));
            }
        }
    }
    return true;
}

nlohmann::json composite_capture_pointer(HWND target, const RECT& bounds, int width, int height,
                                         std::vector<unsigned char>& bgra, const PointerSprite* agent) {
    const nlohmann::json absent{{"visible", false}};
    if (!valid_capture(bounds, width, height, bgra.size())) return absent;
    if (agent && agent->visible) {
        if (!owns_hotspot(target, bounds, agent->position)
            || !composite_pointer_sprite(bounds, width, height, bgra, *agent)) return absent;
        const auto p = place(bounds, width, height, agent->position, agent->width, agent->height, agent->hotspot_x, agent->hotspot_y);
        return metadata("agent", *p, agent->width, agent->height, agent->hotspot_x, agent->hotspot_y);
    }
    CURSORINFO cursor{};
    cursor.cbSize = sizeof(cursor);
    if (!GetCursorInfo(&cursor) || !(cursor.flags & CURSOR_SHOWING)
        || !owns_hotspot(target, bounds, cursor.ptScreenPos)) return absent;
    Icon copy;
    copy.value = CopyIcon(cursor.hCursor);
    IconBitmaps bitmaps;
    if (!copy.value || !GetIconInfo(copy.value, &bitmaps.value)) return absent;
    BITMAP shape{};
    const auto bitmap = bitmaps.value.hbmColor ? bitmaps.value.hbmColor : bitmaps.value.hbmMask;
    if (!bitmap || GetObjectW(bitmap, sizeof(shape), &shape) != sizeof(shape)
        || shape.bmWidth <= 0 || shape.bmHeight <= 0) return absent;
    const int icon_width = shape.bmWidth;
    if (!bitmaps.value.hbmColor && shape.bmHeight % 2 != 0) return absent;
    const int icon_height = bitmaps.value.hbmColor ? shape.bmHeight : shape.bmHeight / 2;
    if (bitmaps.value.xHotspot >= static_cast<DWORD>(icon_width)
        || bitmaps.value.yHotspot >= static_cast<DWORD>(icon_height)) return absent;
    const auto hotspot_x = static_cast<int>(bitmaps.value.xHotspot);
    const auto hotspot_y = static_cast<int>(bitmaps.value.yHotspot);
    const auto p = place(bounds, width, height, cursor.ptScreenPos, icon_width, icon_height, hotspot_x, hotspot_y);
    std::vector<unsigned char> coverage;
    if (!p || !cursor_coverage(bitmaps.value, icon_width, icon_height, shape, coverage)
        || !draw_cursor(copy.value, *p, icon_width, icon_height, coverage, width, bgra)) return absent;
    return metadata("system", *p, icon_width, icon_height, hotspot_x, hotspot_y);
}

} // namespace acecode::computer_use
#endif
