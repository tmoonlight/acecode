#include "custom_toast.hpp"

#ifdef _WIN32

#include "../utils/encoding.hpp"
#include "../utils/logger.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <mmsystem.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace acecode::desktop::custom_toast {
namespace {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

constexpr UINT kMsgShowToast = WM_APP + 0x21;
constexpr UINT kMsgQuitLoop = WM_APP + 0x22;

constexpr UINT_PTR kAnimationTimerId = 1;
constexpr UINT_PTR kDismissTimerId = 1;
constexpr UINT kAnimationIntervalMs = 15;

constexpr wchar_t kControllerClassName[] = L"ACECodeToastController";
constexpr wchar_t kToastClassName[] = L"ACECodeToastWindow";

// Layout in device-independent pixels (96 dpi).
constexpr int kCardWidthDip = 368;
constexpr int kPaddingDip = 16;
constexpr int kIconSizeDip = 32;
constexpr int kIconGapDip = 12;
constexpr int kCloseSizeDip = 24;
constexpr int kCloseInsetDip = 6;
constexpr int kAccentBarDip = 4;
constexpr int kCornerRadiusDip = 8;
constexpr int kTitleBodyGapDip = 4;
constexpr int kStackMarginDip = 12;
constexpr int kScreenMarginXDip = 16;
constexpr int kScreenMarginYDip = 16;
constexpr int kMaxBodyLines = 3;

// ---------------------------------------------------------------------------
// Small Win32 helpers
// ---------------------------------------------------------------------------

HMODULE current_module() {
    HMODULE module = nullptr;
    ::GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&current_module),
        &module);
    return module ? module : ::GetModuleHandleW(nullptr);
}

// The FTXUI terminal build never sets a process DPI awareness context, and the
// desktop shell sets one before its first HWND. Raising awareness for this
// thread only keeps the toast crisp in both without touching process state.
// Typed through HANDLE so the file still compiles against toolchains whose
// headers predate DPI_AWARENESS_CONTEXT (MinGW/WinLibs).
void enable_thread_dpi_awareness() {
    using SetThreadDpiAwarenessContextFn = HANDLE(WINAPI*)(HANDLE);
    HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
    if (!user32) return;
    auto setter = reinterpret_cast<SetThreadDpiAwarenessContextFn>(
        reinterpret_cast<void*>(
            ::GetProcAddress(user32, "SetThreadDpiAwarenessContext")));
    // -4 == DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    if (setter) setter(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)));
}

unsigned monitor_dpi(HMONITOR monitor) {
    using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    if (monitor) {
        HMODULE shcore = ::LoadLibraryW(L"shcore.dll");
        if (shcore) {
            auto getter = reinterpret_cast<GetDpiForMonitorFn>(
                reinterpret_cast<void*>(
                    ::GetProcAddress(shcore, "GetDpiForMonitor")));
            UINT dpi_x = 0;
            UINT dpi_y = 0;
            const bool ok = getter && getter(monitor, 0, &dpi_x, &dpi_y) == S_OK;
            ::FreeLibrary(shcore);
            if (ok && dpi_y != 0) return dpi_y;
        }
    }
    HDC screen = ::GetDC(nullptr);
    unsigned dpi = kBaseDpi;
    if (screen) {
        const int value = ::GetDeviceCaps(screen, LOGPIXELSY);
        if (value > 0) dpi = static_cast<unsigned>(value);
        ::ReleaseDC(nullptr, screen);
    }
    return dpi;
}

unsigned system_dpi() {
    HDC screen = ::GetDC(nullptr);
    unsigned dpi = kBaseDpi;
    if (screen) {
        const int value = ::GetDeviceCaps(screen, LOGPIXELSY);
        if (value > 0) dpi = static_cast<unsigned>(value);
        ::ReleaseDC(nullptr, screen);
    }
    return dpi;
}

DWORD read_dword_value(HKEY root, const wchar_t* subkey, const wchar_t* name,
                       DWORD fallback) {
    HKEY key = nullptr;
    if (::RegOpenKeyExW(root, subkey, 0, KEY_QUERY_VALUE, &key) !=
        ERROR_SUCCESS) {
        return fallback;
    }
    DWORD value = 0;
    DWORD type = 0;
    DWORD size = sizeof(value);
    const LONG status = ::RegQueryValueExW(
        key, name, nullptr, &type, reinterpret_cast<BYTE*>(&value), &size);
    ::RegCloseKey(key);
    if (status != ERROR_SUCCESS || type != REG_DWORD) return fallback;
    return value;
}

bool system_uses_dark_theme() {
    // 0 = dark apps. Missing value (Windows 10 pre-1803, Server) means light.
    return read_dword_value(
               HKEY_CURRENT_USER,
               L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Themes\\"
               L"Personalize",
               L"AppsUseLightTheme", 1) == 0;
}

COLORREF system_accent_color() {
    // Same lookup Electron uses: DWM's AccentColor is RGBA, the older
    // ColorizationColor is BGRA.
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows\\DWM",
                        0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
        DWORD value = 0;
        DWORD type = 0;
        DWORD size = sizeof(value);
        const bool accent =
            ::RegQueryValueExW(key, L"AccentColor", nullptr, &type,
                               reinterpret_cast<BYTE*>(&value),
                               &size) == ERROR_SUCCESS &&
            type == REG_DWORD;
        if (!accent) {
            size = sizeof(value);
            if (::RegQueryValueExW(key, L"ColorizationColor", nullptr, &type,
                                   reinterpret_cast<BYTE*>(&value),
                                   &size) == ERROR_SUCCESS &&
                type == REG_DWORD) {
                ::RegCloseKey(key);
                return RGB(GetBValue(value), GetGValue(value), GetRValue(value));
            }
        } else {
            ::RegCloseKey(key);
            return RGB(GetRValue(value), GetGValue(value), GetBValue(value));
        }
        ::RegCloseKey(key);
    }
    return RGB(0x40, 0x7d, 0xd0);
}

