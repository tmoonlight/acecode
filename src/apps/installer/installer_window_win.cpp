#include "installer_window_win.hpp"

#include "desktop/splash_icon_painter_win.hpp"
#include "installer_canvas_win.hpp"
#include "installer_timeline.hpp"
#include "platform/native_ui/folder_picker.hpp"
#include "utils/joining_thread.hpp"

#include <windows.h>
#include <dwmapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

namespace acecode::installer::ui {
namespace {

using Gdiplus::Color;
using Gdiplus::PointF;
using Gdiplus::RectF;
using Clock = std::chrono::steady_clock;

constexpr wchar_t kWindowClass[] = L"ACECodeSetupWindow";
constexpr UINT_PTR kFrameTimer = 1;
constexpr float kMargin = 30.0f;
constexpr float kCardWidth = 440.0f;
constexpr float kCardHeight = 452.0f;
constexpr float kIconSize = 124.0f;
constexpr float kIconTop = 34.0f;

const Color kText(255, 242, 246, 251);
const Color kSoftText(255, 214, 225, 238);
const Color kMuted(255, 138, 158, 186);
const Color kAccentA(255, 0x38, 0xD5, 0xF7);
const Color kAccentB(255, 0x25, 0x63, 0xEB);

enum class Page {
    welcome,
    installing,
    installed,
    failed,
    running,
    confirm_uninstall,
    uninstalling,
    uninstalled,
    uninstall_failed,
};

enum class Hit { none, close, change_dir, toggle_path, toggle_desktop, primary, secondary };

struct Hotspot {
    Hit id;
    RectF rect;
};

std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    const int length = ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                             static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(std::max(0, length)), L'\0');
    if (length > 0) {
        ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                              out.data(), length);
    }
    return out;
}

UINT window_dpi(HWND hwnd) {
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static const auto fn = reinterpret_cast<GetDpiForWindowFn>(
        ::GetProcAddress(::GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    if (fn && hwnd) {
        if (const UINT dpi = fn(hwnd)) return dpi;
    }
    HDC screen = ::GetDC(nullptr);
    const int dpi = screen ? ::GetDeviceCaps(screen, LOGPIXELSX) : 96;
    if (screen) ::ReleaseDC(nullptr, screen);
    return static_cast<UINT>(std::max(96, dpi));
}

RECT cursor_work_area() {
    POINT pt{};
    ::GetCursorPos(&pt);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    ::GetMonitorInfoW(::MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY), &info);
    return info.rcWork;
}

// 按显示器 DPI 缩放;工作区放不下(矮屏幕配高缩放)时整体缩小到能放下。
float fitted_scale(float dpi_scale, const RECT& area) {
    const float width = static_cast<float>(area.right - area.left) - 24.0f;
    const float height = static_cast<float>(area.bottom - area.top) - 24.0f;
    const float fit = std::min(width / (kCardWidth + 2.0f * kMargin),
                               height / (kCardHeight + 2.0f * kMargin));
    return fit > 0.0f ? std::min(dpi_scale, fit) : dpi_scale;
}

double ms_between(Clock::time_point from, Clock::time_point to) {
    return std::chrono::duration<double, std::milli>(to - from).count();
}

bool contains(const RectF& rect, float x, float y) {
    return x >= rect.X && x < rect.GetRight() && y >= rect.Y && y < rect.GetBottom();
}

}  // namespace

class InstallerWindow final : public StepObserver {
public:
    explicit InstallerWindow(const WindowConfig& config)
        : config_(config),
          s_(config.strings),
          install_dir_(config.install_dir),
          add_to_path_(config.add_to_path),
          desktop_shortcut_(config.desktop_shortcut),
          path_locked_(config.existing.has_value() || config.mode == InstallerMode::uninstall),
          page_(config.mode == InstallerMode::install ? Page::welcome : Page::confirm_uninstall) {
        toggle_path_anim_ = add_to_path_ ? 1.0 : 0.0;
        toggle_desktop_anim_ = desktop_shortcut_ ? 1.0 : 0.0;
    }

