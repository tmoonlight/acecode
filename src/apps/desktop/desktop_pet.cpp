#include "desktop_pet.hpp"
#include "web_host.hpp"

#include "desktop_pet_layout.hpp"
#include "platform/native_ui/strings.hpp"

#include "utils/encoding.hpp"
#include "utils/logger.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <type_traits>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <dwmapi.h>
#  include <wrl.h>
#  include <WebView2.h>
#  include <webview/webview.h>
#  pragma comment(lib, "dwmapi.lib")

namespace acecode {
// cmake/acecode_desktop.cmake 用 acecode_bin2cpp.cmake 把
// assets/desktop_pet/agent_office_pet.html 嵌进 exe。
const unsigned char* desktop_pet_page_data();
std::size_t desktop_pet_page_size();
} // namespace acecode
#endif

namespace acecode::desktop {

#ifdef _WIN32
namespace {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
struct GdiDeleter { void operator()(HGDIOBJ value) const { if (value) ::DeleteObject(value); } };
using UniqueRegion = std::unique_ptr<std::remove_pointer_t<HRGN>, GdiDeleter>;

constexpr wchar_t kPetWindowClass[] = L"ACECodeDesktopPetWindow";
constexpr UINT kMsgStartDrag = WM_APP + 0x41;
constexpr UINT kMsgShowMenu = WM_APP + 0x42;
constexpr UINT kMsgStartResize = WM_APP + 0x43;
constexpr UINT_PTR kRevealTimer = 1;
constexpr UINT_PTR kResizeTimer = 2;
// 拖把手期间约 60Hz 轮询鼠标:不依赖鼠标捕获(按下时捕获在 WebView2 的子窗口手里)。
constexpr UINT kResizePollMs = 15;
constexpr double kZoomStep = 1.1;   // 滚轮一格 / 菜单一次
// 页面第一帧画完会发 ready;万一没收到,导航完成后最多再等这么久也把窗口挪进来。
constexpr UINT kRevealFallbackMs = 3000;
constexpr UINT kMenuDock = 1;
constexpr UINT kMenuHide = 2;
constexpr UINT kMenuZoomIn = 3;
constexpr UINT kMenuZoomOut = 4;
constexpr UINT kMenuResetSize = 5;

std::string hresult_text(HRESULT hr) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "0x%08lX", static_cast<unsigned long>(hr));
    return buffer;
}

bool desktop_pet_disabled_by_env() {
    const char* value = std::getenv("ACECODE_DESKTOP_PET");
    if (!value) return false;
    const std::string text(value);
    return text == "0" || text == "off" || text == "false" || text == "no";
}

bool chinese_ui() {
    return native_locale().rfind("zh", 0) == 0;
}

HMONITOR primary_monitor() {
    return ::MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
}

DesktopPetRect work_area_of(HMONITOR monitor) {
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!monitor || !::GetMonitorInfoW(monitor, &info)) {
        RECT work{};
        ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        info.rcWork = work;
    }
    return {info.rcWork.left, info.rcWork.top,
            info.rcWork.right - info.rcWork.left,
            info.rcWork.bottom - info.rcWork.top};
}

int monitor_dpi(HMONITOR monitor) {
    using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    if (monitor) {
        if (HMODULE shcore = ::LoadLibraryW(L"shcore.dll")) {
            auto getter = reinterpret_cast<GetDpiForMonitorFn>(
                reinterpret_cast<void*>(::GetProcAddress(shcore, "GetDpiForMonitor")));
            UINT dpi_x = 0;
            UINT dpi_y = 0;
            const bool ok = getter && getter(monitor, 0, &dpi_x, &dpi_y) == S_OK;
            ::FreeLibrary(shcore);
            if (ok && dpi_y != 0) return static_cast<int>(dpi_y);
        }
    }
    int dpi = 96;
    if (HDC screen = ::GetDC(nullptr)) {
        const int value = ::GetDeviceCaps(screen, LOGPIXELSY);
        if (value > 0) dpi = value;
        ::ReleaseDC(nullptr, screen);
    }
    return dpi;
}

std::filesystem::path pet_root_dir() {
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = ::GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
    std::filesystem::path base;
    if (length > 0 && length < MAX_PATH) {
        base = buffer;
    } else {
        base = std::filesystem::temp_directory_path();
    }
    return base / L"ACECode" / L"desktop-pet";
}

