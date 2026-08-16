#include "splash_screen.hpp"

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>

#  include <algorithm>
#  include <cmath>
#  include <cstring>
#  include <cstdint>
#  include <filesystem>
#  include <string>
#endif

namespace acecode::desktop {

#ifdef _WIN32

namespace {

constexpr wchar_t kSplashClassName[] = L"ACECodeSplashWindow";
constexpr wchar_t kAppIconResourceName[] = L"IDI_ICON1";
constexpr int kAppIconResourceId = 1;

HMONITOR active_monitor() {
    if (HWND fg = ::GetForegroundWindow()) {
        if (HMONITOR m = ::MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST)) {
            return m;
        }
    }
    POINT pt{};
    if (::GetCursorPos(&pt)) {
        if (HMONITOR m = ::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST)) {
            return m;
        }
    }
    return ::MonitorFromWindow(nullptr, MONITOR_DEFAULTTOPRIMARY);
}

RECT active_monitor_rect() {
    RECT fallback{
        0,
        0,
        ::GetSystemMetrics(SM_CXSCREEN),
        ::GetSystemMetrics(SM_CYSCREEN),
    };
    HMONITOR monitor = active_monitor();
    if (!monitor) return fallback;
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!::GetMonitorInfoW(monitor, &mi)) return fallback;
    return mi.rcMonitor;
}

int choose_icon_size(const RECT& rc) {
    const int w = std::max(1, static_cast<int>(rc.right - rc.left));
    const int h = std::max(1, static_cast<int>(rc.bottom - rc.top));
    const int bounded = std::min({256, std::max(160, std::min(w, h) / 6)});
    return bounded;
}

std::wstring utf8_to_wide(const std::string& value) {
    if (value.empty()) return {};
    int length = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring out(static_cast<std::size_t>(length), L'\0');
    if (::MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), out.data(), length) <= 0) {
        return {};
    }
    return out;
}

std::uint32_t premultiplied_bgra(unsigned char red,
                                 unsigned char green,
                                 unsigned char blue,
                                 unsigned char alpha) {
    const auto premultiply = [alpha](unsigned char channel) {
        return (static_cast<std::uint32_t>(channel) * alpha + 127u) / 255u;
    };
    return (static_cast<std::uint32_t>(alpha) << 24u) |
           (premultiply(red) << 16u) |
           (premultiply(green) << 8u) |
           premultiply(blue);
}

void composite_grayscale_text(std::uint32_t* target,
                              const std::uint32_t* mask,
                              int bitmap_width,
                              int bitmap_height,
                              const RECT& rect) {
    if (!target || !mask || bitmap_width <= 0 || bitmap_height <= 0) return;
    const int left = std::clamp(static_cast<int>(rect.left), 0, bitmap_width);
    const int right = std::clamp(static_cast<int>(rect.right), 0, bitmap_width);
    const int top = std::clamp(static_cast<int>(rect.top), 0, bitmap_height);
    const int bottom = std::clamp(static_cast<int>(rect.bottom), 0, bitmap_height);
    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            const auto offset = static_cast<std::size_t>(y) * bitmap_width + x;
            const std::uint32_t mask_pixel = mask[offset];
            const std::uint32_t mask_blue = mask_pixel & 0xFFu;
            const std::uint32_t mask_green = (mask_pixel >> 8u) & 0xFFu;
            const std::uint32_t mask_red = (mask_pixel >> 16u) & 0xFFu;
            const std::uint32_t coverage =
                (mask_red + mask_green + mask_blue + 1u) / 3u;
            if (coverage == 0) continue;

            const std::uint32_t background = target[offset];
            const std::uint32_t background_blue = background & 0xFFu;
            const std::uint32_t background_green = (background >> 8u) & 0xFFu;
            const std::uint32_t background_red = (background >> 16u) & 0xFFu;
            const std::uint32_t background_alpha = (background >> 24u) & 0xFFu;
            const std::uint32_t inverse = 255u - coverage;
            const auto source_over = [coverage, inverse](
                                         std::uint32_t foreground,
                                         std::uint32_t background_channel) {
                return (foreground * coverage +
                        background_channel * inverse + 127u) / 255u;
            };

            const std::uint32_t out_blue = source_over(250u, background_blue);
            const std::uint32_t out_green = source_over(247u, background_green);
            const std::uint32_t out_red = source_over(245u, background_red);
            const std::uint32_t out_alpha =
                coverage + (background_alpha * inverse + 127u) / 255u;
            target[offset] =
                (out_alpha << 24u) |
                (out_red << 16u) |
                (out_green << 8u) |
                out_blue;
        }
    }
}