    ~InstallerWindow() override {
        worker_.join();
        if (hwnd_) ::DestroyWindow(hwnd_);
    }

    int run() {
        if (!create()) return 1;
        MSG msg{};
        while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        return exit_code_;
    }

    void on_step(InstallStep step, double fraction) override {
        std::lock_guard<std::mutex> lock(mu_);
        step_ = step;
        step_fraction_ = fraction;
    }

private:
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
        if (msg == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                                reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        auto* self = reinterpret_cast<InstallerWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self && self->hwnd_ == hwnd) return self->handle(msg, wparam, lparam);
        return ::DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    bool create() {
        HINSTANCE instance = ::GetModuleHandleW(nullptr);
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &InstallerWindow::wnd_proc;
        wc.hInstance = instance;
        wc.hCursor = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        wc.hIcon = ::LoadIconW(instance, MAKEINTRESOURCEW(1));
        wc.hIconSm = wc.hIcon;
        wc.lpszClassName = kWindowClass;
        if (!::RegisterClassExW(&wc) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }
        const std::wstring title = wide(config_.mode == InstallerMode::install
                                            ? s_.setup_title
                                            : s_.uninstall_window_title);
        // 先建在鼠标所在显示器上,GetDpiForWindow 才取到那块屏的 DPI。
        const RECT area = cursor_work_area();
        hwnd_ = ::CreateWindowExW(WS_EX_LAYERED | WS_EX_APPWINDOW, kWindowClass, title.c_str(),
                                  WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX, area.left, area.top, 1,
                                  1, nullptr, nullptr, instance, this);
        if (!hwnd_) return false;
        ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        apply_scale(fitted_scale(static_cast<float>(window_dpi(hwnd_)) / 96.0f, area));
        pos_.x = area.left + ((area.right - area.left) - width_) / 2;
        pos_.y = area.top + ((area.bottom - area.top) - height_) / 2;
        started_ = scene_started_ = last_tick_ = Clock::now();
        render();
        ::ShowWindow(hwnd_, SW_SHOW);
        ::SetForegroundWindow(hwnd_);
        ::SetTimer(hwnd_, kFrameTimer, 15, nullptr);
        return true;
    }

    void apply_scale(float scale) {
        scale_ = std::max(0.75f, scale);
        width_ = static_cast<int>(std::lround((kCardWidth + 2.0f * kMargin) * scale_));
        height_ = static_cast<int>(std::lround((kCardHeight + 2.0f * kMargin) * scale_));
        surface_.resize(width_, height_);
        CardStyle style;
        style.card = R(0.0f, 0.0f, kCardWidth, kCardHeight);
        style.radius = 18.0f * scale_;
        style.shadow = kMargin * scale_ * 0.9f;
        const RectF icon = R((kCardWidth - kIconSize) / 2.0f, kIconTop, kIconSize, kIconSize);
        style.halo_center = PointF(icon.X + icon.Width / 2.0f, icon.Y + icon.Height / 2.0f);
        style.halo_radius = 210.0f * scale_;
        background_ = render_card_background(width_, height_, style);
        fonts_ = make_fonts(scale_);
    }

    RectF R(float x, float y, float w, float h) const {
        return RectF((kMargin + x) * scale_, (kMargin + y) * scale_, w * scale_, h * scale_);
    }

    double clock_ms() const { return ms_between(started_, Clock::now()); }
    double scene_ms() const { return ms_between(scene_started_, Clock::now()); }
    bool busy() const { return page_ == Page::installing || page_ == Page::uninstalling; }

    void set_page(Page page) {
        page_ = page;
        scene_started_ = Clock::now();
        hover_ = pressed_ = Hit::none;
    }