struct ToastPalette {
    COLORREF background = RGB(0x2b, 0x2b, 0x2b);
    COLORREF background_hot = RGB(0x36, 0x36, 0x36);
    COLORREF title = RGB(0xf2, 0xf2, 0xf2);
    COLORREF body = RGB(0xc4, 0xc4, 0xc4);
    COLORREF close = RGB(0x9a, 0x9a, 0x9a);
    COLORREF close_hot = RGB(0xff, 0xff, 0xff);
    COLORREF accent = RGB(0x40, 0x7d, 0xd0);
};

ToastPalette make_palette(bool dark, COLORREF accent) {
    ToastPalette palette;
    palette.accent = accent;
    if (dark) {
        palette.background = RGB(0x24, 0x24, 0x26);
        palette.background_hot = RGB(0x30, 0x30, 0x33);
        palette.title = RGB(0xf4, 0xf4, 0xf5);
        palette.body = RGB(0xbd, 0xbd, 0xc2);
        palette.close = RGB(0x8d, 0x8d, 0x94);
        palette.close_hot = RGB(0xff, 0xff, 0xff);
    } else {
        palette.background = RGB(0xfb, 0xfb, 0xfd);
        palette.background_hot = RGB(0xf0, 0xf0, 0xf4);
        palette.title = RGB(0x1b, 0x1b, 0x1f);
        palette.body = RGB(0x4a, 0x4a, 0x52);
        palette.close = RGB(0x6a, 0x6a, 0x74);
        palette.close_hot = RGB(0x11, 0x11, 0x14);
    }
    return palette;
}

// Coverage (0..255) of one pixel against a rounded rectangle. Only the corner
// boxes need supersampling; everything else is fully covered.
int rounded_corner_coverage(int x, int y, int width, int height, int radius) {
    if (radius <= 0) return 255;
    int center_x = 0;
    int center_y = 0;
    if (x < radius) {
        center_x = radius;
    } else if (x >= width - radius) {
        center_x = width - radius;
    } else {
        return 255;
    }
    if (y < radius) {
        center_y = radius;
    } else if (y >= height - radius) {
        center_y = height - radius;
    } else {
        return 255;
    }

    constexpr int kSamples = 4;
    const double radius_sq = static_cast<double>(radius) * radius;
    int hits = 0;
    for (int sy = 0; sy < kSamples; ++sy) {
        const double py = y + (sy + 0.5) / kSamples;
        for (int sx = 0; sx < kSamples; ++sx) {
            const double px = x + (sx + 0.5) / kSamples;
            const double dx = px - center_x;
            const double dy = py - center_y;
            if (dx * dx + dy * dy <= radius_sq) ++hits;
        }
    }
    return hits * 255 / (kSamples * kSamples);
}

HICON load_app_icon(int size) {
    // acecode.rc.in compiles acecode.ico as IDI_ICON1. Older builds emitted it
    // as a named resource, newer ones as ordinal 1, so try both.
    HINSTANCE instance = reinterpret_cast<HINSTANCE>(current_module());
    if (HICON icon = static_cast<HICON>(::LoadImageW(
            instance, L"IDI_ICON1", IMAGE_ICON, size, size, LR_DEFAULTCOLOR))) {
        return icon;
    }
    if (HICON icon = static_cast<HICON>(
            ::LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, size, size,
                         LR_DEFAULTCOLOR))) {
        return icon;
    }
    // LoadIconW returns a shared handle that must not be destroyed. Copy it so
    // ToastWindow can keep one simple ownership rule for every successful
    // branch above.
    if (HICON icon = ::LoadIconW(instance, L"IDI_ICON1")) {
        return ::CopyIcon(icon);
    }
    if (HICON icon = ::LoadIconW(instance, MAKEINTRESOURCEW(1))) {
        return ::CopyIcon(icon);
    }
    return nullptr;
}

class Controller;

// ---------------------------------------------------------------------------
// One toast card
// ---------------------------------------------------------------------------

class ToastWindow {
public:
    ToastWindow(Controller& controller, NotifyPayload payload)
        : controller_(controller), payload_(std::move(payload)) {
        title_ = acecode::utf8_to_wide(payload_.title);
        body_ = acecode::utf8_to_wide(payload_.body);
        HDC screen = ::GetDC(nullptr);
        hdc_ = ::CreateCompatibleDC(screen);
        if (screen) ::ReleaseDC(nullptr, screen);
    }

    ~ToastWindow() {
        if (hdc_) {
            if (previous_bitmap_) ::SelectObject(hdc_, previous_bitmap_);
            ::DeleteDC(hdc_);
        }
        if (bitmap_) ::DeleteObject(bitmap_);
        if (icon_) ::DestroyIcon(icon_);
    }

    ToastWindow(const ToastWindow&) = delete;
    ToastWindow& operator=(const ToastWindow&) = delete;

