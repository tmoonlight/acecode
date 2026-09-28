#include "pointer_overlay.hpp"
#include "config/vocab/pointer_appearance.hpp"

#ifdef _WIN32
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <iterator>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

namespace acecode::computer_use {
namespace {
using Clock = std::chrono::steady_clock;
constexpr wchar_t kClassName[] = L"ACECode.ComputerUse.PointerOverlay.13ECBC3A-77EF-45B8-932D-E11BC2B12FDB";
constexpr UINT kApply = WM_APP + 71;
constexpr UINT_PTR kAnimationTimer = 1;
constexpr DWORD kCommandTimeout = 1000;
constexpr auto kMoveDuration = std::chrono::milliseconds(120);
constexpr auto kPressDuration = std::chrono::milliseconds(250);

std::mutex registry_mutex;
std::set<HWND> registered_windows;

struct Desired {
    bool visible = false;
    POINT target{};
    UINT dpi = 96;
    bool pressed = false;
    std::string style = pointer_appearance::kDefaultStyle;
    std::string color = pointer_appearance::kDefaultColor;
    unsigned suppression = 0;
    std::uint64_t show_sequence = 0;
    std::uint64_t settle_sequence = 0;
};

struct SharedState {
    std::mutex mutex;
    Desired desired;
    PointerSprite rendered;
    HWND window = nullptr;
    HANDLE initialized = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE stopped = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    ~SharedState() {
        if (initialized) CloseHandle(initialized);
        if (stopped) CloseHandle(stopped);
        if (stop) CloseHandle(stop);
    }
};

struct Point { double x; double y; };
constexpr std::array<Point, 7> kArrow{{{0, 0}, {0, 26}, {7, 20}, {13, 31}, {19, 28}, {13, 18}, {24, 18}}};

bool inside_arrow(Point point) {
    bool inside = false;
    for (std::size_t i = 0, j = kArrow.size() - 1; i < kArrow.size(); j = i++) {
        const auto a = kArrow[i], b = kArrow[j];
        if ((a.y > point.y) != (b.y > point.y)
            && point.x < (b.x - a.x) * (point.y - a.y) / (b.y - a.y) + a.x) inside = !inside;
    }
    return inside;
}

double edge_distance(Point point) {
    double result = std::numeric_limits<double>::max();
    for (std::size_t i = 0, j = kArrow.size() - 1; i < kArrow.size(); j = i++) {
        const auto a = kArrow[i], b = kArrow[j];
        const double dx = b.x - a.x, dy = b.y - a.y;
        const double ratio = std::clamp(((point.x - a.x) * dx + (point.y - a.y) * dy) / (dx * dx + dy * dy), 0.0, 1.0);
        result = std::min(result, std::hypot(point.x - a.x - ratio * dx, point.y - a.y - ratio * dy));
    }
    return result;
}

double badge_distance(Point point) {
    const double x = std::abs(point.x - 32.5) - 9.5;
    const double y = std::abs(point.y - 30.5) - 3.5;
    return std::hypot(std::max(x, 0.0), std::max(y, 0.0)) + std::min(std::max(x, y), 0.0) - 3.0;
}

bool badge_text(Point point) {
    constexpr std::array<std::array<unsigned, 7>, 3> letters{{
        {{0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}},
        {{0x0f, 0x10, 0x10, 0x10, 0x10, 0x10, 0x0f}},
        {{0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f}}
    }};
    const int x = static_cast<int>(std::floor(point.x - 24));
    const int y = static_cast<int>(std::floor(point.y - 27));
    if (x < 0 || y < 0 || x >= 17 || y >= 7 || x % 6 >= 5) return false;
    return (letters[static_cast<std::size_t>(x / 6)][static_cast<std::size_t>(y)] & (1U << (4 - x % 6))) != 0;
}

PointerSprite draw_sprite(POINT position, UINT dpi, double press_progress,
                          const std::string& style, const std::string& color) {
    PointerSprite sprite;
    const double scale = std::clamp(dpi, 48U, 768U) / 96.0;
    const bool badge = style == "ace";
    const auto hex = [](char ch) -> unsigned { return ch <= '9' ? static_cast<unsigned>(ch - '0') : static_cast<unsigned>(ch - 'a' + 10); };
    const auto channel = [&](std::size_t index) -> double { return hex(color[index]) * 16 + hex(color[index + 1]); };
    const std::array<double, 4> fill{channel(5), channel(3), channel(1), 255};
    sprite.visible = true;
    sprite.position = position;
    sprite.width = static_cast<int>(std::ceil((badge ? 66 : 52) * scale));
    sprite.height = static_cast<int>(std::ceil((badge ? 58 : 52) * scale));
    sprite.hotspot_x = sprite.hotspot_y = static_cast<int>(std::lround(18 * scale));
    sprite.bgra.resize(static_cast<std::size_t>(sprite.width) * sprite.height * 4);
    constexpr int samples = 2;
    for (int y = 0; y < sprite.height; ++y) {
        for (int x = 0; x < sprite.width; ++x) {
            std::array<double, 4> pixel{};
            for (int sy = 0; sy < samples; ++sy) {
                for (int sx = 0; sx < samples; ++sx) {
                    const Point point{(x + (sx + .5) / samples - sprite.hotspot_x) / scale,
                                      (y + (sy + .5) / samples - sprite.hotspot_y) / scale};
                    std::array<double, 4> sample{};
                    if (press_progress >= 0) {
                        const double radius = 6.0 + 9.0 * press_progress;
                        const double distance = std::hypot(point.x, point.y);
                        const double alpha = std::abs(distance - radius) <= 1.5 ? .8 * (1.0 - press_progress) : 0;
                        for (std::size_t component = 0; component < sample.size(); ++component)
                            sample[component] = fill[component] * alpha;
                    }
                    const bool interior = inside_arrow(point);
                    const double edge = edge_distance(point);
                    if (interior || edge <= 1.05) {
                        if (!interior) sample = {24, 24, 24, 255};
                        else if (edge <= .85) sample = {255, 255, 255, 255};
                        else sample = fill;
                    }
                    if (badge) {
                        const auto distance = badge_distance(point);
                        if (distance <= .75) {
                            if (distance > 0) sample = {24, 24, 24, 255};
                            else if (distance >= -.65 || badge_text(point)) sample = {255, 255, 255, 255};
                            else sample = fill;
                        }
                    }
                    for (std::size_t component = 0; component < pixel.size(); ++component)
                        pixel[component] += sample[component] / (samples * samples);
                }
            }
            const auto offset = (static_cast<std::size_t>(y) * sprite.width + x) * 4;
            for (std::size_t component = 0; component < pixel.size(); ++component)
                sprite.bgra[offset + component] = static_cast<unsigned char>(std::clamp(std::lround(pixel[component]), 0L, 255L));
        }
    }
    return sprite;
}

class OverlayThread {
public:
    explicit OverlayThread(std::shared_ptr<SharedState> shared) : shared_(std::move(shared)) {}
    ~OverlayThread() {
        if (window_) DestroyWindow(window_);
        if (memory_ && original_bitmap_) SelectObject(memory_, original_bitmap_);
        if (bitmap_) DeleteObject(bitmap_);
        if (memory_) DeleteDC(memory_);
        if (screen_) ReleaseDC(nullptr, screen_);
    }

