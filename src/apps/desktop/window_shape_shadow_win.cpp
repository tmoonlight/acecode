#ifdef _WIN32

#include "window_shape_shadow_win.hpp"

#include <windowsx.h>

#include <vector>

namespace acecode::desktop {

namespace {

constexpr wchar_t kShadowWindowClassName[] = L"ACECodeChamferShadow";
constexpr wchar_t kGripWindowClassName[] = L"ACECodeChamferResizeGrip";

bool same_style(const ShadowStyle& a, const ShadowStyle& b) {
    return a.margin == b.margin && a.offset_y == b.offset_y && a.active == b.active &&
           a.glow_r == b.glow_r && a.glow_g == b.glow_g && a.glow_b == b.glow_b &&
           a.glow_width == b.glow_width;
}

bool register_window_class(HINSTANCE instance, const wchar_t* name, WNDPROC proc) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = instance;
    wc.lpszClassName = name;
    wc.lpfnWndProc = proc;
    return ::RegisterClassExW(&wc) != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

int hit_test_for_zone(ResizeZone zone) {
    switch (zone) {
        case ResizeZone::Left: return HTLEFT;
        case ResizeZone::Right: return HTRIGHT;
        case ResizeZone::Top: return HTTOP;
        case ResizeZone::Bottom: return HTBOTTOM;
        case ResizeZone::TopLeft: return HTTOPLEFT;
        case ResizeZone::TopRight: return HTTOPRIGHT;
        case ResizeZone::BottomLeft: return HTBOTTOMLEFT;
        case ResizeZone::BottomRight: return HTBOTTOMRIGHT;
        case ResizeZone::None: break;
    }
    return HTNOWHERE;
}

// 建一张 top-down 的 32 位 DIB 交给 fill 写预乘像素,再经 UpdateLayeredWindow 摆到
// 主窗口旁边。
template <typename Fill>
void present_layered(HWND window, const ShadowStrip& strip, const RECT& owner_rect, Fill&& fill) {
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = strip.width;
    info.bmiHeader.biHeight = -strip.height;  // top-down,像素顺序与 render_chamfer_* 一致
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = ::GetDC(nullptr);
    HDC memory = ::CreateCompatibleDC(screen);
    HBITMAP bitmap = ::CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap && bits && memory) {
        fill(static_cast<std::uint32_t*>(bits));
        HGDIOBJ previous = ::SelectObject(memory, bitmap);
        POINT position{owner_rect.left + strip.x, owner_rect.top + strip.y};
        SIZE size{strip.width, strip.height};
        POINT source{0, 0};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        ::UpdateLayeredWindow(window, screen, &position, &size, memory, &source, 0, &blend, ULW_ALPHA);
        ::SelectObject(memory, previous);
    }
    if (bitmap) ::DeleteObject(bitmap);
    if (memory) ::DeleteDC(memory);
    ::ReleaseDC(nullptr, screen);
}

} // namespace

ChamferShadowWindows::~ChamferShadowWindows() {
    destroy();
}

LRESULT CALLBACK ChamferShadowWindows::grip_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_NCHITTEST: {
            const auto* self = reinterpret_cast<const ChamferShadowWindows*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            return self ? self->grip_hit_test(lparam) : HTNOWHERE;
        }
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_NCLBUTTONDOWN: {
            // 光标由 DefWindowProc 按命中值给出;按下时让主窗口进入系统缩放循环,
            // 与点系统隐形缩放边一样先把主窗口激活。
            HWND owner = ::GetWindow(hwnd, GW_OWNER);
            if (wparam >= HTLEFT && wparam <= HTBOTTOMRIGHT && owner && !::IsZoomed(owner)) {
                ::SetForegroundWindow(owner);
                ::ReleaseCapture();
                ::SendMessageW(owner, WM_NCLBUTTONDOWN, wparam, lparam);
            }
            return 0;
        }
        default:
            break;
    }
    return ::DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT ChamferShadowWindows::grip_hit_test(LPARAM lparam) const {
    RECT rect{};
    if (!owner_ || ::IsZoomed(owner_) || !::GetWindowRect(owner_, &rect)) return HTNOWHERE;
    const int x = GET_X_LPARAM(lparam) - rect.left;
    const int y = GET_Y_LPARAM(lparam) - rect.top;
    return hit_test_for_zone(chamfer_resize_zone(x, y, rect.right - rect.left, rect.bottom - rect.top,
                                                 last_cut_, last_grip_, last_reach_));
}