void fill_rounded_rect(std::uint32_t* pixels,
                       int bitmap_width,
                       int bitmap_height,
                       const RECT& rect,
                       int radius,
                       std::uint32_t bgra) {
    if (!pixels || bitmap_width <= 0 || bitmap_height <= 0) return;
    const int left = std::clamp(static_cast<int>(rect.left), 0, bitmap_width);
    const int right = std::clamp(static_cast<int>(rect.right), 0, bitmap_width);
    const int top = std::clamp(static_cast<int>(rect.top), 0, bitmap_height);
    const int bottom = std::clamp(static_cast<int>(rect.bottom), 0, bitmap_height);
    const int bounded_radius = std::max(0, std::min(
        radius, std::min((right - left) / 2, (bottom - top) / 2)));
    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            const int nearest_x = std::clamp(
                x, left + bounded_radius, right - bounded_radius - 1);
            const int nearest_y = std::clamp(
                y, top + bounded_radius, bottom - bounded_radius - 1);
            const int dx = x - nearest_x;
            const int dy = y - nearest_y;
            if (dx * dx + dy * dy <= bounded_radius * bounded_radius) {
                pixels[static_cast<std::size_t>(y) * bitmap_width + x] = bgra;
            }
        }
    }
}

std::wstring bundled_icon_path() {
    wchar_t buf[MAX_PATH] = {0};
    DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n == MAX_PATH) return {};
    std::filesystem::path cur = std::filesystem::path(std::wstring(buf, n)).parent_path();
    for (int i = 0; i < 7; ++i) {
        auto candidate = cur / "assets" / "windows" / "acecode.ico";
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec) && !ec) {
            return candidate.wstring();
        }
        if (!cur.has_parent_path()) break;
        cur = cur.parent_path();
    }
    return {};
}

} // namespace

struct SplashScreen::Impl {
    HWND hwnd = nullptr;
    HICON icon = nullptr;
    bool destroy_icon = false;
    POINT pos{};
    int icon_size = 192;
    int window_width = 520;
    int window_height = 250;
    std::wstring status_message;

