#include "splash_screen.hpp"

#include <utility>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <dwmapi.h>

#  include "splash_animation.hpp"
#  include "splash_icon_painter_win.hpp"
#  include "utils/joining_thread.hpp"

#  include <algorithm>
#  include <atomic>
#  include <chrono>
#  include <cmath>
#  include <cstring>
#  include <cstdint>
#  include <mutex>
#  include <optional>
#  include <string>
#  include <vector>
#elif !defined(__APPLE__)
#  include "application_icon.hpp"
#  include "linux_desktop.hpp"
#  include <gtk/gtk.h>
#  include <algorithm>
#endif

namespace acecode::desktop {

#ifdef _WIN32

namespace {

constexpr wchar_t kSplashClassName[] = L"ACECodeSplashWindow";

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

double elapsed_ms(std::chrono::steady_clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

} // namespace

// 启动浮窗。启动期间主线程要同步等配置、工作区和后台服务,不能在主线程上做动画,
// 所以窗口由一条绘制线程创建并按屏幕刷新节奏逐帧重画(时间轴见 splash_animation.hpp)。
struct SplashScreen::Impl {
    // 主线程与绘制线程共享,受 mu 保护。
    std::mutex mu;
    std::wstring status_message;
    std::uint64_t status_revision = 0;
    bool thread_running = false;
    bool finish_requested = false;
    bool reveal_done = false;
    std::function<void()> on_reveal;
    // 回调执行期间 close() 会等它结束:close() 返回后回调不会再碰主窗口。
    std::mutex reveal_mu;
    std::atomic<bool> stop_requested{false};

    // 以下只在绘制线程上访问。
    HWND hwnd = nullptr;
    POINT pos{};
    int icon_size = 192;
    int icon_top = 12;
    int window_width = 520;
    int window_height = 268;
    HDC surface_dc = nullptr;
    HBITMAP surface_bitmap = nullptr;
    HGDIOBJ surface_previous = nullptr;
    std::uint32_t* surface_bits = nullptr;
    std::vector<std::uint32_t> status_layer;
    RECT status_rect{};

    // 最后声明:析构时最先停下并等绘制线程退出。
    JoiningThread worker;

    ~Impl() {
        close();
    }

    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
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
        {
            std::lock_guard<std::mutex> lock(mu);
            if (thread_running || worker.joinable()) return;
            thread_running = true;
        }
        worker = JoiningThread(&Impl::run, this);
    }

    void set_status(const std::string& message, std::uint64_t) {
        std::lock_guard<std::mutex> lock(mu);
        status_message = utf8_to_wide(message);
        ++status_revision;
    }

    void finish(std::function<void()> callback) {
        {
            std::lock_guard<std::mutex> lock(mu);
            if (reveal_done) return;
            if (thread_running) {
                finish_requested = true;
                on_reveal = std::move(callback);
                return;
            }
            reveal_done = true;
        }
        if (callback) callback();
    }

    void close() {
        stop_requested.store(true);
        {
            std::lock_guard<std::mutex> lock(mu);
            reveal_done = true;
            on_reveal = nullptr;
        }
        { std::lock_guard<std::mutex> wait_for_callback(reveal_mu); }
        worker.request_stop();
        worker.join();
    }

    // ── 绘制线程 ──

    void run() {
        {
            SplashGraphicsRuntime graphics;
            if (graphics.ok() && create_window()) animate();
            destroy_window();
        }
        std::function<void()> pending;
        {
            std::lock_guard<std::mutex> lock(mu);
            thread_running = false;
            // 窗口没建成时,首屏一就绪就直接交出主窗口。
            if (finish_requested && !reveal_done) {
                reveal_done = true;
                pending = std::move(on_reveal);
            }
        }
        invoke_reveal(std::move(pending));
    }

    void reveal() {
        std::function<void()> callback;
        {
            std::lock_guard<std::mutex> lock(mu);
            if (reveal_done) return;
            reveal_done = true;
            callback = std::move(on_reveal);
        }
        invoke_reveal(std::move(callback));
    }

    void invoke_reveal(std::function<void()> callback) {
        if (!callback) return;
        std::lock_guard<std::mutex> lock(reveal_mu);
        if (!stop_requested.load()) callback();
    }