    void run() {
        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        WNDCLASSW klass{};
        klass.lpfnWndProc = procedure;
        klass.hInstance = GetModuleHandleW(nullptr);
        klass.lpszClassName = kClassName;
        if (!RegisterClassW(&klass)) {
            WNDCLASSW existing{};
            if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS
                || !GetClassInfoW(klass.hInstance, kClassName, &existing) || existing.lpfnWndProc != procedure) return;
        }
        window_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
            kClassName, L"ACECode Computer Use pointer", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, klass.hInstance, this);
        if (!window_) return;
        {
            std::lock_guard<std::mutex> lock(registry_mutex);
            registered_windows.insert(window_);
        }
        // Available on Windows 10 2004+. Input/capture callers additionally use
        // suppress(), so an older OS rejecting this flag cannot cause a double
        // cursor or make this visual an input target.
        SetWindowDisplayAffinity(window_, 0x00000011 /* WDA_EXCLUDEFROMCAPTURE */);
        {
            std::lock_guard<std::mutex> lock(shared_->mutex);
            shared_->window = window_;
        }
        SetEvent(shared_->initialized);
        for (;;) {
            const DWORD ready = MsgWaitForMultipleObjects(1, &shared_->stop, FALSE, INFINITE, QS_ALLINPUT);
            if (ready == WAIT_OBJECT_0 || ready == WAIT_FAILED) break;
            MSG message{};
            bool quit = false;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) { quit = true; break; }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (quit) break;
        }
    }