    ~Impl() {
        close();
    }

    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        }
        auto* self = reinterpret_cast<Impl*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        switch (msg) {
            case WM_ERASEBKGND:
                return 1;
            case WM_NCHITTEST:
                return HTTRANSPARENT;
            default:
                break;
        }
        return ::DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    static bool register_class(HINSTANCE instance) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = Impl::wnd_proc;
        wc.hInstance = instance;
        wc.hCursor = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
        wc.lpszClassName = kSplashClassName;
        if (::RegisterClassW(&wc)) return true;
        return ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }

    void show() {
        if (hwnd) return;

        HINSTANCE instance = ::GetModuleHandleW(nullptr);
        if (!register_class(instance)) return;

        RECT rect = active_monitor_rect();
        icon_size = choose_icon_size(rect);
        load_icon(instance);
        if (!icon) return;

        const int monitor_w = std::max(1, static_cast<int>(rect.right - rect.left));
        const int monitor_h = std::max(1, static_cast<int>(rect.bottom - rect.top));
        window_width = std::min(
            std::max(icon_size + 96, 520),
            std::max(icon_size, monitor_w - 32));
        window_height = icon_size + 64;
        pos.x = rect.left + (monitor_w - window_width) / 2;
        pos.y = rect.top + (monitor_h - window_height) / 2;
        hwnd = ::CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            kSplashClassName,
            L"ACECode",
            WS_POPUP,
            pos.x,
            pos.y,
            window_width,
            window_height,
            nullptr,
            nullptr,
            instance,
            this);
        if (!hwnd) return;

        render_layered_icon();
        ::SetWindowPos(hwnd, HWND_TOPMOST, pos.x, pos.y, window_width, window_height,
                       SWP_NOACTIVATE | SWP_SHOWWINDOW);
        ::ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    }

    void set_status(const std::string& message, std::uint64_t) {
        status_message = utf8_to_wide(message);
        if (hwnd) render_layered_icon();
    }

    void close() {
        if (hwnd) {
            ::DestroyWindow(hwnd);
            hwnd = nullptr;
        }
        if (icon && destroy_icon) {
            ::DestroyIcon(icon);
        }
        icon = nullptr;
        destroy_icon = false;
    }

    void load_icon(HINSTANCE instance) {
        icon = static_cast<HICON>(::LoadImageW(
            instance,
            kAppIconResourceName,
            IMAGE_ICON,
            icon_size,
            icon_size,
            LR_DEFAULTCOLOR));
        destroy_icon = icon != nullptr;
        if (icon) return;

        icon = static_cast<HICON>(::LoadImageW(
            instance,
            MAKEINTRESOURCEW(kAppIconResourceId),
            IMAGE_ICON,
            icon_size,
            icon_size,
            LR_DEFAULTCOLOR));
        destroy_icon = icon != nullptr;
        if (icon) return;

        auto path = bundled_icon_path();
        if (!path.empty()) {
            icon = static_cast<HICON>(::LoadImageW(
                nullptr,
                path.c_str(),
                IMAGE_ICON,
                icon_size,
                icon_size,
                LR_LOADFROMFILE | LR_DEFAULTCOLOR));
            destroy_icon = icon != nullptr;
            if (icon) return;
        }

        icon = ::LoadIconW(instance, kAppIconResourceName);
        if (icon) return;

        icon = ::LoadIconW(instance, MAKEINTRESOURCEW(kAppIconResourceId));
        destroy_icon = false;
    }

    void render_layered_icon() {
        HDC screen = ::GetDC(nullptr);
        if (!screen) return;

        HDC mem = ::CreateCompatibleDC(screen);
        if (!mem) {
            ::ReleaseDC(nullptr, screen);
            return;
        }

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = window_width;
        bmi.bmiHeader.biHeight = -window_height;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HBITMAP bitmap = ::CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bitmap) {
            ::DeleteDC(mem);
            ::ReleaseDC(nullptr, screen);
            return;
        }

        std::memset(bits, 0,
                    static_cast<size_t>(window_width) *
                    static_cast<size_t>(window_height) * 4);
        HBITMAP old = static_cast<HBITMAP>(::SelectObject(mem, bitmap));
        const int icon_x = (window_width - icon_size) / 2;
        ::DrawIconEx(mem, icon_x, 0, icon, icon_size, icon_size,
                     0, nullptr, DI_NORMAL);

        const std::wstring& status = status_message;
        if (!status.empty()) {
            const int dpi = std::max(96, ::GetDeviceCaps(screen, LOGPIXELSY));
            HFONT font = ::CreateFontW(
                -::MulDiv(11, dpi, 72), 0, 0, 0, FW_NORMAL,
                FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                L"Segoe UI");
            HDC text_dc = ::CreateCompatibleDC(screen);
            void* mask_bits = nullptr;
            HBITMAP mask_bitmap = text_dc
                ? ::CreateDIBSection(
                      screen, &bmi, DIB_RGB_COLORS, &mask_bits, nullptr, 0)
                : nullptr;
            if (text_dc && mask_bitmap && mask_bits) {
                std::memset(mask_bits, 0,
                            static_cast<size_t>(window_width) *
                            static_cast<size_t>(window_height) * 4);
                HBITMAP old_mask_bitmap =
                    static_cast<HBITMAP>(::SelectObject(text_dc, mask_bitmap));
                HFONT old_font = font
                    ? static_cast<HFONT>(::SelectObject(text_dc, font))
                    : nullptr;
                ::SetBkMode(text_dc, TRANSPARENT);
                ::SetTextColor(text_dc, RGB(255, 255, 255));

                RECT measured{0, 0, window_width - 40, 40};
                ::DrawTextW(
                    text_dc, status.c_str(), static_cast<int>(status.size()),
                    &measured, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
                const int pill_width = std::min(
                    window_width - 20,
                    std::max(120,
                             static_cast<int>(measured.right - measured.left) + 32));
                const int pill_height = std::max(
                    34, static_cast<int>(measured.bottom - measured.top) + 16);
                RECT pill{
                    (window_width - pill_width) / 2,
                    icon_size + 8,
                    (window_width + pill_width) / 2,
                    std::min(window_height - 4, icon_size + 8 + pill_height),
                };
                fill_rounded_rect(
                    static_cast<std::uint32_t*>(bits), window_width, window_height,
                    pill, 12, premultiplied_bgra(27, 24, 24, 180));

                RECT text_rect{
                    pill.left + 16, pill.top, pill.right - 16, pill.bottom};
                ::DrawTextW(
                    text_dc, status.c_str(), static_cast<int>(status.size()),
                    &text_rect,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                        DT_NOPREFIX);
                composite_grayscale_text(
                    static_cast<std::uint32_t*>(bits),
                    static_cast<const std::uint32_t*>(mask_bits),
                    window_width, window_height, text_rect);

                if (old_font) ::SelectObject(text_dc, old_font);
                ::SelectObject(text_dc, old_mask_bitmap);
            }
            if (mask_bitmap) ::DeleteObject(mask_bitmap);
            if (text_dc) ::DeleteDC(text_dc);
            if (font) ::DeleteObject(font);
        }

        POINT src{0, 0};
        SIZE size{window_width, window_height};
        BLENDFUNCTION blend{};
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = 255;
        blend.AlphaFormat = AC_SRC_ALPHA;
        ::UpdateLayeredWindow(hwnd, screen, &pos, &size, mem, &src, 0, &blend, ULW_ALPHA);

        ::SelectObject(mem, old);
        ::DeleteObject(bitmap);
        ::DeleteDC(mem);
        ::ReleaseDC(nullptr, screen);
    }
};

#else

struct SplashScreen::Impl {
    void show() {}
    void set_status(const std::string&, std::uint64_t) {}
    void close() {}
};

#endif

SplashScreen::~SplashScreen() {
    close();
}

void SplashScreen::show() {
    if (!impl_) impl_ = new Impl();
    impl_->show();
}

void SplashScreen::set_status(const std::string& message,
                              std::uint64_t elapsed_ms) {
    if (!impl_) impl_ = new Impl();
    impl_->set_status(message, elapsed_ms);
}

void SplashScreen::close() {
    if (!impl_) return;
    impl_->close();
    delete impl_;
    impl_ = nullptr;
}

} // namespace acecode::desktop