    bool create_window() {
        HINSTANCE instance = ::GetModuleHandleW(nullptr);
        if (!register_class(instance)) return false;

        const RECT rect = active_monitor_rect();
        icon_size = choose_icon_size(rect);
        // 图标上方留一点空,方砖绽开的过冲和落地震动不会被窗口边缘切掉。
        icon_top = std::max(8, icon_size / 16);
        const int monitor_w = std::max(1, static_cast<int>(rect.right - rect.left));
        const int monitor_h = std::max(1, static_cast<int>(rect.bottom - rect.top));
        window_width = std::min(
            std::max(icon_size + 96, 520),
            std::max(icon_size, monitor_w - 32));
        window_height = icon_top + icon_size + 64;
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
            nullptr);
        if (!hwnd) return false;

        HDC screen = ::GetDC(nullptr);
        if (!screen) return false;
        surface_dc = ::CreateCompatibleDC(screen);
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = window_width;
        bmi.bmiHeader.biHeight = -window_height;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        surface_bitmap = surface_dc
            ? ::CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0)
            : nullptr;
        ::ReleaseDC(nullptr, screen);
        if (!surface_bitmap || !bits) return false;
        surface_previous = ::SelectObject(surface_dc, surface_bitmap);
        surface_bits = static_cast<std::uint32_t*>(bits);
        status_layer.assign(
            static_cast<std::size_t>(window_width) * static_cast<std::size_t>(window_height), 0);
        return true;
    }

    void destroy_window() {
        if (hwnd) {
            ::DestroyWindow(hwnd);
            hwnd = nullptr;
        }
        if (surface_dc && surface_previous) ::SelectObject(surface_dc, surface_previous);
        if (surface_bitmap) ::DeleteObject(surface_bitmap);
        if (surface_dc) ::DeleteDC(surface_dc);
        surface_dc = nullptr;
        surface_bitmap = nullptr;
        surface_previous = nullptr;
        surface_bits = nullptr;
    }

    void animate() {
        using Clock = std::chrono::steady_clock;
        const auto started = Clock::now();
        std::optional<Clock::time_point> finish_started;
        std::optional<std::uint64_t> drawn_revision;
        bool shown = false;
        while (!stop_requested.load()) {
            pump_messages();
            bool finishing = false;
            std::optional<std::wstring> status;
            {
                std::lock_guard<std::mutex> lock(mu);
                finishing = finish_requested;
                if (drawn_revision != status_revision) {
                    status = status_message;
                    drawn_revision = status_revision;
                }
            }
            if (status) render_status_layer(*status);

            const auto now = Clock::now();
            if (finishing && !finish_started) finish_started = now;
            const auto frame = splash_anim::frame_at(
                elapsed_ms(now - started),
                finish_started ? std::optional<double>(elapsed_ms(now - *finish_started))
                               : std::nullopt);
            present(frame);
            if (!shown) {
                ::SetWindowPos(hwnd, HWND_TOPMOST, pos.x, pos.y, window_width, window_height,
                               SWP_NOACTIVATE | SWP_SHOWWINDOW);
                shown = true;
            }
            if (frame.reveal_main_window) reveal();
            if (frame.done) return;
            wait_for_next_frame();
        }
    }

    static void pump_messages() {
        MSG msg{};
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
    }

    // 跟着合成器的刷新节奏走;合成器不等待(或不可用)时退回约 60 帧。
    static void wait_for_next_frame() {
        const auto before = std::chrono::steady_clock::now();
        if (FAILED(::DwmFlush())) {
            ::Sleep(15);
            return;
        }
        if (std::chrono::steady_clock::now() - before < std::chrono::milliseconds(2)) {
            ::Sleep(12);
        }
    }

    void present(const splash_anim::Frame& frame) {
        ::GdiFlush();
        std::memset(surface_bits, 0,
                    static_cast<std::size_t>(window_width) *
                    static_cast<std::size_t>(window_height) * 4);
        paint_splash_icon(surface_bits, window_width, window_height,
                          (window_width - icon_size) / 2.0, icon_top, icon_size, frame);
        blend_status_layer(frame.status_opacity);

        HDC screen = ::GetDC(nullptr);
        POINT src{0, 0};
        SIZE size{window_width, window_height};
        BLENDFUNCTION blend{};
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = static_cast<BYTE>(
            std::clamp(frame.window_opacity, 0.0, 1.0) * 255.0 + 0.5);
        blend.AlphaFormat = AC_SRC_ALPHA;
        ::UpdateLayeredWindow(hwnd, screen, &pos, &size, surface_dc, &src, 0, &blend, ULW_ALPHA);
        if (screen) ::ReleaseDC(nullptr, screen);
    }

    // 状态条只在文字变化时重画,每帧按透明度叠到图标下方。
    void blend_status_layer(double opacity) {
        if (opacity <= 0.0 || ::IsRectEmpty(&status_rect)) return;
        const std::uint32_t scale = static_cast<std::uint32_t>(
            std::clamp(opacity, 0.0, 1.0) * 255.0 + 0.5);
        for (int y = status_rect.top; y < status_rect.bottom; ++y) {
            for (int x = status_rect.left; x < status_rect.right; ++x) {
                const auto offset = static_cast<std::size_t>(y) * window_width + x;
                const std::uint32_t source = status_layer[offset];
                if (source == 0) continue;
                const std::uint32_t target = surface_bits[offset];
                std::uint32_t out = 0;
                const std::uint32_t source_alpha = (((source >> 24u) & 0xFFu) * scale + 127u) / 255u;
                for (unsigned shift = 0; shift <= 24; shift += 8) {
                    const std::uint32_t s = (((source >> shift) & 0xFFu) * scale + 127u) / 255u;
                    const std::uint32_t d = (target >> shift) & 0xFFu;
                    const std::uint32_t channel = s + (d * (255u - source_alpha) + 127u) / 255u;
                    out |= std::min<std::uint32_t>(channel, 255u) << shift;
                }
                surface_bits[offset] = out;
            }
        }
    }

    void render_status_layer(const std::wstring& status) {
        std::fill(status_layer.begin(), status_layer.end(), 0u);
        status_rect = RECT{};
        if (status.empty()) return;

        HDC screen = ::GetDC(nullptr);
        if (!screen) return;
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = window_width;
        bmi.bmiHeader.biHeight = -window_height;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

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
            ? ::CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &mask_bits, nullptr, 0)
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
            const int pill_top = icon_top + icon_size + 8;
            RECT pill{
                (window_width - pill_width) / 2,
                pill_top,
                (window_width + pill_width) / 2,
                std::min(window_height - 4, pill_top + pill_height),
            };
            fill_rounded_rect(
                status_layer.data(), window_width, window_height,
                pill, 12, premultiplied_bgra(27, 24, 24, 180));

            RECT text_rect{
                pill.left + 16, pill.top, pill.right - 16, pill.bottom};
            ::DrawTextW(
                text_dc, status.c_str(), static_cast<int>(status.size()),
                &text_rect,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                    DT_NOPREFIX);
            ::GdiFlush();
            composite_grayscale_text(
                status_layer.data(),
                static_cast<const std::uint32_t*>(mask_bits),
                window_width, window_height, text_rect);
            status_rect = pill;

            if (old_font) ::SelectObject(text_dc, old_font);
            ::SelectObject(text_dc, old_mask_bitmap);
        }
        if (mask_bitmap) ::DeleteObject(mask_bitmap);
        if (text_dc) ::DeleteDC(text_dc);
        if (font) ::DeleteObject(font);
        ::ReleaseDC(nullptr, screen);
    }
};