    static void register_class(HINSTANCE instance) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &ToastWindow::wnd_proc;
        wc.lpszClassName = kToastClassName;
        wc.cbWndExtra = sizeof(ToastWindow*);
        wc.hInstance = instance;
        // IDC_ARROW is an integer resource encoded through the TCHAR-neutral
        // macro. Its pointer value is character-width agnostic.
        wc.hCursor =
            ::LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
        ::RegisterClassExW(&wc);
    }

    static ToastWindow* from(HWND hwnd) {
        return reinterpret_cast<ToastWindow*>(::GetWindowLongPtrW(hwnd, 0));
    }

    bool create(HINSTANCE instance) {
        hwnd_ = ::CreateWindowExW(
            WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
            kToastClassName, nullptr, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
            instance, this);
        return hwnd_ != nullptr;
    }

    HWND hwnd() const { return hwnd_; }
    int height() const { return static_cast<int>(size_.cy); }
    bool highlighted() const { return highlighted_; }
    bool finished() const { return finished_; }
    bool animation_active() const {
        return ease_in_active_ || ease_out_active_ || stack_collapse_active();
    }
    int vertical_position() const { return vertical_target_; }

    void pop_up(int vertical_offset) {
        // Size the card before it joins the stack: the controller derives the
        // next slot from this toast's height, and a zero height would stack the
        // following toast straight on top of it for one frame.
        measure_and_resize();
        vertical_current_ = vertical_offset;
        vertical_target_ = vertical_offset;
        stack_collapse_pos_ = 1.0f;
        ease_in_start_ = ::GetTickCount();
        ease_in_active_ = true;
    }

    void set_vertical_position(int vertical_offset) {
        if (vertical_offset == vertical_target_) return;
        // Restart the collapse from wherever the previous one got to, so the
        // card never jumps when two toasts expire back to back.
        vertical_current_ += static_cast<int>(
            (vertical_target_ - vertical_current_) * stack_collapse_pos_);
        vertical_target_ = vertical_offset;
        stack_collapse_start_ = ::GetTickCount();
        stack_collapse_pos_ = 0.0f;
    }

    // Also drops the cached layout: fonts, DPI and the icon size all move when
    // the display configuration changes.
    void invalidate() {
        needs_redraw_ = true;
        layout_valid_ = false;
    }

    // Starts the fade-out. Further input is ignored but mouse messages keep
    // flowing so the stack does not collapse under the cursor.
    void dismiss() {
        if (interactive_) {
            interactive_ = false;
            ::KillTimer(hwnd_, kDismissTimerId);
            if (!ease_out_active_) {
                ease_out_start_ = ::GetTickCount();
                ease_out_active_ = true;
            }
        }
    }

    HDWP animate(HDWP hdwp, const ToastRect& work_area, int margin_x,
                 int margin_y);

private:
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT message, WPARAM wparam,
                                     LPARAM lparam);

    bool stack_collapse_active() const {
        return vertical_current_ != vertical_target_;
    }

    void measure_and_resize();
    void draw();
    void apply_rounded_alpha();
    void push_contents();
    void schedule_dismissal();
    void cancel_dismissal();
    void on_mouse_move(POINT cursor);
    void on_mouse_leave();
    void on_click();

    Controller& controller_;
    NotifyPayload payload_;
    std::wstring title_;
    std::wstring body_;

    HWND hwnd_ = nullptr;
    HDC hdc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ previous_bitmap_ = nullptr;
    void* bits_ = nullptr;
    HICON icon_ = nullptr;
    int icon_size_ = 0;

    SIZE size_ = {0, 0};
    RECT close_rect_ = {0, 0, 0, 0};
    bool needs_redraw_ = true;
    bool layout_valid_ = false;

    bool highlighted_ = false;
    bool close_hot_ = false;
    bool interactive_ = true;
    bool finished_ = false;

    bool ease_in_active_ = false;
    bool ease_out_active_ = false;
    DWORD ease_in_start_ = 0;
    DWORD ease_out_start_ = 0;
    DWORD stack_collapse_start_ = 0;
    float ease_in_pos_ = 0.0f;
    float ease_out_pos_ = 0.0f;
    float stack_collapse_pos_ = 1.0f;

    int vertical_current_ = 0;
    int vertical_target_ = 0;
};

// ---------------------------------------------------------------------------
// Hidden controller window: owns fonts, the animation timer and the stack
// ---------------------------------------------------------------------------

class Controller {
public:
    explicit Controller(InitOptions options) : options_(std::move(options)) {
        anchor_ = static_cast<HWND>(options_.activation_window);
    }

    ~Controller() { release_assets(); }