std::filesystem::path pet_user_data_dir() {
    return pet_root_dir() / L"webview2";
}

// 用户调过的大小(逻辑倍数,与 DPI 无关);0 表示没调过,用默认整数倍。
double load_logical_scale() {
    std::ifstream in(pet_root_dir() / L"settings.json");
    if (!in) return 0.0;
    const auto json = nlohmann::json::parse(in, nullptr, false);
    if (json.is_discarded() || !json.is_object() || !json.contains("scale") || !json["scale"].is_number()) return 0.0;
    const double scale = json["scale"].get<double>();
    return std::isfinite(scale) && scale > 0.0 ? scale : 0.0;
}

void save_logical_scale(double scale) {
    std::error_code ec;
    std::filesystem::create_directories(pet_root_dir(), ec);
    nlohmann::json json = nlohmann::json::object();
    if (scale > 0.0) json["scale"] = std::round(scale * 1000.0) / 1000.0;
    std::ofstream out(pet_root_dir() / L"settings.json", std::ios::trunc);
    if (out) out << json.dump();
}

// 与 tao/winit 的透明窗口同一招:空区域的 blur-behind 让 DWM 按像素 alpha 合成
// 这个窗口(区域为空所以不会真的模糊)。WebView2 背景设成全透明后,页面里
// 没画东西的地方(原来的草地)就直接透出桌面。
void enable_per_pixel_alpha(HWND hwnd) {
    UniqueRegion empty(::CreateRectRgn(0, 0, -1, -1));
    DWM_BLURBEHIND blur{};
    blur.dwFlags = DWM_BB_ENABLE | DWM_BB_BLURREGION;
    blur.fEnable = TRUE;
    blur.hRgnBlur = empty.get();
    const HRESULT hr = ::DwmEnableBlurBehindWindow(hwnd, &blur);
    if (FAILED(hr)) {
        LOG_WARN("[desktop-pet] DwmEnableBlurBehindWindow failed: " + hresult_text(hr));
    }
}

// 窗口区域只留房间轮廓:透明角落既不绘制也不接收点击,点击会落到后面的窗口。
void apply_hit_region(HWND hwnd, double scale, const std::array<double, 4>& overlay = {}) {
    const auto polygon = desktop_pet_hit_polygon(scale);
    std::vector<POINT> points;
    points.reserve(polygon.size());
    for (const auto& point : polygon) points.push_back(POINT{point.x, point.y});
    UniqueRegion region(::CreatePolygonRgn(points.data(), static_cast<int>(points.size()), WINDING));
    UniqueRegion toolbar(::CreateRectRgn(0, 0,
        static_cast<int>(std::lround(kDesktopPetSceneWidth * scale)),
        static_cast<int>(std::lround(kDesktopPetToolbarHeight * scale))));
    if (region && toolbar) ::CombineRgn(region.get(), region.get(), toolbar.get(), RGN_OR);
    if (region && overlay[2] > overlay[0] && overlay[3] > overlay[1]) {
        RECT bounds{};
        ::GetClientRect(hwnd, &bounds);
        UniqueRegion popup(::CreateRectRgn(
            static_cast<int>(overlay[0] * bounds.right), static_cast<int>(overlay[1] * bounds.bottom),
            static_cast<int>(std::ceil(overlay[2] * bounds.right)), static_cast<int>(std::ceil(overlay[3] * bounds.bottom))));
        if (popup) ::CombineRgn(region.get(), region.get(), popup.get(), RGN_OR);
    }
    // SetWindowRgn takes ownership only on success.
    if (region && ::SetWindowRgn(hwnd, region.get(), TRUE)) (void)region.release();
}

} // namespace

struct DesktopPet::Impl : std::enable_shared_from_this<DesktopPet::Impl> {
    explicit Impl(WebHost& owner) : host(owner) {}
    WebHost& host; // Required GUI host; DesktopPet is destroyed before this host.
    nlohmann::json office_snapshot = {{"follow", true}};
    bool page_ready = false;
    std::array<double, 4> hit_overlay{};
    HWND hwnd = nullptr;
    webview::detail::mswebview2::loader loader;
    ComPtr<ICoreWebView2Environment> environment;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;
    EventRegistrationToken message_token{};
    EventRegistrationToken navigation_token{};
    DesktopPetPlacement placement;
    double logical_scale = 0.0;   // 用户调过的大小,0 = 默认
    bool revealed = false;
    bool user_moved = false;
    bool closed = false;
    bool resizing = false;
    DesktopPetRect resize_start{};
    POINT resize_cursor{};