private:
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<OverlayThread*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<OverlayThread*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (message == WM_NCHITTEST) return HTTRANSPARENT;
        if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
        if (!self) return DefWindowProcW(window, message, wparam, lparam);
        if (message == kApply || (message == WM_TIMER && wparam == kAnimationTimer)) {
            try { self->apply(); }
            catch (...) {
                // A visual failure must never escape the window procedure or
                // leave the application with a stuck overlay/input operation.
                ShowWindow(window, SW_HIDE);
                KillTimer(window, kAnimationTimer);
                std::lock_guard<std::mutex> lock(self->shared_->mutex);
                self->shared_->rendered.visible = false;
            }
            return 0;
        }
        if (message == WM_CLOSE) { DestroyWindow(window); return 0; }
        if (message == WM_DESTROY) {
            KillTimer(window, kAnimationTimer);
            {
                std::lock_guard<std::mutex> lock(registry_mutex);
                registered_windows.erase(window);
            }
            {
                std::lock_guard<std::mutex> lock(self->shared_->mutex);
                self->shared_->window = nullptr;
                self->shared_->rendered.visible = false;
            }
            self->window_ = nullptr;
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    bool present(const PointerSprite& sprite, bool suppressed) {
        if (!screen_) screen_ = GetDC(nullptr);
        if (screen_ && !memory_) memory_ = CreateCompatibleDC(screen_);
        if (!memory_) return false;
        if (!bitmap_ || bitmap_width_ != sprite.width || bitmap_height_ != sprite.height) {
            if (original_bitmap_) SelectObject(memory_, original_bitmap_);
            if (bitmap_) { DeleteObject(bitmap_); bitmap_ = nullptr; }
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = sprite.width;
            info.bmiHeader.biHeight = -sprite.height;
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            bitmap_ = CreateDIBSection(screen_, &info, DIB_RGB_COLORS, &pixels_, nullptr, 0);
            if (!bitmap_ || !pixels_) return false;
            const auto previous = SelectObject(memory_, bitmap_);
            if (!previous || previous == HGDI_ERROR) return false;
            if (!original_bitmap_) original_bitmap_ = previous;
            bitmap_width_ = sprite.width;
            bitmap_height_ = sprite.height;
        }
        std::copy(sprite.bgra.begin(), sprite.bgra.end(), static_cast<unsigned char*>(pixels_));
        POINT position{sprite.position.x - sprite.hotspot_x, sprite.position.y - sprite.hotspot_y};
        POINT source{};
        SIZE size{sprite.width, sprite.height};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        if (!UpdateLayeredWindow(window_, screen_, &position, &size, memory_, &source, 0, &blend, ULW_ALPHA)) return false;
        if (suppressed) ShowWindow(window_, SW_HIDE);
        else SetWindowPos(window_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        return true;
    }

    void apply() {
        Desired desired;
        {
            std::lock_guard<std::mutex> lock(shared_->mutex);
            desired = shared_->desired;
        }
        const auto now = Clock::now();
        if (!desired.visible) {
            ShowWindow(window_, SW_HIDE);
            KillTimer(window_, kAnimationTimer);
            was_visible_ = false;
            std::lock_guard<std::mutex> lock(shared_->mutex);
            shared_->rendered.visible = false;
            return;
        }
        if (desired.show_sequence != show_sequence_) {
            from_ = current_;
            target_ = desired.target;
            if (!was_visible_) from_ = target_;
            movement_started_ = now;
            show_sequence_ = desired.show_sequence;
            press_started_ = desired.pressed ? now : Clock::time_point{};
        }
        const bool settle = desired.settle_sequence != settle_sequence_;
        settle_sequence_ = desired.settle_sequence;
        if (settle) movement_started_ = now - kMoveDuration;
        const double progress = std::clamp(std::chrono::duration<double>(now - movement_started_).count()
            / std::chrono::duration<double>(kMoveDuration).count(), 0.0, 1.0);
        const double eased = 1.0 - std::pow(1.0 - progress, 3.0);
        current_ = {static_cast<LONG>(std::lround(from_.x + (static_cast<double>(target_.x) - from_.x) * eased)),
                    static_cast<LONG>(std::lround(from_.y + (static_cast<double>(target_.y) - from_.y) * eased))};
        double press_progress = -1;
        if (press_started_ != Clock::time_point{}) {
            press_progress = std::chrono::duration<double>(now - press_started_).count() / std::chrono::duration<double>(kPressDuration).count();
            if (press_progress >= 1) press_progress = -1;
        }
        PointerSprite sprite;
        if (press_progress < 0) {
            // Moving an unchanged arrow should not rasterize its polygon on
            // every timer tick. Only a changing pulse needs a fresh sprite.
            if (normal_sprite_.bgra.empty() || normal_dpi_ != desired.dpi
                || normal_style_ != desired.style || normal_color_ != desired.color) {
                normal_sprite_ = draw_sprite(current_, desired.dpi, -1, desired.style, desired.color);
                normal_dpi_ = desired.dpi;
                normal_style_ = desired.style;
                normal_color_ = desired.color;
            }
            sprite = normal_sprite_;
            sprite.position = current_;
        } else sprite = draw_sprite(current_, desired.dpi, press_progress, desired.style, desired.color);
        if (!present(sprite, desired.suppression != 0)) {
            ShowWindow(window_, SW_HIDE);
            sprite.visible = false;
        }
        {
            std::lock_guard<std::mutex> lock(shared_->mutex);
            shared_->rendered = std::move(sprite);
        }
        was_visible_ = true;
        if (progress < 1 || press_progress >= 0) SetTimer(window_, kAnimationTimer, 16, nullptr);
        else KillTimer(window_, kAnimationTimer);
    }

    std::shared_ptr<SharedState> shared_;
    HWND window_ = nullptr;
    HDC screen_ = nullptr;
    HDC memory_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ original_bitmap_ = nullptr;
    void* pixels_ = nullptr;
    int bitmap_width_ = 0, bitmap_height_ = 0;
    bool was_visible_ = false;
    POINT current_{}, from_{}, target_{};
    std::uint64_t show_sequence_ = 0, settle_sequence_ = 0;
    Clock::time_point movement_started_{}, press_started_{};
    PointerSprite normal_sprite_;
    UINT normal_dpi_ = 0;
    std::string normal_style_, normal_color_;
};

void synchronize(const std::shared_ptr<SharedState>& shared) {
    HWND window = nullptr;
    {
        std::lock_guard<std::mutex> lock(shared->mutex);
        window = shared->window;
    }
    if (!window) return;
    DWORD_PTR ignored = 0;
    // No caller stack pointer is passed: a timed-out UI message may complete
    // later, and reads its command from the shared state instead.
    if (!SendMessageTimeoutW(window, kApply, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, kCommandTimeout, &ignored))
        throw std::runtime_error("Computer Use pointer overlay did not acknowledge its visibility update.");
}
} // namespace