    static void register_class(HINSTANCE instance) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &Controller::wnd_proc;
        wc.lpszClassName = kControllerClassName;
        wc.cbWndExtra = sizeof(Controller*);
        wc.hInstance = instance;
        ::RegisterClassExW(&wc);
    }

    static Controller* from(HWND hwnd) {
        return reinterpret_cast<Controller*>(::GetWindowLongPtrW(hwnd, 0));
    }

    bool create(HINSTANCE instance) {
        instance_ = instance;
        // Not a message-only window on purpose: HWND_MESSAGE windows do not
        // receive the WM_SETTINGCHANGE / WM_DISPLAYCHANGE broadcasts the stack
        // needs in order to re-anchor itself.
        hwnd_ = ::CreateWindowExW(0, kControllerClassName, nullptr, 0, 0, 0, 0, 0,
                                  nullptr, nullptr, instance, this);
        return hwnd_ != nullptr;
    }

    HWND hwnd() const { return hwnd_; }
    HINSTANCE instance() const { return instance_; }
    const ToastPalette& palette() const { return palette_; }
    bool sound_enabled() const { return options_.play_sound; }

    unsigned dpi() {
        ensure_assets();
        return dpi_;
    }

    int dip(int value) { return scale_for_dpi(value, dpi()); }

    HFONT title_font() {
        ensure_assets();
        return title_font_;
    }

    HFONT body_font() {
        ensure_assets();
        return body_font_;
    }

    void start_animation() {
        if (animating_ || !hwnd_) return;
        // 15 ms rather than 16: the timer is coarse, so ask for more than 60 Hz
        // to actually land near it.
        ::SetTimer(hwnd_, kAnimationTimerId, kAnimationIntervalMs, nullptr);
        animating_ = true;
    }

    void enqueue(NotifyPayload payload) {
        if (queue_.size() >= kMaxQueuedToasts) {
            LOG_WARN("[notifications] custom toast queue is full, dropping " +
                     payload.id);
            return;
        }
        queue_.push_back(std::move(payload));
        drain_queue();
    }

    void animate_all();
    void destroy_all();
    void on_display_change();

private:
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT message, WPARAM wparam,
                                     LPARAM lparam);

    void ensure_assets();
    void release_assets();
    void drain_queue();
    ToastRect work_area() const;

    InitOptions options_;
    HWND hwnd_ = nullptr;
    HWND anchor_ = nullptr;
    HINSTANCE instance_ = nullptr;
    bool animating_ = false;

    bool assets_valid_ = false;
    unsigned dpi_ = kBaseDpi;
    HFONT title_font_ = nullptr;
    HFONT body_font_ = nullptr;
    bool stock_title_font_ = false;
    bool stock_body_font_ = false;
    ToastPalette palette_;

    std::vector<std::unique_ptr<ToastWindow>> toasts_;
    std::vector<NotifyPayload> queue_;
};

// ---------------------------------------------------------------------------
// ToastWindow implementation
// ---------------------------------------------------------------------------

LRESULT CALLBACK ToastWindow::wnd_proc(HWND hwnd, UINT message, WPARAM wparam,
                                       LPARAM lparam) {
    switch (message) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            ::SetWindowLongPtrW(hwnd, 0,
                                reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return 0;
        }
        case WM_NCDESTROY:
            // The controller owns the object; only drop the back pointer here.
            ::SetWindowLongPtrW(hwnd, 0, 0);
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_TIMER:
            if (wparam == kDismissTimerId) {
                if (auto* toast = from(hwnd)) toast->dismiss();
                return 0;
            }
            break;
        case WM_MOUSEMOVE:
            if (auto* toast = from(hwnd)) {
                POINT cursor = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                toast->on_mouse_move(cursor);
            }
            return 0;
        case WM_MOUSELEAVE:
            if (auto* toast = from(hwnd)) toast->on_mouse_leave();
            return 0;
        case WM_LBUTTONDOWN:
            if (auto* toast = from(hwnd)) toast->on_click();
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(hwnd, message, wparam, lparam);
}

void ToastWindow::on_mouse_move(POINT cursor) {
    bool changed = false;
    if (!highlighted_) {
        highlighted_ = true;
        changed = true;
        TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd_, 0};
        ::TrackMouseEvent(&tme);
    }
    const bool close_hot = ::PtInRect(&close_rect_, cursor) != FALSE;
    if (close_hot != close_hot_) {
        close_hot_ = close_hot;
        changed = true;
    }
    // Hovering holds the toast open; the countdown restarts on mouse leave.
    if (interactive_) cancel_dismissal();
    if (changed) {
        // Hover only repaints — the measured layout is unaffected.
        draw();
        push_contents();
    }
}

void ToastWindow::on_mouse_leave() {
    highlighted_ = false;
    close_hot_ = false;
    draw();
    push_contents();
    if (interactive_ && !ease_in_active_) schedule_dismissal();
    controller_.start_animation();
}

void ToastWindow::on_click() {
    if (!interactive_) return;
    const bool close_clicked = close_hot_;
    NotifyPayload payload = payload_;
    dismiss();
    controller_.start_animation();
    if (!close_clicked) {
        // May run arbitrary application code. It never touches this object.
        dispatch_notification_activation(payload);
    }
}

void ToastWindow::schedule_dismissal() {
    ULONG duration = 0;
    if (!::SystemParametersInfoW(SPI_GETMESSAGEDURATION, 0, &duration, 0)) {
        duration = 0;
    }
    const unsigned seconds =
        clamp_auto_dismiss_seconds(static_cast<unsigned>(duration));
    ::SetTimer(hwnd_, kDismissTimerId, seconds * 1000, nullptr);
}

void ToastWindow::cancel_dismissal() {
    ::KillTimer(hwnd_, kDismissTimerId);
}