    ~Impl() { close(); }

    void bind_bridge() {
        const auto weak = weak_from_this();
        host.bind("aceDesktop_updateOffice", [weak](const std::string& request) {
            const auto self = weak.lock();
            if (!self || self->closed || request.size() > 512 * 1024) return std::string("false");
            try {
                const auto args = nlohmann::json::parse(request);
                if (!args.is_array() || args.size() != 1 || !args[0].is_string()) return std::string("false");
                const auto value = nlohmann::json::parse(args[0].get<std::string>());
                if (!value.is_object() || value.value("version", 0) != 1 ||
                    !value.contains("agents") || !value["agents"].is_array() ||
                    !value.contains("offices") || !value["offices"].is_array() || value["offices"].size() > 5) {
                    return std::string("false");
                }
                self->office_snapshot = value;
                self->publish_snapshot();
                return std::string("true");
            } catch (...) { return std::string("false"); }
        });
        host.bind("aceDesktop_getOfficeState", [weak](const std::string&) {
            const auto self = weak.lock();
            return self && !self->closed ? self->office_snapshot.dump() : std::string("{}");
        });
    }

    void publish_snapshot() {
        if (closed || !page_ready || !webview || !office_snapshot.contains("version")) return;
        webview->PostWebMessageAsJson(utf8_to_wide(office_snapshot.dump()).c_str());
    }

    void office_action(const std::wstring& message) {
        if (message.size() > 8192) return;
        const auto value = nlohmann::json::parse(wide_to_utf8(message), nullptr, false);
        if (!value.is_object()) return;
        const auto type_field = value.find("type");
        if (type_field == value.end() || !type_field->is_string()) return;
        const auto type = type_field->get<std::string>();
        if (type == "overlay") {
            hit_overlay = {};
            const auto rect = value.find("rect");
            if (rect != value.end() && rect->is_array() && rect->size() == 4) {
                for (std::size_t i = 0; i < 4; ++i) {
                    if (!(*rect)[i].is_number()) { hit_overlay = {}; break; }
                    const double coordinate = (*rect)[i].get<double>();
                    hit_overlay[i] = std::isfinite(coordinate) ? std::clamp(coordinate, 0.0, 1.0) : 0.0;
                }
            }
            RECT bounds{};
            ::GetClientRect(hwnd, &bounds);
            apply_hit_region(hwnd, bounds.right / static_cast<double>(kDesktopPetSceneWidth), hit_overlay);
            return;
        }
        if (type != "select" && type != "follow" && type != "open") return;
        host.eval("window.dispatchEvent(new CustomEvent('ace-desktop-office-action',{detail:" +
                  value.dump() + "}));");
    }