bool ChamferShadowWindows::ensure_created(HWND owner) {
    if (owner_ == owner && windows_[0]) return true;
    destroy();
    HINSTANCE instance = ::GetModuleHandleW(nullptr);
    static const bool registered = register_window_class(instance, kShadowWindowClassName, ::DefWindowProcW) &&
                                   register_window_class(instance, kGripWindowClassName, &grip_proc);
    if (!registered) return false;
    for (auto& window : windows_) {
        window = ::CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            kShadowWindowClassName, L"", WS_POPUP, 0, 0, 0, 0, owner, nullptr, instance, nullptr);
        if (!window) {
            destroy();
            return false;
        }
    }
    // 把手不带 WS_EX_TRANSPARENT:分层窗口只让 alpha 为 0 的像素点穿,把手范围内是 alpha 1。
    for (auto& grip : grips_) {
        grip = ::CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            kGripWindowClassName, L"", WS_POPUP, 0, 0, 0, 0, owner, nullptr, instance, nullptr);
        if (!grip) {
            destroy();
            return false;
        }
        ::SetWindowLongPtrW(grip, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    }
    owner_ = owner;
    rendered_ = false;
    return true;
}

void ChamferShadowWindows::update(HWND owner, int cut, const ShadowStyle& style, int grip, int reach) {
    if (!owner || !::IsWindow(owner) || !ensure_created(owner)) return;
    RECT rect{};
    if (!::GetWindowRect(owner, &rect)) return;
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    const auto strips = chamfer_shadow_strips(width, height, cut, style.margin);
    const auto grip_strips = chamfer_shadow_strips(width, height, cut, grip);
    const bool resized = !rendered_ || width != last_width_ || height != last_height_ || cut != last_cut_;
    const bool render_shadow = resized || !same_style(style, last_style_);
    const bool render_grips = resized || grip != last_grip_ || reach != last_reach_;
    // 命中判定读这几个值,先于重画更新。
    last_width_ = width;
    last_height_ = height;
    last_cut_ = cut;
    last_grip_ = grip;
    last_reach_ = reach;
    last_style_ = style;
    rendered_ = true;

    HDWP batch = ::BeginDeferWindowPos(static_cast<int>(windows_.size() + grips_.size()));
    // 隐藏着的窗口等位置都摆好再显示,免得在旧位置闪一下(最大化还原、窗口从极小变大)。
    std::vector<HWND> to_show;
    const auto place = [&](HWND window, const std::vector<ShadowStrip>& layout, std::size_t i, bool redraw,
                           const auto& fill) {
        if (i >= layout.size()) {
            ::ShowWindow(window, SW_HIDE);
            return;
        }
        const auto& strip = layout[i];
        if (redraw) {
            present_layered(window, strip, rect, [&](std::uint32_t* pixels) { fill(pixels, strip); });
        } else if (batch) {
            batch = ::DeferWindowPos(batch, window, nullptr, rect.left + strip.x, rect.top + strip.y, 0, 0,
                                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        }
        if (!::IsWindowVisible(window)) to_show.push_back(window);
    };
    for (std::size_t i = 0; i < windows_.size(); ++i) {
        place(windows_[i], strips, i, render_shadow, [&](std::uint32_t* pixels, const ShadowStrip& strip) {
            render_chamfer_shadow(pixels, strip, width, height, cut, style);
        });
    }
    for (std::size_t i = 0; i < grips_.size(); ++i) {
        place(grips_[i], grip_strips, i, render_grips, [&](std::uint32_t* pixels, const ShadowStrip& strip) {
            render_chamfer_grip_mask(pixels, strip, width, height, cut, grip, reach);
        });
    }
    if (batch) ::EndDeferWindowPos(batch);
    for (HWND window : to_show) ::ShowWindow(window, SW_SHOWNOACTIVATE);
    visible_ = true;
}

void ChamferShadowWindows::hide() {
    if (!visible_) return;
    for (HWND window : windows_) {
        if (window) ::ShowWindow(window, SW_HIDE);
    }
    for (HWND grip : grips_) {
        if (grip) ::ShowWindow(grip, SW_HIDE);
    }
    visible_ = false;
}

void ChamferShadowWindows::destroy() {
    for (auto& window : windows_) {
        if (window && ::IsWindow(window)) ::DestroyWindow(window);
        window = nullptr;
    }
    for (auto& grip : grips_) {
        if (grip && ::IsWindow(grip)) ::DestroyWindow(grip);
        grip = nullptr;
    }
    owner_ = nullptr;
    rendered_ = false;
    visible_ = false;
}

} // namespace acecode::desktop

#endif // _WIN32