void ToastWindow::measure_and_resize() {
    if (layout_valid_ || !hdc_) return;

    const int padding = controller_.dip(kPaddingDip);
    const int bar = controller_.dip(kAccentBarDip);
    const int icon_size = controller_.dip(kIconSizeDip);
    const int icon_gap = controller_.dip(kIconGapDip);
    const int close_size = controller_.dip(kCloseSizeDip);
    const int close_inset = controller_.dip(kCloseInsetDip);
    const int title_gap = controller_.dip(kTitleBodyGapDip);
    const int width = controller_.dip(kCardWidthDip);

    const int text_left = bar + padding + icon_size + icon_gap;
    const int text_right = width - padding;

    TEXTMETRICW title_metrics = {};
    HGDIOBJ previous_font = ::SelectObject(hdc_, controller_.title_font());
    ::GetTextMetricsW(hdc_, &title_metrics);

    TEXTMETRICW body_metrics = {};
    ::SelectObject(hdc_, controller_.body_font());
    ::GetTextMetricsW(hdc_, &body_metrics);

    const int body_line_height =
        std::max(1, static_cast<int>(body_metrics.tmHeight));
    int body_height = 0;
    if (!body_.empty()) {
        RECT measure = {0, 0, std::max(1, text_right - text_left), 0};
        ::DrawTextW(hdc_, body_.c_str(), static_cast<int>(body_.size()),
                    &measure,
                    DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
        const int lines = fit_body_lines(
            static_cast<int>(measure.bottom - measure.top), body_line_height,
            kMaxBodyLines);
        body_height = std::max(lines, 1) * body_line_height;
    }
    if (previous_font) ::SelectObject(hdc_, previous_font);

    int height = padding + static_cast<int>(title_metrics.tmHeight) + padding;
    if (body_height > 0) height += title_gap + body_height;
    height = std::max(height, padding * 2 + icon_size);

    close_rect_.right = width - close_inset;
    close_rect_.left = close_rect_.right - close_size;
    close_rect_.top = close_inset;
    close_rect_.bottom = close_rect_.top + close_size;

    if (width != size_.cx || height != size_.cy) {
        BITMAPINFO info = {};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = width;
        // Negative height keeps the DIB top-down so the alpha fix-up below can
        // walk rows in the obvious order.
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HBITMAP bitmap = ::CreateDIBSection(nullptr, &info, DIB_RGB_COLORS,
                                            &bits, nullptr, 0);
        if (!bitmap) return;
        HGDIOBJ previous = ::SelectObject(hdc_, bitmap);
        if (!previous) {
            ::DeleteObject(bitmap);
            return;
        }
        if (!previous_bitmap_) previous_bitmap_ = previous;
        if (bitmap_) ::DeleteObject(bitmap_);
        bitmap_ = bitmap;
        bits_ = bits;
        size_.cx = width;
        size_.cy = height;
        needs_redraw_ = true;
    }

    if (!icon_ || icon_size != icon_size_) {
        if (icon_) ::DestroyIcon(icon_);
        icon_ = load_app_icon(icon_size);
        icon_size_ = icon_size;
    }
    layout_valid_ = true;
}

void ToastWindow::draw() {
    if (!hdc_ || !bits_ || size_.cx <= 0 || size_.cy <= 0) return;

    const ToastPalette& palette = controller_.palette();
    const int padding = controller_.dip(kPaddingDip);
    const int bar = controller_.dip(kAccentBarDip);
    const int icon_size = controller_.dip(kIconSizeDip);
    const int icon_gap = controller_.dip(kIconGapDip);
    const int title_gap = controller_.dip(kTitleBodyGapDip);
    const int text_left = bar + padding + icon_size + icon_gap;
    const int text_right = static_cast<int>(size_.cx) - padding;

    RECT card = {0, 0, size_.cx, size_.cy};
    if (HBRUSH brush = ::CreateSolidBrush(
            highlighted_ ? palette.background_hot : palette.background)) {
        ::FillRect(hdc_, &card, brush);
        ::DeleteObject(brush);
    }
    RECT accent_bar = {0, 0, bar, size_.cy};
    if (HBRUSH brush = ::CreateSolidBrush(palette.accent)) {
        ::FillRect(hdc_, &accent_bar, brush);
        ::DeleteObject(brush);
    }

    if (icon_) {
        ::DrawIconEx(hdc_, bar + padding, padding, icon_, icon_size, icon_size,
                     0, nullptr, DI_NORMAL);
    }

    ::SetBkMode(hdc_, TRANSPARENT);

    TEXTMETRICW title_metrics = {};
    HGDIOBJ previous_font = ::SelectObject(hdc_, controller_.title_font());
    ::GetTextMetricsW(hdc_, &title_metrics);
    ::SetTextColor(hdc_, palette.title);
    RECT title_rect = {text_left, padding, close_rect_.left - padding / 2,
                       padding + title_metrics.tmHeight};
    if (!title_.empty()) {
        ::DrawTextW(hdc_, title_.c_str(), static_cast<int>(title_.size()),
                    &title_rect,
                    DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }

    if (!body_.empty()) {
        ::SelectObject(hdc_, controller_.body_font());
        ::SetTextColor(hdc_, palette.body);
        RECT body_rect = {text_left, title_rect.bottom + title_gap, text_right,
                          size_.cy - padding};
        ::DrawTextW(hdc_, body_.c_str(), static_cast<int>(body_.size()),
                    &body_rect,
                    DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS |
                        DT_EDITCONTROL);
    }

    const wchar_t close_glyph = L'\x2715';
    ::SelectObject(hdc_, controller_.body_font());
    ::SetTextColor(hdc_, close_hot_ ? palette.close_hot : palette.close);
    RECT close_rect = close_rect_;
    ::DrawTextW(hdc_, &close_glyph, 1, &close_rect,
                DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
    if (previous_font) ::SelectObject(hdc_, previous_font);

    apply_rounded_alpha();
    needs_redraw_ = false;
}

void ToastWindow::apply_rounded_alpha() {
    if (!bits_) return;
    // GDI drawing leaves the alpha byte undefined, so the layered window would
    // composite garbage. Rewrite the whole alpha plane here: opaque inside the
    // rounded rectangle, antialiased across the corner arcs, and premultiplied
    // where coverage is partial because UpdateLayeredWindow expects that.
    ::GdiFlush();
    const int width = static_cast<int>(size_.cx);
    const int height = static_cast<int>(size_.cy);
    const int radius =
        std::min({controller_.dip(kCornerRadiusDip), width / 2, height / 2});
    auto* pixels = static_cast<BYTE*>(bits_);
    for (int y = 0; y < height; ++y) {
        BYTE* row = pixels + static_cast<size_t>(y) * width * 4;
        for (int x = 0; x < width; ++x) {
            BYTE* pixel = row + static_cast<size_t>(x) * 4;
            const int coverage =
                rounded_corner_coverage(x, y, width, height, radius);
            if (coverage >= 255) {
                pixel[3] = 255;
                continue;
            }
            pixel[0] = static_cast<BYTE>(pixel[0] * coverage / 255);
            pixel[1] = static_cast<BYTE>(pixel[1] * coverage / 255);
            pixel[2] = static_cast<BYTE>(pixel[2] * coverage / 255);
            pixel[3] = static_cast<BYTE>(coverage);
        }
    }
}

void ToastWindow::push_contents() {
    if (!hwnd_ || !::IsWindowVisible(hwnd_)) return;
    RECT rect = {};
    if (!::GetWindowRect(hwnd_, &rect)) return;
    POINT source = {size_.cx - (rect.right - rect.left), 0L};
    if (source.x < 0) source.x = 0;
    SIZE size = {rect.right - rect.left, rect.bottom - rect.top};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0,
                           static_cast<BYTE>(std::lround(
                               255.0 * (1.0 - ease_out_pos_))),
                           AC_SRC_ALPHA};
    ::UpdateLayeredWindow(hwnd_, nullptr, nullptr, &size, hdc_, &source, 0,
                          &blend, ULW_ALPHA);
}

HDWP ToastWindow::animate(HDWP hdwp, const ToastRect& work_area, int margin_x,
                          int margin_y) {
    measure_and_resize();
    if (needs_redraw_) draw();
    if (!bitmap_) return hdwp;

    const DWORD now = ::GetTickCount();

    if (ease_in_active_) {
        ease_in_pos_ = ease_in_position(now - ease_in_start_, kEaseInDurationMs);
        if (ease_in_pos_ >= 1.0f) {
            ease_in_active_ = false;
            if (interactive_ && !highlighted_) schedule_dismissal();
        }
    }
    if (ease_out_active_) {
        ease_out_pos_ =
            ease_out_position(now - ease_out_start_, kEaseOutDurationMs);
        if (ease_out_pos_ >= 1.0f) ease_out_active_ = false;
    }
    if (stack_collapse_active()) {
        stack_collapse_pos_ = stack_collapse_position(now - stack_collapse_start_,
                                                      kStackCollapseDurationMs);
    }

    const int collapse_offset = static_cast<int>(
        (vertical_target_ - vertical_current_) * stack_collapse_pos_);
    const int vertical_offset = vertical_current_ + collapse_offset;
    if (stack_collapse_pos_ >= 1.0f) vertical_current_ = vertical_target_;

    const ToastPlacement placement =
        compute_toast_placement(work_area, margin_x, margin_y,
                                static_cast<int>(size_.cx),
                                static_cast<int>(size_.cy), vertical_offset,
                                ease_in_pos_);

    UINT flags = SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOREDRAW | SWP_NOCOPYBITS;
    const bool fading_done = !ease_out_active_ && ease_out_pos_ >= 1.0f;
    if (fading_done) {
        flags &= ~static_cast<UINT>(SWP_SHOWWINDOW);
        flags |= SWP_HIDEWINDOW;
        finished_ = true;
        highlighted_ = false;
    }

    // Reveal the card from its right edge: the destination width grows while
    // the source origin walks back toward zero, so the card looks like it
    // slides out of the screen border instead of scaling in place.
    POINT source = {size_.cx - placement.width, 0};
    if (source.x < 0) source.x = 0;
    POINT destination = {placement.x, placement.y};
    SIZE size = {placement.width, placement.height};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0,
                           static_cast<BYTE>(std::lround(
                               255.0 * (1.0 - ease_out_pos_))),
                           AC_SRC_ALPHA};

    UPDATELAYEREDWINDOWINFO update = {};
    update.cbSize = sizeof(update);
    update.hdcDst = nullptr;
    update.pptDst = &destination;
    update.psize = &size;
    update.hdcSrc = hdc_;
    update.pptSrc = &source;
    update.crKey = 0;
    update.pblend = &blend;
    update.dwFlags = ULW_ALPHA;
    update.prcDirty = nullptr;
    // Fails while the width is still zero at the start of the ease-in; the
    // DeferWindowPos below is what actually places the window in that frame.
    ::UpdateLayeredWindowIndirect(hwnd_, &update);

    return ::DeferWindowPos(hdwp, hwnd_, HWND_TOPMOST, destination.x,
                            destination.y, size.cx, size.cy, flags);
}

// ---------------------------------------------------------------------------
// Controller implementation
// ---------------------------------------------------------------------------

LRESULT CALLBACK Controller::wnd_proc(HWND hwnd, UINT message, WPARAM wparam,
                                      LPARAM lparam) {
    switch (message) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            ::SetWindowLongPtrW(hwnd, 0,
                                reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return 0;
        }
        case WM_TIMER:
            if (wparam == kAnimationTimerId) {
                if (auto* controller = from(hwnd)) controller->animate_all();
                return 0;
            }
            break;
        case WM_DISPLAYCHANGE:
            if (auto* controller = from(hwnd)) controller->on_display_change();
            break;
        case WM_SETTINGCHANGE:
            if (wparam == SPI_SETWORKAREA || wparam == SPI_SETNONCLIENTMETRICS) {
                if (auto* controller = from(hwnd)) controller->on_display_change();
            }
            break;
        case kMsgShowToast: {
            std::unique_ptr<NotifyPayload> payload(
                reinterpret_cast<NotifyPayload*>(lparam));
            if (auto* controller = from(hwnd)) {
                if (payload) controller->enqueue(std::move(*payload));
            }
            return 0;
        }
        case kMsgQuitLoop:
            if (auto* controller = from(hwnd)) controller->destroy_all();
            ::PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(hwnd, message, wparam, lparam);
}

void Controller::ensure_assets() {
    if (assets_valid_) return;
    assets_valid_ = true;

    HMONITOR monitor = nullptr;
    if (anchor_ && ::IsWindow(anchor_)) {
        monitor = ::MonitorFromWindow(anchor_, MONITOR_DEFAULTTONEAREST);
    }
    if (!monitor) {
        POINT origin = {0, 0};
        monitor = ::MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    }
    dpi_ = monitor_dpi(monitor);
    palette_ = make_palette(system_uses_dark_theme(), system_accent_color());

    if (body_font_ && !stock_body_font_) ::DeleteObject(body_font_);
    if (title_font_ && !stock_title_font_) ::DeleteObject(title_font_);
    body_font_ = nullptr;
    title_font_ = nullptr;
    stock_body_font_ = false;
    stock_title_font_ = false;

    NONCLIENTMETRICSW metrics = {};
    metrics.cbSize = sizeof(metrics);
    if (::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics),
                                &metrics, 0)) {
        const unsigned measured = system_dpi();
        const unsigned base = measured == 0 ? kBaseDpi : measured;
        LOGFONTW body = metrics.lfMessageFont;
        body.lfHeight = ::MulDiv(body.lfHeight, static_cast<int>(dpi_),
                                 static_cast<int>(base));
        LOGFONTW title = body;
        title.lfHeight = ::MulDiv(title.lfHeight, 108, 100);
        title.lfWeight = FW_SEMIBOLD;
        body_font_ = ::CreateFontIndirectW(&body);
        title_font_ = ::CreateFontIndirectW(&title);
    }

    // A missing metrics call or a failed font creation must not leave the DC
    // without a font; text would then render at an unpredictable size.
    HFONT stock = static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
    if (!body_font_) body_font_ = stock;
    if (!title_font_) title_font_ = stock;
    stock_body_font_ = body_font_ == stock;
    stock_title_font_ = title_font_ == stock;
}