    LRESULT handle(UINT msg, WPARAM wparam, LPARAM lparam) {
        switch (msg) {
            case WM_TIMER:
                if (wparam == kFrameTimer) tick();
                return 0;
            case WM_NCHITTEST: {
                POINT pt{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
                ::ScreenToClient(hwnd_, &pt);
                return hit_at(pt) != Hit::none ? HTCLIENT : HTCAPTION;
            }
            case WM_MOUSEMOVE: {
                hover_ = hit_at(POINT{static_cast<short>(LOWORD(lparam)),
                                      static_cast<short>(HIWORD(lparam))});
                if (!tracking_mouse_) {
                    TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd_, 0};
                    tracking_mouse_ = ::TrackMouseEvent(&track) != FALSE;
                }
                return 0;
            }
            case WM_MOUSELEAVE:
            case WM_NCMOUSEMOVE:
                tracking_mouse_ = msg == WM_NCMOUSEMOVE && tracking_mouse_;
                hover_ = Hit::none;
                return msg == WM_NCMOUSEMOVE ? ::DefWindowProcW(hwnd_, msg, wparam, lparam) : 0;
            case WM_SETCURSOR:
                if (LOWORD(lparam) == HTCLIENT) {
                    ::SetCursor(::LoadCursorW(
                        nullptr, MAKEINTRESOURCEW(hover_ != Hit::none ? 32649 : 32512)));
                    return TRUE;
                }
                break;
            case WM_LBUTTONDOWN:
                pressed_ = hover_;
                ::SetCapture(hwnd_);
                return 0;
            case WM_LBUTTONUP: {
                ::ReleaseCapture();
                const Hit released = hit_at(POINT{static_cast<short>(LOWORD(lparam)),
                                                  static_cast<short>(HIWORD(lparam))});
                const Hit target = pressed_;
                pressed_ = Hit::none;
                if (target != Hit::none && target == released) activate(target);
                return 0;
            }
            case WM_KEYDOWN:
                on_key(wparam);
                return 0;
            case WM_DPICHANGED: {
                const auto* suggested = reinterpret_cast<const RECT*>(lparam);
                MONITORINFO info{};
                info.cbSize = sizeof(info);
                ::GetMonitorInfoW(::MonitorFromRect(suggested, MONITOR_DEFAULTTONEAREST), &info);
                apply_scale(fitted_scale(static_cast<float>(HIWORD(wparam)) / 96.0f, info.rcWork));
                pos_.x = suggested->left;
                pos_.y = suggested->top;
                render();
                return 0;
            }
            case WM_CLOSE:
                if (!busy()) ::DestroyWindow(hwnd_);
                return 0;
            case WM_DESTROY:
                ::KillTimer(hwnd_, kFrameTimer);
                hwnd_ = nullptr;
                ::PostQuitMessage(exit_code_);
                return 0;
            default:
                break;
        }
        return ::DefWindowProcW(hwnd_, msg, wparam, lparam);
    }

    Hit hit_at(POINT pt) const {
        const float x = static_cast<float>(pt.x);
        const float y = static_cast<float>(pt.y);
        for (auto it = hotspots_.rbegin(); it != hotspots_.rend(); ++it) {
            if (contains(it->rect, x, y)) return it->id;
        }
        return Hit::none;
    }

    // ── 交互 ──

    void activate(Hit hit) {
        switch (hit) {
            case Hit::close:
                close_window(page_ == Page::installed || page_ == Page::uninstalled ? 0 : 1);
                break;
            case Hit::change_dir: {
                const auto picked = acecode::desktop::pick_folder(hwnd_);
                if (picked) install_dir_ = normalize_chosen_install_dir(*picked);
                break;
            }
            case Hit::toggle_path:
                add_to_path_ = !add_to_path_;
                break;
            case Hit::toggle_desktop:
                desktop_shortcut_ = !desktop_shortcut_;
                break;
            case Hit::primary:
                primary_action();
                break;
            case Hit::secondary:
                secondary_action();
                break;
            case Hit::none:
                break;
        }
    }