struct PointerOverlay::Impl {
    std::shared_ptr<SharedState> shared = std::make_shared<SharedState>();
    std::thread thread;

    Impl() {
        if (!shared->initialized || !shared->stopped || !shared->stop) return;
        thread = std::thread([state = shared] {
            try { OverlayThread overlay(state); overlay.run(); } catch (...) {}
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->window = nullptr;
                state->rendered.visible = false;
            }
            SetEvent(state->initialized);
            SetEvent(state->stopped);
        });
        if (WaitForSingleObject(shared->initialized, kCommandTimeout) != WAIT_OBJECT_0) SetEvent(shared->stop);
    }

    ~Impl() {
        if (!thread.joinable()) return;
        SetEvent(shared->stop);
        if (WaitForSingleObject(shared->stopped, kCommandTimeout) == WAIT_OBJECT_0) thread.join();
        else thread.detach(); // The thread retains all resources via shared_ptr.
    }
};

PointerOverlay::PointerOverlay() : impl_(std::make_unique<Impl>()) {}
PointerOverlay::~PointerOverlay() = default;

void PointerOverlay::configure(const std::string& style, const std::string& color) {
    const std::string normalized_style = pointer_appearance::valid_style(style) ? style : pointer_appearance::kDefaultStyle;
    const auto normalized_color = pointer_appearance::normalize_color(color).value_or(pointer_appearance::kDefaultColor);
    {
        std::lock_guard<std::mutex> lock(impl_->shared->mutex);
        auto& desired = impl_->shared->desired;
        if (desired.style == normalized_style && desired.color == normalized_color) return;
        desired.style = normalized_style;
        desired.color = normalized_color;
    }
    synchronize(impl_->shared);
}