void Controller::release_assets() {
    // Stock objects are owned by GDI and must never be deleted.
    if (body_font_ && !stock_body_font_) ::DeleteObject(body_font_);
    if (title_font_ && !stock_title_font_) ::DeleteObject(title_font_);
    body_font_ = nullptr;
    title_font_ = nullptr;
    stock_body_font_ = false;
    stock_title_font_ = false;
    assets_valid_ = false;
}

void Controller::on_display_change() {
    release_assets();
    for (auto& toast : toasts_) toast->invalidate();
    start_animation();
}

ToastRect Controller::work_area() const {
    HMONITOR monitor = nullptr;
    if (anchor_ && ::IsWindow(anchor_)) {
        monitor = ::MonitorFromWindow(anchor_, MONITOR_DEFAULTTONEAREST);
    }
    if (!monitor) {
        POINT origin = {0, 0};
        monitor = ::MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    }
    // LONG -> int would be a narrowing conversion inside a braced initializer,
    // which MSVC rejects outright; spell the casts out.
    auto to_toast_rect = [](const RECT& rect) {
        return ToastRect{static_cast<int>(rect.left), static_cast<int>(rect.top),
                         static_cast<int>(rect.right),
                         static_cast<int>(rect.bottom)};
    };

    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    if (monitor && ::GetMonitorInfoW(monitor, &info)) {
        return to_toast_rect(info.rcWork);
    }
    RECT rect = {};
    if (::SystemParametersInfoW(SPI_GETWORKAREA, 0, &rect, 0)) {
        return to_toast_rect(rect);
    }
    return {0, 0, ::GetSystemMetrics(SM_CXSCREEN),
            ::GetSystemMetrics(SM_CYSCREEN)};
}