    bool start() {
        const HMONITOR monitor = primary_monitor();
        const DesktopPetRect work = work_area_of(monitor);
        logical_scale = load_logical_scale();
        placement = place_desktop_pet(work, monitor_dpi(monitor), logical_scale);

        HINSTANCE instance = ::GetModuleHandleW(nullptr);
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.hInstance = instance;
        window_class.lpfnWndProc = &Impl::window_proc;
        window_class.lpszClassName = kPetWindowClass;
        window_class.hCursor = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
        // 纯黑在 per-pixel alpha 合成下就是全透明:WebView2 首帧之前窗口什么也不显示。
        window_class.hbrBackground = static_cast<HBRUSH>(::GetStockObject(BLACK_BRUSH));
        if (!::RegisterClassExW(&window_class) &&
            ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            LOG_WARN("[desktop-pet] RegisterClassExW failed: " + std::to_string(::GetLastError()));
            return false;
        }

        // 先放在工作区外面完成 WebView2 初始化与首帧,再挪进来(同 WebHost 的
        // OffscreenUntilReady:父窗口可见时 WebView2 初始化更可靠,也不会闪出白底)。
        hwnd = ::CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
            kPetWindowClass,
            L"ACECode Desktop Pet",
            WS_POPUP,
            work.x + work.width + 10000,
            work.y + work.height + 10000,
            placement.window.width,
            placement.window.height,
            nullptr,
            nullptr,
            instance,
            this);
        if (!hwnd) {
            LOG_WARN("[desktop-pet] CreateWindowExW failed: " + std::to_string(::GetLastError()));
            return false;
        }
        enable_per_pixel_alpha(hwnd);
        apply_hit_region(hwnd, placement.scale);
        ::ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        return create_environment();
    }

    bool create_environment() {
        std::error_code ec;
        const auto user_data = pet_user_data_dir();
        std::filesystem::create_directories(user_data, ec);
        if (ec) {
            LOG_WARN("[desktop-pet] failed to create WebView2 profile directory: " + ec.message());
            return false;
        }
        const auto weak = weak_from_this();
        auto completed = Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [weak](HRESULT result, ICoreWebView2Environment* value) -> HRESULT {
                if (const auto self = weak.lock()) self->environment_ready(result, value);
                return S_OK;
            });
        const HRESULT hr = loader.create_environment_with_options(
            nullptr, user_data.wstring().c_str(), nullptr, completed.Get());
        if (FAILED(hr)) {
            LOG_WARN("[desktop-pet] failed to start WebView2 environment: " + hresult_text(hr));
            return false;
        }
        return true;
    }

    void environment_ready(HRESULT result, ICoreWebView2Environment* value) {
        if (closed) return;
        if (FAILED(result) || !value) {
            LOG_WARN("[desktop-pet] WebView2 environment failed: " + hresult_text(result));
            close();
            return;
        }
        environment = value;
        const auto weak = weak_from_this();
        const HRESULT hr = environment->CreateCoreWebView2Controller(
            hwnd,
            Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                [weak](HRESULT created, ICoreWebView2Controller* controller_value) -> HRESULT {
                    if (const auto self = weak.lock()) {
                        self->controller_ready(created, controller_value);
                    } else if (controller_value) {
                        controller_value->Close();
                    }
                    return S_OK;
                })
                .Get());
        if (FAILED(hr)) {
            LOG_WARN("[desktop-pet] CreateCoreWebView2Controller failed: " + hresult_text(hr));
            close();
        }
    }

    void controller_ready(HRESULT result, ICoreWebView2Controller* value) {
        if (closed) {
            if (value) value->Close();
            return;
        }
        if (FAILED(result) || !value) {
            LOG_WARN("[desktop-pet] WebView2 controller failed: " + hresult_text(result));
            close();
            return;
        }
        controller = value;
        ComPtr<ICoreWebView2Controller2> controller2;
        if (SUCCEEDED(controller.As(&controller2))) {
            // 全透明背景;进程级 WEBVIEW2_DEFAULT_BACKGROUND_COLOR(主窗口用的浅色)在这里被覆盖。
            controller2->put_DefaultBackgroundColor(COREWEBVIEW2_COLOR{0, 0, 0, 0});
        }
        RECT bounds{};
        ::GetClientRect(hwnd, &bounds);
        controller->put_Bounds(bounds);
        if (FAILED(controller->get_CoreWebView2(&webview)) || !webview) {
            LOG_WARN("[desktop-pet] WebView2 core unavailable");
            close();
            return;
        }
        configure_settings();

        const auto weak = weak_from_this();
        webview->add_WebMessageReceived(
            Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                [weak](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                    LPWSTR raw = nullptr;
                    if (args && SUCCEEDED(args->TryGetWebMessageAsString(&raw)) && raw) {
                        const std::wstring message(raw);
                        ::CoTaskMemFree(raw);
                        if (const auto self = weak.lock()) self->on_web_message(message);
                    }
                    return S_OK;
                })
                .Get(),
            &message_token);
        webview->add_NavigationCompleted(
            Callback<ICoreWebView2NavigationCompletedEventHandler>(
                [weak](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                    BOOL success = FALSE;
                    if (args) args->get_IsSuccess(&success);
                    if (const auto self = weak.lock()) self->navigation_completed(success != FALSE);
                    return S_OK;
                })
                .Get(),
            &navigation_token);

        const std::string page(reinterpret_cast<const char*>(acecode::desktop_pet_page_data()),
                               acecode::desktop_pet_page_size());
        const HRESULT hr = webview->NavigateToString(utf8_to_wide(page).c_str());
        if (FAILED(hr)) {
            LOG_WARN("[desktop-pet] NavigateToString failed: " + hresult_text(hr));
            close();
        }
    }

    void configure_settings() {
        ComPtr<ICoreWebView2Settings> settings;
        if (FAILED(webview->get_Settings(&settings)) || !settings) return;
        settings->put_AreDefaultContextMenusEnabled(FALSE);
        settings->put_AreDevToolsEnabled(FALSE);
        settings->put_IsStatusBarEnabled(FALSE);
        // Ctrl+滚轮缩放会打破整数倍像素对齐。
        settings->put_IsZoomControlEnabled(FALSE);
        ComPtr<ICoreWebView2Settings3> settings3;
        if (SUCCEEDED(settings.As(&settings3))) {
            settings3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
        }
        ComPtr<ICoreWebView2Settings5> settings5;
        if (SUCCEEDED(settings.As(&settings5))) settings5->put_IsPinchZoomEnabled(FALSE);
        ComPtr<ICoreWebView2Settings6> settings6;
        if (SUCCEEDED(settings.As(&settings6))) settings6->put_IsSwipeNavigationEnabled(FALSE);
    }

    void navigation_completed(bool success) {
        if (closed || revealed) return;
        if (!success) {
            LOG_WARN("[desktop-pet] page navigation failed");
            close();
            return;
        }
        ::SetTimer(hwnd, kRevealTimer, kRevealFallbackMs, nullptr);
    }

    void on_web_message(const std::wstring& message) {
        if (closed || !hwnd) return;
        if (message == L"ready") {
            page_ready = true;
            reveal();
            publish_snapshot();
        } else if (!message.empty() && message.front() == L'{') {
            office_action(message);
        } else if (message == L"drag") {
            // 回调里不直接进模态拖动循环,回到窗口过程再开始。
            ::PostMessageW(hwnd, kMsgStartDrag, 0, 0);
        } else if (message == L"menu") {
            ::PostMessageW(hwnd, kMsgShowMenu, 0, 0);
        } else if (message == L"resize") {
            ::PostMessageW(hwnd, kMsgStartResize, 0, 0);
        } else if (message == L"size-reset") {
            reset_size();
        } else if (message.rfind(L"zoom ", 0) == 0) {
            int steps = 0;
            double fx = 1.0, fy = 1.0;
            if (::swscanf_s(message.c_str() + 5, L"%d %lf %lf", &steps, &fx, &fy) >= 1) zoom_by(steps, fx, fy);
        }
    }

    void reveal() {
        if (revealed || closed || !hwnd) return;
        revealed = true;
        ::KillTimer(hwnd, kRevealTimer);
        apply_placement(placement);
        LOG_INFO("[desktop-pet] shown at " + std::to_string(placement.window.x) + "," +
                 std::to_string(placement.window.y) + " size " +
                 std::to_string(placement.window.width) + "x" +
                 std::to_string(placement.window.height) + " scale " +
                 std::to_string(placement.scale));
    }

    void apply_placement(const DesktopPetPlacement& next) {
        placement = next;
        // 尺寸变了会同步收到 WM_SIZE,在那里更新 WebView2 边界和命中区域。
        ::SetWindowPos(hwnd, HWND_TOPMOST, next.window.x, next.window.y,
                       next.window.width, next.window.height,
                       SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    int window_dpi() const { return monitor_dpi(::MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY)); }
    DesktopPetRect window_work_area() const { return work_area_of(::MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY)); }
    DesktopPetRect window_rect() const {
        RECT r{};
        ::GetWindowRect(hwnd, &r);
        return {r.left, r.top, r.right - r.left, r.bottom - r.top};
    }

    // 缩放后告诉页面当前是默认大小的百分之几,页面闪一下提示。
    void notify_size(int dpi, const DesktopPetRect& work) {
        if (!webview) return;
        const double base = desktop_pet_scale(dpi, work.width, work.height);
        const int percent = static_cast<int>(std::lround(placement.scale / base * 100.0));
        webview->PostWebMessageAsString((L"size " + std::to_wstring(percent)).c_str());
    }

    void apply_scale(double device_scale, double fx, double fy) {
        const int dpi = window_dpi();
        const DesktopPetRect work = window_work_area();
        const double scale = clamp_desktop_pet_scale(device_scale, dpi, work);
        apply_placement(scale_desktop_pet(window_rect(), scale, fx, fy, work));
        logical_scale = scale * 96.0 / dpi;
        notify_size(dpi, work);
    }

    void zoom_by(int steps, double fx, double fy) {
        if (!revealed || closed || steps == 0) return;
        const double current = window_rect().width / static_cast<double>(kDesktopPetSceneWidth);
        // 停在右下角时锚在右下角(缩放后还贴着角);拖动过之后以鼠标所在点为中心。
        if (!user_moved) { fx = 1.0; fy = 1.0; }
        apply_scale(current * std::pow(kZoomStep, steps), fx, fy);
        save_logical_scale(logical_scale);
    }

    void start_resize() {
        if (!revealed || closed || resizing) return;
        resizing = true;
        resize_start = window_rect();
        ::GetCursorPos(&resize_cursor);
        ::SetTimer(hwnd, kResizeTimer, kResizePollMs, nullptr);
    }

    void resize_tick() {
        const int button = ::GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON;
        if (!(::GetAsyncKeyState(button) & 0x8000)) {
            finish_resize();
            return;
        }
        POINT cursor{};
        ::GetCursorPos(&cursor);
        // 把手在左上方:往左上拖变大、右下角不动;横竖两个方向取放大得多的那个。
        const double by_x = resize_start.width - (cursor.x - resize_cursor.x);
        const double by_y = (resize_start.height - (cursor.y - resize_cursor.y)) *
                            kDesktopPetSceneWidth / static_cast<double>(kDesktopPetSceneHeight);
        const int dpi = window_dpi();
        const DesktopPetRect work = window_work_area();
        const double scale = clamp_desktop_pet_scale(std::max(by_x, by_y) / kDesktopPetSceneWidth, dpi, work);
        const auto next = scale_desktop_pet(resize_start, scale, 1.0, 1.0, work);
        if (next.window.width == placement.window.width && next.window.x == placement.window.x &&
            next.window.y == placement.window.y) return;
        apply_placement(next);
        logical_scale = scale * 96.0 / dpi;
        notify_size(dpi, work);
    }

    void finish_resize() {
        ::KillTimer(hwnd, kResizeTimer);
        resizing = false;
        save_logical_scale(logical_scale);
    }

    void reset_size() {
        if (!revealed || closed) return;
        logical_scale = 0.0;
        save_logical_scale(0.0);
        const int dpi = window_dpi();
        const DesktopPetRect work = window_work_area();
        if (!user_moved) dock_to_corner();
        else apply_placement(scale_desktop_pet(window_rect(), desktop_pet_scale(dpi, work.width, work.height), 1.0, 1.0, work));
        notify_size(dpi, work);
    }

    void dock_to_corner() {
        const HMONITOR monitor = revealed
            ? ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY)
            : primary_monitor();
        apply_placement(place_desktop_pet(work_area_of(monitor), monitor_dpi(monitor), logical_scale));
    }

    void start_drag() {
        if (!hwnd) return;
        // 与主窗口的 WebHost::start_window_drag 相同:放开 WebView2 子窗口的鼠标捕获,
        // 交给系统的标题栏拖动循环,按钮松开时循环自己结束。
        ::ReleaseCapture();
        ::SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
    }

    void show_menu() {
        if (!hwnd) return;
        HMENU menu = ::CreatePopupMenu();
        if (!menu) return;
        const bool zh = chinese_ui();
        ::AppendMenuW(menu, MF_STRING, kMenuDock, zh ? L"回到右下角" : L"Move to corner");
        ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(menu, MF_STRING, kMenuZoomIn, zh ? L"放大（也可以滚轮）" : L"Zoom in (or scroll)");
        ::AppendMenuW(menu, MF_STRING, kMenuZoomOut, zh ? L"缩小" : L"Zoom out");
        ::AppendMenuW(menu, MF_STRING, kMenuResetSize, zh ? L"恢复默认大小" : L"Reset size");
        ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(menu, MF_STRING, kMenuHide, zh ? L"隐藏桌面宠物" : L"Hide desktop pet");
        POINT cursor{};
        ::GetCursorPos(&cursor);
        // KB135788:弹菜单前把宿主设为前台,菜单后补一条消息,点空白处菜单才会消失。
        ::SetForegroundWindow(hwnd);
        const UINT command = static_cast<UINT>(::TrackPopupMenuEx(
            menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, cursor.x, cursor.y, hwnd, nullptr));
        ::PostMessageW(hwnd, WM_NULL, 0, 0);
        ::DestroyMenu(menu);
        if (command == kMenuDock) {
            user_moved = false;
            dock_to_corner();
        } else if (command == kMenuZoomIn) {
            zoom_by(1, 0.5, 0.5);
        } else if (command == kMenuZoomOut) {
            zoom_by(-1, 0.5, 0.5);
        } else if (command == kMenuResetSize) {
            reset_size();
        } else if (command == kMenuHide) {
            LOG_INFO("[desktop-pet] hidden from context menu");
            close();
        }
    }

    void handle_dpi_changed(int dpi) {
        const HMONITOR monitor = ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
        const DesktopPetRect work = work_area_of(monitor);
        if (!revealed) {
            // 还在屏幕外:只换尺寸,位置等 reveal 时按主屏算。
            const DesktopPetPlacement next = place_desktop_pet(work_area_of(primary_monitor()), dpi, logical_scale);
            placement.scale = next.scale;
            placement.window.width = next.window.width;
            placement.window.height = next.window.height;
            RECT current{};
            ::GetWindowRect(hwnd, &current);
            ::SetWindowPos(hwnd, nullptr, current.left, current.top, next.window.width,
                           next.window.height, SWP_NOACTIVATE | SWP_NOZORDER);
            return;
        }
        if (user_moved) {
            RECT current{};
            ::GetWindowRect(hwnd, &current);
            apply_placement(resize_desktop_pet(
                {current.left, current.top, current.right - current.left,
                 current.bottom - current.top},
                dpi, work, logical_scale));
        } else {
            apply_placement(place_desktop_pet(work, dpi, logical_scale));
        }
    }

    LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_SIZE: {
            RECT bounds{};
            ::GetClientRect(hwnd, &bounds);
            if (controller) controller->put_Bounds(bounds);
            if (bounds.right > 0) apply_hit_region(hwnd, bounds.right / static_cast<double>(kDesktopPetSceneWidth), hit_overlay);
            break;
        }
        case WM_MOVE:
            if (controller) controller->NotifyParentWindowPositionChanged();
            break;
        case WM_EXITSIZEMOVE:
            if (revealed) user_moved = true;
            break;
        case WM_DPICHANGED:
            handle_dpi_changed(HIWORD(wparam));
            return 0;
        case WM_DISPLAYCHANGE:
            if (revealed && !user_moved) dock_to_corner();
            break;
        case WM_SETTINGCHANGE:
            if (wparam == SPI_SETWORKAREA && revealed && !user_moved) dock_to_corner();
            break;
        case WM_TIMER:
            if (wparam == kResizeTimer) {
                resize_tick();
                return 0;
            }
            if (wparam == kRevealTimer) {
                ::KillTimer(hwnd, kRevealTimer);
                LOG_WARN("[desktop-pet] page ready signal missing; revealing after timeout");
                reveal();
                return 0;
            }
            break;
        case kMsgStartDrag:
            start_drag();
            return 0;
        case kMsgShowMenu:
            show_menu();
            return 0;
        case kMsgStartResize:
            start_resize();
            return 0;
        default:
            break;
        }
        return ::DefWindowProcW(hwnd, message, wparam, lparam);
    }

    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            auto* self = static_cast<Impl*>(create->lpCreateParams);
            ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            if (self) self->hwnd = window;
        }
        auto* self = reinterpret_cast<Impl*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
        if (!self || self->hwnd != window) return ::DefWindowProcW(window, message, wparam, lparam);
        return self->handle_message(message, wparam, lparam);
    }

    void close() {
        if (closed) return;
        closed = true;
        if (webview) {
            webview->remove_WebMessageReceived(message_token);
            webview->remove_NavigationCompleted(navigation_token);
        }
        if (controller) controller->Close();
        webview.Reset();
        controller.Reset();
        environment.Reset();
        if (hwnd) {
            HWND window = hwnd;
            hwnd = nullptr;
            ::SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            ::DestroyWindow(window);
        }
    }
};
#endif

DesktopPet::DesktopPet(WebHost& host) {
#ifdef _WIN32
    if (desktop_pet_disabled_by_env()) {
        LOG_INFO("[desktop-pet] disabled by ACECODE_DESKTOP_PET");
        return;
    }
    auto impl = std::make_shared<Impl>(host);
    impl->bind_bridge();
    if (impl->start()) {
        impl_ = std::move(impl);
    } else {
        impl->close();
    }
#else
    (void)host;
#endif
}

DesktopPet::~DesktopPet() {
#ifdef _WIN32
    if (impl_) impl_->close();
#endif
}

} // namespace acecode::desktop