#elif !defined(__APPLE__)

struct SplashScreen::Impl {
    GtkWidget* window = nullptr;
    GtkWidget* status_label = nullptr;

    static void flush_events() {
        // Startup also waits synchronously for the daemon. Paint the logo and
        // status at each milestone without starting a second GUI event loop.
        for (int i = 0; i < 64 && gtk_events_pending(); ++i) {
            gtk_main_iteration_do(FALSE);
        }
        if (auto* display = gdk_display_get_default()) gdk_display_flush(display);
    }

    void show() {
        if (window || !gtk_init_check(nullptr, nullptr)) return;
        const auto area = linux_active_work_area();
        const int icon_size = std::clamp(
            std::min(area.right - area.left, area.bottom - area.top) / 6,
            160, 256);
        const auto icon_path = application_icon_path();
        GdkPixbuf* icon = gdk_pixbuf_new_from_file_at_scale(
            icon_path.c_str(), icon_size, icon_size, TRUE, nullptr);
        if (!icon) return;

        window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title(GTK_WINDOW(window), "ACECode");
        gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
        gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
        gtk_window_set_skip_taskbar_hint(GTK_WINDOW(window), TRUE);
        gtk_window_set_skip_pager_hint(GTK_WINDOW(window), TRUE);
        gtk_window_set_accept_focus(GTK_WINDOW(window), FALSE);
        gtk_window_set_focus_on_map(GTK_WINDOW(window), FALSE);
        gtk_window_set_type_hint(GTK_WINDOW(window), GDK_WINDOW_TYPE_HINT_SPLASHSCREEN);
        gtk_window_set_icon(GTK_WINDOW(window), icon);
        gtk_widget_set_app_paintable(window, TRUE);
        if (auto* visual = gdk_screen_get_rgba_visual(gtk_widget_get_screen(window))) {
            gtk_widget_set_visual(window, visual);
        }
        g_signal_connect(window, "draw", G_CALLBACK(+[](GtkWidget*, cairo_t* cr, gpointer) -> gboolean {
            cairo_save(cr);
            cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
            cairo_set_source_rgba(cr, 0, 0, 0, 0);
            cairo_paint(cr);
            cairo_restore(cr);
            return FALSE;
        }), nullptr);
        g_signal_connect(window, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer data) {
            auto* self = static_cast<Impl*>(data);
            self->window = nullptr;
            self->status_label = nullptr;
        }), this);

        auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        gtk_container_add(GTK_CONTAINER(window), box);
        gtk_box_pack_start(GTK_BOX(box), gtk_image_new_from_pixbuf(icon), FALSE, FALSE, 0);
        g_object_unref(icon);
        status_label = gtk_label_new("ACECode");
        gtk_label_set_ellipsize(GTK_LABEL(status_label), PANGO_ELLIPSIZE_END);
        gtk_widget_set_size_request(status_label, icon_size + 160, 36);
        auto* css = gtk_css_provider_new();
        gtk_css_provider_load_from_data(css,
            "label { color: white; background-color: rgba(27,24,24,0.7);"
            " border-radius: 12px; padding: 4px 16px; font-size: 13px; }",
            -1, nullptr);
        gtk_style_context_add_provider(gtk_widget_get_style_context(status_label),
            GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(css);
        gtk_box_pack_start(GTK_BOX(box), status_label, FALSE, FALSE, 0);
        gtk_window_set_default_size(GTK_WINDOW(window), icon_size + 160, icon_size + 44);
        center_linux_window(window, area);
        gtk_widget_show_all(window);
        flush_events();
    }

    void set_status(const std::string& message, std::uint64_t) {
        if (!status_label) return;
        gtk_label_set_text(GTK_LABEL(status_label), message.c_str());
        flush_events();
    }

    void close() {
        if (window) gtk_widget_destroy(window);
    }

    void finish(std::function<void()> on_reveal) {
        close();
        if (on_reveal) on_reveal();
    }
};

#else

struct SplashScreen::Impl {
    void show() {}
    void set_status(const std::string&, std::uint64_t) {}
    void close() {}
    void finish(std::function<void()> on_reveal) {
        if (on_reveal) on_reveal();
    }
};

#endif

SplashScreen::SplashScreen() = default;

SplashScreen::~SplashScreen() {
    close();
}

void SplashScreen::show() {
    if (!impl_) impl_ = std::make_unique<Impl>();
    impl_->show();
}

void SplashScreen::set_status(const std::string& message,
                              std::uint64_t elapsed_ms) {
    if (!impl_) impl_ = std::make_unique<Impl>();
    impl_->set_status(message, elapsed_ms);
}

void SplashScreen::finish(std::function<void()> on_reveal) {
    if (!impl_) {
        if (on_reveal) on_reveal();
        return;
    }
    impl_->finish(std::move(on_reveal));
}

void SplashScreen::close() {
    if (!impl_) return;
    impl_->close();
    impl_.reset();
}

} // namespace acecode::desktop