void Controller::drain_queue() {
    while (toasts_.size() < kMaxVisibleToasts && !queue_.empty()) {
        NotifyPayload payload = std::move(queue_.front());
        queue_.erase(queue_.begin());

        auto toast = std::make_unique<ToastWindow>(*this, std::move(payload));
        if (!toast->create(instance_)) {
            LOG_WARN("[notifications] custom toast window creation failed");
            continue;
        }

        int offset = 0;
        if (!toasts_.empty()) {
            ToastWindow& last = *toasts_.back();
            offset = last.vertical_position() + last.height() +
                     dip(kStackMarginDip);
        }
        toast->pop_up(offset);
        toasts_.push_back(std::move(toast));

        if (options_.play_sound) {
            ::PlaySoundW(L"Notification.Default", nullptr,
                         SND_ALIAS | SND_ASYNC | SND_NODEFAULT);
        }
        start_animation();
    }
}

void Controller::animate_all() {
    const ToastRect area = work_area();
    const int margin_x = dip(kScreenMarginXDip);
    const int margin_y = dip(kScreenMarginYDip);

    bool keep_animating = false;
    if (!toasts_.empty()) {
        HDWP hdwp = ::BeginDeferWindowPos(static_cast<int>(toasts_.size()));
        for (auto& toast : toasts_) {
            hdwp = toast->animate(hdwp, area, margin_x, margin_y);
            keep_animating |= toast->animation_active();
            if (!hdwp) break;
        }
        if (hdwp) ::EndDeferWindowPos(hdwp);
    }

    if (!keep_animating && hwnd_) {
        ::KillTimer(hwnd_, kAnimationTimerId);
        animating_ = false;
    }

    // While the cursor rests on a toast the stack stays frozen: reclaiming the
    // slot of an expired neighbour would slide a different card under the
    // pointer and turn a click into the wrong action.
    const bool frozen = std::any_of(
        toasts_.begin(), toasts_.end(),
        [](const std::unique_ptr<ToastWindow>& toast) {
            return toast->highlighted();
        });
    if (!frozen) {
        for (auto it = toasts_.begin(); it != toasts_.end();) {
            if ((*it)->finished()) {
                ::DestroyWindow((*it)->hwnd());
                it = toasts_.erase(it);
            } else {
                ++it;
            }
        }
        int offset = 0;
        for (auto& toast : toasts_) {
            toast->set_vertical_position(offset);
            offset += toast->height() + dip(kStackMarginDip);
            if (toast->animation_active()) keep_animating = true;
        }
        if (keep_animating) start_animation();
    }

    drain_queue();
}