void PointerOverlay::show(POINT target, UINT dpi, bool pressed) {
    {
        std::lock_guard<std::mutex> lock(impl_->shared->mutex);
        auto& desired = impl_->shared->desired;
        desired.visible = true;
        desired.target = target;
        desired.dpi = std::clamp(dpi, 48U, 768U);
        desired.pressed = pressed;
        ++desired.show_sequence;
    }
    synchronize(impl_->shared);
}

void PointerOverlay::hide() {
    {
        std::lock_guard<std::mutex> lock(impl_->shared->mutex);
        impl_->shared->desired.visible = false;
    }
    synchronize(impl_->shared);
}

void PointerOverlay::suppress(bool suppressed) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(impl_->shared->mutex);
        auto& count = impl_->shared->desired.suppression;
        if (suppressed && count < std::numeric_limits<unsigned>::max()) { ++count; changed = true; }
        else if (!suppressed && count) { --count; changed = true; }
    }
    try { synchronize(impl_->shared); }
    catch (...) {
        HWND window = nullptr;
        {
            std::lock_guard<std::mutex> lock(impl_->shared->mutex);
            auto& count = impl_->shared->desired.suppression;
            if (changed && suppressed && count) --count;
            window = impl_->shared->window;
        }
        // A failed acquisition gives the caller no scope to release, so undo
        // its increment. A release must remain released even after a timeout:
        // its RAII scope may already be gone and cannot retry the decrement.
        // Refresh once the UI resumes, without borrowing caller memory.
        if (window && PointerOverlay::owns_window(window)) PostMessageW(window, kApply, 0, 0);
        throw;
    }
}

void PointerOverlay::settle() {
    {
        std::lock_guard<std::mutex> lock(impl_->shared->mutex);
        ++impl_->shared->desired.settle_sequence;
    }
    synchronize(impl_->shared);
}

PointerSprite PointerOverlay::snapshot() const {
    std::lock_guard<std::mutex> lock(impl_->shared->mutex);
    return impl_->shared->rendered;
}

HWND PointerOverlay::window() const {
    std::lock_guard<std::mutex> lock(impl_->shared->mutex);
    return impl_->shared->window;
}

bool PointerOverlay::owns_window(HWND window) {
    if (!window) return false;
    DWORD process = 0;
    if (!GetWindowThreadProcessId(window, &process) || process != GetCurrentProcessId()) return false;
    {
        std::lock_guard<std::mutex> lock(registry_mutex);
        if (!registered_windows.count(window)) return false;
    }
    wchar_t name[128]{};
    return GetClassNameW(window, name, static_cast<int>(std::size(name))) && std::wcscmp(name, kClassName) == 0;
}

} // namespace acecode::computer_use
#endif