    void primary_action() {
        switch (page_) {
            case Page::welcome:
                if (timeline::content_reveal(clock_ms()) >= 0.99) start_worker();
                break;
            case Page::failed:
            case Page::running:
            case Page::uninstall_failed:
            case Page::confirm_uninstall:
                start_worker();
                break;
            case Page::installed:
                launch_installed_app(install_dir_);
                close_window(0);
                break;
            case Page::uninstalled:
                close_window(0);
                break;
            default:
                break;
        }
    }

    void secondary_action() {
        switch (page_) {
            case Page::installed:
                close_window(0);
                break;
            case Page::running:
                if (config_.mode == InstallerMode::install) {
                    set_page(Page::welcome);
                } else {
                    set_page(Page::confirm_uninstall);
                }
                break;
            case Page::failed:
            case Page::uninstall_failed:
            case Page::confirm_uninstall:
                close_window(1);
                break;
            default:
                break;
        }
    }

    void on_key(WPARAM key) {
        if (key == VK_RETURN) {
            activate(Hit::primary);
        } else if (key == VK_ESCAPE) {
            if (page_ == Page::installed || page_ == Page::failed || page_ == Page::running ||
                page_ == Page::uninstall_failed || page_ == Page::confirm_uninstall) {
                secondary_action();
            } else if (!busy()) {
                close_window(page_ == Page::uninstalled ? 0 : 1);
            }
        }
    }

    void close_window(int code) {
        exit_code_ = code;
        if (hwnd_) ::DestroyWindow(hwnd_);
    }

    // ── 工作线程 ──