void Controller::destroy_all() {
    for (auto& toast : toasts_) {
        if (toast->hwnd()) ::DestroyWindow(toast->hwnd());
    }
    toasts_.clear();
    queue_.clear();
    if (hwnd_ && animating_) {
        ::KillTimer(hwnd_, kAnimationTimerId);
        animating_ = false;
    }
}

// ---------------------------------------------------------------------------
// Render thread + public surface
// ---------------------------------------------------------------------------

std::mutex g_mutex;
bool g_running = false;
HWND g_controller_hwnd = nullptr;
std::thread g_thread;

void render_thread_main(InitOptions options, std::promise<HWND> ready) {
    enable_thread_dpi_awareness();

    HINSTANCE instance = reinterpret_cast<HINSTANCE>(current_module());
    if (!instance) {
        ready.set_value(nullptr);
        return;
    }
    Controller::register_class(instance);
    ToastWindow::register_class(instance);

    Controller controller(std::move(options));
    if (!controller.create(instance)) {
        LOG_WARN("[notifications] custom toast controller window failed");
        ready.set_value(nullptr);
        return;
    }
    ready.set_value(controller.hwnd());

    MSG message;
    while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }
    controller.destroy_all();
}

} // namespace

bool initialize(const InitOptions& options) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_running) return true;

    std::promise<HWND> ready;
    std::future<HWND> future = ready.get_future();
    std::thread worker(render_thread_main, options, std::move(ready));

    HWND hwnd = nullptr;
    if (future.wait_for(std::chrono::seconds(5)) == std::future_status::ready) {
        hwnd = future.get();
    }
    if (!hwnd) {
        // The thread either failed early or is wedged. A failed start is
        // non-fatal, so let it go rather than blocking startup on a join.
        worker.detach();
        LOG_WARN("[notifications] self-drawn toast renderer failed to start");
        return false;
    }

    g_controller_hwnd = hwnd;
    g_thread = std::move(worker);
    g_running = true;
    LOG_INFO("[notifications] self-drawn toast renderer started");
    return true;
}

bool is_available() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_running;
}

bool show(const NotifyPayload& payload) {
    auto copy = std::make_unique<NotifyPayload>(payload);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_running) return false;

    // Serialize this post with shutdown's quit post. Once shutdown marks the
    // renderer stopped, no heap-owned payload can be queued behind WM_QUIT and
    // left unclaimed in the thread message queue.
    if (!::PostMessageW(g_controller_hwnd, kMsgShowToast, 0,
                        reinterpret_cast<LPARAM>(copy.get()))) {
        return false;
    }
    copy.release();
    return true;
}

void shutdown() {
    std::thread worker;
    HWND hwnd = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_running) return;
        g_running = false;
        hwnd = g_controller_hwnd;
        g_controller_hwnd = nullptr;
        worker = std::move(g_thread);
    }
    if (hwnd) ::PostMessageW(hwnd, kMsgQuitLoop, 0, 0);
    if (!worker.joinable()) return;
    // A click handler runs on the render thread and may tear notifications
    // down from there; joining ourselves would deadlock.
    if (worker.get_id() == std::this_thread::get_id()) {
        worker.detach();
        return;
    }
    worker.join();
}

} // namespace acecode::desktop::custom_toast

#endif // _WIN32
