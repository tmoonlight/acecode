#include "taskbar_badge_win.hpp"

#ifdef _WIN32
#include <commctrl.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace acecode::desktop {
namespace {

struct BadgeGraphicsRuntime {
    ULONG_PTR token = 0;
    BadgeGraphicsRuntime() {
        Gdiplus::GdiplusStartupInput input;
        if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) token = 0;
    }
    ~BadgeGraphicsRuntime() {
        if (token) Gdiplus::GdiplusShutdown(token);
    }
};

Gdiplus::Color badge_color(WindowBackgroundColor color) {
    return Gdiplus::Color(255, color.r, color.g, color.b);
}

} // namespace

HICON create_taskbar_badged_icon(HICON original, int size, const TaskbarBadge& badge) {
    if (!original || size < 16 || size > 256 || badge.count <= 0) return nullptr;
    static BadgeGraphicsRuntime runtime;
    if (!runtime.token) return nullptr;

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP color_bitmap = ::CreateDIBSection(nullptr, &info, DIB_RGB_COLORS,
                                             &pixels, nullptr, 0);
    if (!color_bitmap) return nullptr;
    std::memset(pixels, 0, static_cast<std::size_t>(size) * size * 4);
    HDC dc = ::CreateCompatibleDC(nullptr);
    if (!dc) {
        ::DeleteObject(color_bitmap);
        return nullptr;
    }
    HGDIOBJ previous = ::SelectObject(dc, color_bitmap);
    // Leave space above the logo so the badge does not hide its main lettering.
    const int logo_size = size * 7 / 8;
    const bool logo_drawn = ::DrawIconEx(dc, 0, size - logo_size, original,
                                       logo_size, logo_size, 0, nullptr, DI_NORMAL) != 0;
    ::SelectObject(dc, previous);
    ::DeleteDC(dc);
    if (!logo_drawn) {
        ::DeleteObject(color_bitmap);
        return nullptr;
    }

    bool drawn = false;
    {
        Gdiplus::Bitmap bitmap(size, size, size * 4, PixelFormat32bppPARGB,
                              static_cast<BYTE*>(pixels));
        Gdiplus::Graphics graphics(&bitmap);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
        const float diameter = size * 0.64f;
        const float border = std::max(1.0f, size * 0.035f);
        Gdiplus::RectF circle(size - diameter, 0.0f, diameter, diameter);
        Gdiplus::SolidBrush outline(badge_color(badge.outline));
        Gdiplus::SolidBrush background(badge_color(badge.background));
        Gdiplus::SolidBrush foreground(badge_color(badge.foreground));
        graphics.FillEllipse(&outline, circle);
        circle.Inflate(-border, -border);
        graphics.FillEllipse(&background, circle);
        const auto label = taskbar_badge_label(badge.count);
        const std::wstring text(label.begin(), label.end());
        const float font_size = size * (label.size() == 1 ? 0.43f : label.size() == 2 ? 0.35f : 0.24f);
        Gdiplus::Font font(L"Segoe UI", font_size, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::StringFormat format(Gdiplus::StringFormat::GenericTypographic());
        format.SetAlignment(Gdiplus::StringAlignmentCenter);
        format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        format.SetFormatFlags(format.GetFormatFlags() | Gdiplus::StringFormatFlagsNoWrap);
        drawn = graphics.DrawString(text.c_str(), -1, &font, circle, &format,
                                    &foreground) == Gdiplus::Ok;
        graphics.Flush(Gdiplus::FlushIntentionSync);
    }
    HICON icon = nullptr;
    if (drawn) {
        std::vector<BYTE> mask_bits(static_cast<std::size_t>((size + 15) / 16) * 2 * size, 0);
        HBITMAP mask = ::CreateBitmap(size, size, 1, 1, mask_bits.data());
        if (mask) {
            ICONINFO icon_info{};
            icon_info.fIcon = TRUE;
            icon_info.hbmColor = color_bitmap;
            icon_info.hbmMask = mask;
            icon = ::CreateIconIndirect(&icon_info);
            ::DeleteObject(mask);
        }
    }
    ::DeleteObject(color_bitmap);
    return icon;
}

WindowsTaskbarBadge::WindowsTaskbarBadge(HWND window) : window_(window) {
    original_large_ = reinterpret_cast<HICON>(::SendMessageW(window_, WM_GETICON, ICON_BIG, 0));
    original_small_ = reinterpret_cast<HICON>(::SendMessageW(window_, WM_GETICON, ICON_SMALL, 0));
    taskbar_created_ = ::RegisterWindowMessageW(L"TaskbarButtonCreated");
    ::SetWindowSubclass(window_, window_proc, reinterpret_cast<UINT_PTR>(this),
                        reinterpret_cast<DWORD_PTR>(this));
}

WindowsTaskbarBadge::~WindowsTaskbarBadge() {
    if (window_ && ::IsWindow(window_)) {
        ::RemoveWindowSubclass(window_, window_proc, reinterpret_cast<UINT_PTR>(this));
        apply_icons(original_large_, original_small_);
    }
    release_badged_icons();
}

void WindowsTaskbarBadge::apply_icons(HICON large_icon, HICON small_icon) {
    ::SendMessageW(window_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(large_icon));
    ::SendMessageW(window_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small_icon));
}

void WindowsTaskbarBadge::release_badged_icons() {
    if (badged_large_) ::DestroyIcon(badged_large_);
    if (badged_small_) ::DestroyIcon(badged_small_);
    badged_large_ = nullptr;
    badged_small_ = nullptr;
}

bool WindowsTaskbarBadge::set(const TaskbarBadge& badge) {
    if (!window_ || !::IsWindow(window_) || badge.count < 0) return false;
    if (badge == badge_) return true;
    const auto previous = badge_;
    badge_ = badge;
    if (refresh()) return true;
    badge_ = previous;
    return false;
}

bool WindowsTaskbarBadge::refresh() {
    if (!window_ || !::IsWindow(window_)) return false;
    if (badge_.count == 0) {
        apply_icons(original_large_, original_small_);
        release_badged_icons();
        return true;
    }
    UINT dpi = ::GetDpiForWindow(window_);
    if (!dpi) dpi = 96;
    HICON large_icon = create_taskbar_badged_icon(
        original_large_, ::GetSystemMetricsForDpi(SM_CXICON, dpi), badge_);
    HICON small_icon = create_taskbar_badged_icon(
        original_small_, ::GetSystemMetricsForDpi(SM_CXSMICON, dpi), badge_);
    if (!large_icon || !small_icon) {
        if (large_icon) ::DestroyIcon(large_icon);
        if (small_icon) ::DestroyIcon(small_icon);
        return false;
    }
    apply_icons(large_icon, small_icon);
    release_badged_icons();
    badged_large_ = large_icon;
    badged_small_ = small_icon;
    return true;
}

LRESULT CALLBACK WindowsTaskbarBadge::window_proc(
    HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR data) {
    auto* badge = reinterpret_cast<WindowsTaskbarBadge*>(data);
    if (message == WM_NCDESTROY) {
        ::RemoveWindowSubclass(window, window_proc, id);
        badge->window_ = nullptr;
        return ::DefSubclassProc(window, message, wparam, lparam);
    }
    const auto result = ::DefSubclassProc(window, message, wparam, lparam);
    if (message == WM_DPICHANGED ||
        (badge->taskbar_created_ && message == badge->taskbar_created_)) {
        badge->refresh();
    }
    return result;
}

} // namespace acecode::desktop
#endif