    void start_worker() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            step_ = InstallStep::preparing;
            step_fraction_ = 0.0;
            worker_done_ = false;
        }
        request_.install_dir = install_dir_;
        request_.add_to_path = add_to_path_;
        request_.desktop_shortcut = desktop_shortcut_;
        request_.terminal_shortcut_name = s_.terminal_shortcut;
        target_progress_ = shown_progress_ = 0.0;
        pending_outcome_.reset();
        set_page(config_.mode == InstallerMode::install ? Page::installing : Page::uninstalling);
        worker_ = JoiningThread(&InstallerWindow::run_worker, this);
    }

    void run_worker() {
        Outcome outcome;
        if (config_.mode == InstallerMode::install && config_.payload) {
            outcome = install(*config_.payload, request_, *this);
        } else if (config_.mode == InstallerMode::uninstall) {
            outcome = uninstall(request_.install_dir, *this);
        }
        std::lock_guard<std::mutex> lock(mu_);
        worker_outcome_ = outcome;
        worker_done_ = true;
    }

    void poll_worker() {
        if (!busy()) return;
        {
            std::lock_guard<std::mutex> lock(mu_);
            target_progress_ = std::max(target_progress_, overall_progress(step_, step_fraction_));
            current_step_ = step_;
            if (worker_done_ && !pending_outcome_) pending_outcome_ = worker_outcome_;
        }
        if (!pending_outcome_) return;
        const Outcome& outcome = *pending_outcome_;
        if (outcome.ok) {
            target_progress_ = 1.0;
            if (shown_progress_ < 0.985) return;
        }
        level_at_end_ = shown_progress_;
        const bool installing = config_.mode == InstallerMode::install;
        if (outcome.ok) {
            exit_code_ = 0;
            set_page(installing ? Page::installed : Page::uninstalled);
        } else if (outcome.failure == FailureKind::running) {
            // detail 是挡住安装的进程映像路径;只显示文件名,告诉用户该去关哪一个。
            const auto slash = outcome.detail.find_last_of("\\/");
            failure_detail_ = slash == std::string::npos ? outcome.detail
                                                         : outcome.detail.substr(slash + 1);
            set_page(Page::running);
        } else {
            failure_text_ = failure_message(s_, outcome.failure);
            failure_detail_ = outcome.detail;
            set_page(installing ? Page::failed : Page::uninstall_failed);
        }
        pending_outcome_.reset();
    }

    // ── 每帧 ──

    void tick() {
        const auto now = Clock::now();
        const double dt = ms_between(last_tick_, now);
        last_tick_ = now;
        poll_worker();
        shown_progress_ = timeline::approach(shown_progress_, target_progress_, dt);
        const auto ease_toggle = [dt](double value, bool on) {
            const double target = on ? 1.0 : 0.0;
            const double k = 1.0 - std::exp(-dt / 60.0);
            return std::abs(target - value) < 0.002 ? target : value + (target - value) * k;
        };
        toggle_path_anim_ = ease_toggle(toggle_path_anim_, add_to_path_);
        toggle_desktop_anim_ = ease_toggle(toggle_desktop_anim_, desktop_shortcut_);
        render();
    }

    timeline::splash_anim::Frame icon_frame() const {
        const double clock = clock_ms();
        const double scene = scene_ms();
        switch (page_) {
            case Page::installing: return timeline::installing(clock, shown_progress_);
            case Page::installed: return timeline::finished(scene, clock, level_at_end_);
            case Page::failed: return timeline::failed(scene, clock, level_at_end_);
            case Page::uninstalling: return timeline::uninstalling(clock, shown_progress_);
            case Page::uninstalled: return timeline::removed(scene);
            case Page::uninstall_failed: return timeline::failed(scene, clock, 0.0);
            default: return timeline::welcome(clock);
        }
    }

    void render() {
        if (!hwnd_ || !surface_.bits()) return;
        std::memcpy(surface_.bits(), background_.data(), background_.size() * sizeof(std::uint32_t));
        const RectF icon = R((kCardWidth - kIconSize) / 2.0f, kIconTop, kIconSize, kIconSize);
        acecode::desktop::paint_splash_icon(surface_.bits(), width_, height_, icon.X, icon.Y,
                                            icon.Width, icon_frame());
        hotspots_.clear();
        {
            Painter painter(surface_.bits(), width_, height_);
            draw_page(painter);
        }
        HDC screen = ::GetDC(nullptr);
        POINT src{0, 0};
        SIZE size{width_, height_};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        ::UpdateLayeredWindow(hwnd_, screen, &pos_, &size, surface_.dc(), &src, 0, &blend,
                              ULW_ALPHA);
        if (screen) ::ReleaseDC(nullptr, screen);
    }

    // ── 页面 ──

    void draw_page(Painter& p) {
        const float alpha = static_cast<float>(
            page_ == Page::welcome || page_ == Page::confirm_uninstall
                ? std::min(timeline::content_reveal(clock_ms()), 1.0)
                : timeline::page_reveal(scene_ms()));
        const float dy = (1.0f - alpha) * 8.0f;
        if (!busy()) draw_close(p, alpha);
        const std::string version = config_.payload ? config_.payload->info.version
                                    : config_.existing ? config_.existing->version
                                                       : std::string();
        switch (page_) {
            case Page::welcome: {
                std::string line = version;
                const auto action = classify_install(
                    config_.existing ? std::optional<std::string>(config_.existing->version)
                                     : std::nullopt,
                    version);
                if (action == InstallAction::update) line = config_.existing->version + "  →  " + version;
                draw_title(p, kProductName, line, alpha, dy);
                draw_path_box(p, R(36, 240 + dy, 368, 40), !path_locked_, alpha);
                draw_toggle(p, Hit::toggle_path, s_.add_to_path, toggle_path_anim_, 292 + dy, alpha);
                draw_toggle(p, Hit::toggle_desktop, s_.desktop_shortcut, toggle_desktop_anim_,
                            326 + dy, alpha);
                const std::string& label = action == InstallAction::update      ? s_.update
                                           : action == InstallAction::reinstall ? s_.reinstall
                                                                                : s_.install;
                draw_button(p, Hit::primary, label, R(36, 380 + dy, 368, 44), true, alpha);
                break;
            }
            case Page::installing:
            case Page::uninstalling: {
                draw_title(p, page_ == Page::installing ? std::string(kProductName)
                                                        : s_.uninstall_title,
                           page_ == Page::installing ? version : std::string(), 1.0f, 0.0f);
                const std::string status = page_ == Page::uninstalling ? s_.uninstalling
                                           : current_step_ == InstallStep::extracting ? s_.extracting
                                           : current_step_ == InstallStep::replacing  ? s_.replacing
                                           : current_step_ == InstallStep::integrating ||
                                                   current_step_ == InstallStep::finished
                                               ? s_.integrating
                                               : s_.preparing;
                p.text(wide(status), R(24, 292, 392, 24), *fonts_.body, kSoftText, Align::center);
                draw_progress(p, R(70, 332, 300, 6), shown_progress_, alpha);
                break;
            }
            case Page::installed:
                draw_title(p, s_.installed_title, version, alpha, dy);
                draw_button(p, Hit::secondary, s_.done, R(36, 380 + dy, 178, 44), false, alpha);
                draw_button(p, Hit::primary, s_.launch, R(226, 380 + dy, 178, 44), true, alpha);
                break;
            case Page::failed:
            case Page::uninstall_failed:
                draw_title(p, page_ == Page::failed ? s_.failed_title : s_.uninstall_failed_title,
                           {}, alpha, dy);
                draw_failure(p, alpha, dy);
                draw_button(p, Hit::secondary, s_.close, R(36, 380 + dy, 178, 44), false, alpha);
                draw_button(p, Hit::primary, s_.retry, R(226, 380 + dy, 178, 44), true, alpha);
                break;
            case Page::running:
                draw_title(p, s_.running_title, {}, alpha, dy);
                p.text(wide(s_.running_body), R(44, 238 + dy, 352, 44), *fonts_.body,
                       with_alpha(kSoftText, alpha), Align::center, true);
                if (!failure_detail_.empty()) {
                    p.text(wide(failure_detail_), R(44, 286 + dy, 352, 20), *fonts_.caption,
                           with_alpha(kMuted, alpha), Align::center);
                }
                draw_button(p, Hit::secondary, s_.cancel, R(36, 380 + dy, 178, 44), false, alpha);
                draw_button(p, Hit::primary, s_.retry, R(226, 380 + dy, 178, 44), true, alpha);
                break;
            case Page::confirm_uninstall:
                draw_title(p, s_.uninstall_title, version, alpha, dy);
                draw_path_box(p, R(36, 240 + dy, 368, 40), false, alpha);
                p.text(wide(s_.uninstall_body), R(36, 292 + dy, 368, 24), *fonts_.body,
                       with_alpha(kMuted, alpha), Align::center);
                draw_button(p, Hit::secondary, s_.cancel, R(36, 380 + dy, 178, 44), false, alpha);
                draw_button(p, Hit::primary, s_.uninstall, R(226, 380 + dy, 178, 44), true, alpha);
                break;
            case Page::uninstalled:
                draw_title(p, s_.uninstalled_title, {}, alpha, dy);
                draw_button(p, Hit::primary, s_.done, R(36, 380 + dy, 368, 44), true, alpha);
                break;
        }
    }

    void add_hotspot(Hit id, const RectF& rect, float alpha) {
        if (alpha > 0.6f) hotspots_.push_back({id, rect});
    }

    void draw_close(Painter& p, float alpha) {
        const RectF rect = R(kCardWidth - 44, 14, 30, 30);
        add_hotspot(Hit::close, rect, alpha);
        if (hover_ == Hit::close) {
            p.fill_ellipse(rect, with_alpha(Color(255, 255, 255, 255), 0.09f * alpha));
        }
        const float c = 5.5f * scale_;
        const PointF mid(rect.X + rect.Width / 2.0f, rect.Y + rect.Height / 2.0f);
        const Color color = with_alpha(hover_ == Hit::close ? kText : kMuted, alpha);
        p.line(PointF(mid.X - c, mid.Y - c), PointF(mid.X + c, mid.Y + c), color, 1.6f * scale_);
        p.line(PointF(mid.X - c, mid.Y + c), PointF(mid.X + c, mid.Y - c), color, 1.6f * scale_);
    }

    void draw_title(Painter& p, const std::string& title, const std::string& subtitle,
                    float alpha, float dy) {
        p.text(wide(title), R(24, 170 + dy, 392, 32), *fonts_.title, with_alpha(kText, alpha),
               Align::center);
        if (!subtitle.empty()) {
            p.text(wide(subtitle), R(24, 203 + dy, 392, 20), *fonts_.subtitle,
                   with_alpha(kMuted, alpha), Align::center);
        }
    }

    void draw_path_box(Painter& p, const RectF& box, bool changeable, float alpha) {
        p.fill_round(box, 10.0f * scale_, with_alpha(Color(255, 255, 255, 255), 0.05f * alpha));
        p.stroke_round(box, 10.0f * scale_, with_alpha(Color(255, 255, 255, 255), 0.09f * alpha),
                       1.0f * scale_);
        const float s = scale_;
        const Color folder = with_alpha(kMuted, alpha);
        p.fill_round(RectF(box.X + 14 * s, box.Y + 13 * s, 7 * s, 4 * s), 1.5f * s, folder);
        p.fill_round(RectF(box.X + 14 * s, box.Y + 15 * s, 16 * s, 12 * s), 2.0f * s, folder);
        const float change_width = changeable ? 64.0f * s : 0.0f;
        p.text(wide(install_dir_),
               RectF(box.X + 40 * s, box.Y, box.Width - 52 * s - change_width, box.Height),
               *fonts_.body, with_alpha(kSoftText, alpha), Align::start, false, true);
        if (!changeable) return;
        const RectF change(box.GetRight() - change_width - 4 * s, box.Y, change_width, box.Height);
        add_hotspot(Hit::change_dir, change, alpha);
        p.text(wide(s_.change), change, *fonts_.body,
               with_alpha(hover_ == Hit::change_dir ? kText : kAccentA, alpha), Align::center);
    }

    void draw_toggle(Painter& p, Hit id, const std::string& label, double value, float y,
                     float alpha) {
        const RectF row = R(36, y, 368, 30);
        add_hotspot(id, row, alpha);
        const float s = scale_;
        p.text(wide(label), RectF(row.X + 4 * s, row.Y, row.Width - 60 * s, row.Height),
               *fonts_.body, with_alpha(hover_ == id ? kText : kSoftText, alpha), Align::start);
        const RectF track(row.GetRight() - 40 * s, row.Y + 4 * s, 38 * s, 22 * s);
        const float v = static_cast<float>(value);
        p.fill_round(track, 11 * s, with_alpha(Color(255, 255, 255, 255), 0.13f * (1 - v) * alpha));
        if (v > 0.01f) {
            p.fill_round_gradient(track, 11 * s, with_alpha(kAccentA, v * alpha),
                                  with_alpha(kAccentB, v * alpha));
        }
        const float knob = 16 * s;
        const float x = track.X + 3 * s + (track.Width - knob - 6 * s) * v;
        p.fill_ellipse(RectF(x, track.Y + 3 * s, knob, knob),
                       with_alpha(Color(255, 248, 250, 253), alpha));
    }

    void draw_button(Painter& p, Hit id, const std::string& label, const RectF& rect,
                     bool primary, float alpha) {
        const bool enabled = alpha > 0.98f;
        if (enabled) add_hotspot(id, rect, alpha);
        const float radius = 12.0f * scale_;
        const float a = enabled ? alpha : alpha * 0.55f;
        const bool hot = hover_ == id && enabled;
        const bool down = pressed_ == id && hot;
        if (primary) {
            p.fill_round_gradient(rect, radius, with_alpha(kAccentA, a), with_alpha(kAccentB, a));
            if (hot) {
                p.fill_round(rect, radius,
                             down ? with_alpha(Color(255, 0, 0, 0), 0.14f * a)
                                  : with_alpha(Color(255, 255, 255, 255), 0.10f * a));
            }
        } else {
            p.fill_round(rect, radius,
                         with_alpha(Color(255, 255, 255, 255), (hot ? 0.12f : 0.07f) * a));
            p.stroke_round(rect, radius, with_alpha(Color(255, 255, 255, 255), 0.10f * a),
                           1.0f * scale_);
        }
        p.text(wide(label), rect, *fonts_.button, with_alpha(primary ? kText : kSoftText, a),
               Align::center);
    }

    void draw_progress(Painter& p, const RectF& track, double value, float alpha) {
        const float radius = track.Height / 2.0f;
        p.fill_round(track, radius, with_alpha(Color(255, 255, 255, 255), 0.10f * alpha));
        const float width = std::max(track.Height,
                                     track.Width * static_cast<float>(std::clamp(value, 0.0, 1.0)));
        const RectF fill(track.X, track.Y, width, track.Height);
        p.fill_round_gradient(fill, radius, with_alpha(kAccentA, alpha), with_alpha(kAccentB, alpha));
        const float glow = 9.0f * scale_;
        p.fill_ellipse(RectF(fill.GetRight() - glow, track.Y + radius - glow, glow * 2, glow * 2),
                       with_alpha(kAccentA, 0.22f * alpha));
    }

    void draw_failure(Painter& p, float alpha, float dy) {
        p.text(wide(failure_text_), R(44, 236 + dy, 352, 66), *fonts_.body,
               with_alpha(kSoftText, alpha), Align::center, true);
        if (!failure_detail_.empty()) {
            p.text(wide(failure_detail_), R(44, 302 + dy, 352, 34), *fonts_.caption,
                   with_alpha(kMuted, alpha), Align::center, true);
        }
        const std::string log = setup_log_path();
        if (!log.empty()) {
            p.text(wide(s_.log_prefix + log), R(44, 340 + dy, 352, 18), *fonts_.caption,
                   with_alpha(kMuted, 0.8f * alpha), Align::center, false, true);
        }
    }

    WindowConfig config_;
    InstallerStrings s_;
    HWND hwnd_ = nullptr;
    float scale_ = 1.0f;
    int width_ = 0;
    int height_ = 0;
    POINT pos_{};
    Surface surface_;
    std::vector<std::uint32_t> background_;
    Fonts fonts_;
    Clock::time_point started_;
    Clock::time_point scene_started_;
    Clock::time_point last_tick_;
    std::string install_dir_;
    bool add_to_path_ = true;
    bool desktop_shortcut_ = false;
    bool path_locked_ = false;
    Page page_;
    double toggle_path_anim_ = 0.0;
    double toggle_desktop_anim_ = 0.0;
    Hit hover_ = Hit::none;
    Hit pressed_ = Hit::none;
    bool tracking_mouse_ = false;
    std::vector<Hotspot> hotspots_;
    double target_progress_ = 0.0;
    double shown_progress_ = 0.0;
    double level_at_end_ = 0.0;
    InstallStep current_step_ = InstallStep::preparing;
    std::optional<Outcome> pending_outcome_;
    std::string failure_text_;
    std::string failure_detail_;
    int exit_code_ = 1;
    InstallRequest request_;

    // 与工作线程共享,受 mu_ 保护。
    std::mutex mu_;
    InstallStep step_ = InstallStep::preparing;
    double step_fraction_ = 0.0;
    bool worker_done_ = false;
    Outcome worker_outcome_;

    // 最后声明:析构时最先等工作线程结束,它引用上面的成员。
    JoiningThread worker_;
};

int run_installer_window(const WindowConfig& config) {
    acecode::desktop::SplashGraphicsRuntime graphics;
    if (!graphics.ok()) return 1;
    InstallerWindow window(config);
    return window.run();
}

}  // namespace acecode::installer::ui
