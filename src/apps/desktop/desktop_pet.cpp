#include "desktop_pet.hpp"
#include "web_host.hpp"

#include "desktop_pet_layout.hpp"
#include "desktop_office_service.hpp"
#include "desktop_pet_resources.hpp"
#include "version.hpp"
#include "platform/native_ui/notifications.hpp"
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
constexpr UINT kMenuPin = 6;
constexpr UINT kMenuPetKindBase = 100;   // + index into kDesktopPetKinds

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

// 窗口区域只留房间轮廓、控制条和页面浮层(成员列表 / 提示 / 状态气泡):
// 透明角落既不绘制也不接收点击,点击会落到后面的窗口。区域同时裁剪绘制,
// 所以冒出墙顶的气泡必须由页面上报,否则会被切掉。
void apply_hit_region(HWND hwnd, double scale, const std::vector<DesktopPetOverlay>& overlays = {}, bool office = true) {
    const auto polygon = desktop_pet_hit_polygon(scale);
    std::vector<POINT> points;
    points.reserve(polygon.size());
    for (const auto& point : polygon) points.push_back(POINT{point.x, point.y});
    UniqueRegion region(office ? ::CreatePolygonRgn(points.data(), static_cast<int>(points.size()), WINDING)
                               : ::CreateRectRgn(0, 0, 0, 0));
    RECT bounds{};
    ::GetClientRect(hwnd, &bounds);
    for (const auto& overlay : overlays) {
        if (!region) break;
        UniqueRegion popup(::CreateRectRgn(
            static_cast<int>(overlay[0] * bounds.right), static_cast<int>(overlay[1] * bounds.bottom),
            static_cast<int>(std::ceil(overlay[2] * bounds.right)), static_cast<int>(std::ceil(overlay[3] * bounds.bottom))));
        if (popup) ::CombineRgn(region.get(), region.get(), popup.get(), RGN_OR);
    }
    // SetWindowRgn takes ownership only on success.
    if (region && ::SetWindowRgn(hwnd, region.get(), TRUE)) (void)region.release();
}

} // namespace

struct PetController : std::enable_shared_from_this<PetController> {
    explicit PetController(WebHost& owner) : host(owner) {}
    WebHost& host; // Required GUI host; DesktopPet is destroyed before this host.
    nlohmann::json office_snapshot = {{"follow", true}};
    std::string pet_kind = "office";
    bool page_ready = false;
    std::vector<DesktopPetOverlay> hit_overlays;
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
    bool pinned = true;
    bool drag_tracking = false;   // 系统标题栏拖动循环进行中,陪伴宠物据此做左右跑动画
    DesktopPetDragTracker drag_tracker;
    int docked_side = 0;          // 鲸鱼娘吸附在工作区左(-1)/右(+1)边,0 = 未吸附
    DesktopPetRect resize_start{};
    POINT resize_cursor{};

    ~PetController() { close(); }

    std::function<void()> on_closed;
    // 右键菜单选了别的宠物:交给服务保存并切换(服务再回调 set_pet_kind)。
    std::function<void(const std::string&)> on_pet_kind;

    void set_pet_kind(const std::string& kind) {
        if (pet_kind == kind) return;
        const bool same_page = pet_kind != "office" && kind != "office";
        pet_kind = kind;
        if (kind != "whale") docked_side = 0;   // 只有鲸鱼娘有趴边偷看的样子
        if (!webview || closed) return;
        hit_overlays.clear();
        if (same_page) publish_character();
        else {
            page_ready = false;
            load_page();
        }
    }

    void publish_character() {
        if (!closed && page_ready && webview && pet_kind != "office")
            webview->PostWebMessageAsJson(utf8_to_wide(desktop_pet_character(pet_kind).dump()).c_str());
    }

    void update_snapshot(const nlohmann::json& value) {
        office_snapshot = value;
        publish_snapshot();
    }

    void publish_snapshot() {
        if (closed || !page_ready || !webview || !office_snapshot.contains("version")) return;
        webview->PostWebMessageAsJson(utf8_to_wide(office_snapshot.dump()).c_str());
    }

    // 陪伴宠物页面按拖动方向播放左右跑(仿 Codex);办公室页面不需要。
    void publish_drag(const char* phase, const char* direction = nullptr) {
        if (closed || !page_ready || !webview || pet_kind == "office") return;
        nlohmann::json event = {{"type", "pet-drag"}, {"phase", phase}};
        if (direction) event["direction"] = direction;
        webview->PostWebMessageAsJson(utf8_to_wide(event.dump()).c_str());
    }

    void publish_dock() {
        if (closed || !page_ready || !webview || pet_kind == "office") return;
        const nlohmann::json event = {{"type", "pet-dock"},
            {"side", docked_side < 0 ? "left" : docked_side > 0 ? "right" : ""}};
        webview->PostWebMessageAsJson(utf8_to_wide(event.dump()).c_str());
    }

    // 拖动松手后:鲸鱼娘离左/右边足够近就贴到那条边,页面换成趴边偷看;
    // 其它情况把窗口收回工作区,头顶的会话卡片不会被屏幕边截掉。
    void settle_dock() {
        const DesktopPetRect work = window_work_area();
        const DesktopPetRect current = window_rect();
        docked_side = pet_kind == "whale" ? desktop_pet_dock_side(current, work) : 0;
        const DesktopPetRect next = docked_side ? dock_desktop_pet(current, work, docked_side)
                                                : keep_desktop_pet_in_work_area(current, work);
        if (next.x != current.x || next.y != current.y) apply_placement({next, placement.scale});
        publish_dock();
    }

    // 缩放后仍然贴着原来那条边。
    DesktopPetPlacement keep_docked(DesktopPetPlacement next, const DesktopPetRect& work) const {
        if (docked_side) next.window = dock_desktop_pet(next.window, work, docked_side);
        return next;
    }

    // 双击宠物把主窗口叫到前台。点击落在 WebView2 子窗口(浏览器进程)里,本进程不一定拿到
    // 前台权限:与文件拖放聚焦相同,短暂挂接当前前台线程的输入队列后再激活,随即断开。
    void activate_main_window() {
        HWND main = static_cast<HWND>(host.native_window());
        if (!main) return;
        const DWORD current_thread = ::GetCurrentThreadId();
        const DWORD foreground_thread = ::GetWindowThreadProcessId(::GetForegroundWindow(), nullptr);
        const bool attached = foreground_thread && foreground_thread != current_thread &&
            ::AttachThreadInput(current_thread, foreground_thread, TRUE) != FALSE;
        if (!activate_notification_window(main)) LOG_WARN("[desktop-pet] main window activation failed");
        if (attached) ::AttachThreadInput(current_thread, foreground_thread, FALSE);
    }

    void set_pinned(bool next) {
        if (::SetWindowPos(hwnd, next ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                           SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)) {
            pinned = next;
        } else {
            LOG_WARN("[desktop-pet] pin update failed: " + std::to_string(::GetLastError()));
        }
        publish_window_state();
    }

    void publish_window_state() {
        if (closed || !page_ready || !webview) return;
        const nlohmann::json state = {{"type", "pet-window-state"}, {"pinned", pinned}};
        webview->PostWebMessageAsJson(utf8_to_wide(state.dump()).c_str());
    }

    void office_action(const std::wstring& message) {
        if (message.size() > 16384) return;
        const auto value = nlohmann::json::parse(wide_to_utf8(message), nullptr, false);
        if (!value.is_object()) return;
        const auto type_field = value.find("type");
        if (type_field == value.end() || !type_field->is_string()) return;
        const auto type = type_field->get<std::string>();
        if (type == "pin") {
            const auto field = value.find("pinned");
            if (field == value.end() || !field->is_boolean()) return;
            set_pinned(field->get<bool>());
            return;
        }
        if (type == "close") {
            // Leave the WebView callback before destroying its controller.
            ::PostMessageW(hwnd, WM_CLOSE, 0, 0);
            return;
        }
        if (type == "overlay") {
            auto next = desktop_pet_overlays_from_message(value);
            if (next == hit_overlays) return;
            hit_overlays = std::move(next);
            RECT bounds{};
            ::GetClientRect(hwnd, &bounds);
            apply_hit_region(hwnd, bounds.right / static_cast<double>(kDesktopPetSceneWidth), hit_overlays, pet_kind == "office");
            return;
        }
        // 双击任何宠物都激活主窗口;带会话时再交给主页面切换到该会话。
        if (type == "activate" || type == "open") activate_main_window();
        // stop / read 来自陪伴宠物的会话卡片(停止该会话的回合、把完成结果标为已读)。
        if (type != "select" && type != "follow" && type != "open" && type != "stop" && type != "read") return;
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
        window_class.lpfnWndProc = &PetController::window_proc;
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
        apply_hit_region(hwnd, placement.scale, {}, pet_kind == "office");
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

        load_page();
    }

    void load_page() {
        const std::string page = desktop_pet_html(pet_kind);
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
            publish_character();
            publish_snapshot();
            publish_window_state();
            publish_dock();
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
        ::SetWindowPos(hwnd, nullptr, next.window.x, next.window.y,
                       next.window.width, next.window.height,
                       SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOZORDER);
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
        apply_placement(keep_docked(scale_desktop_pet(window_rect(), scale, fx, fy, work), work));
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
        else apply_placement(keep_docked(scale_desktop_pet(window_rect(), desktop_pet_scale(dpi, work.width, work.height), 1.0, 1.0, work), work));
        notify_size(dpi, work);
    }

    void dock_to_corner() {
        if (docked_side) {
            docked_side = 0;
            publish_dock();
        }
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
        // 二级菜单「切换宠物」;子菜单随 DestroyMenu(menu) 一起释放。
        if (HMENU kinds = ::CreatePopupMenu()) {
            for (UINT i = 0; i < kDesktopPetKinds.size(); ++i) {
                const auto& kind = kDesktopPetKinds[i];
                ::AppendMenuW(kinds, MF_STRING | (pet_kind == kind.id ? MF_CHECKED : 0u), kMenuPetKindBase + i,
                              utf8_to_wide(zh ? kind.zh : kind.en).c_str());
            }
            ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(kinds), zh ? L"切换宠物" : L"Switch pet");
            ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        }
        ::AppendMenuW(menu, MF_STRING, kMenuDock, zh ? L"回到右下角" : L"Move to corner");
        ::AppendMenuW(menu, MF_STRING | (pinned ? MF_CHECKED : 0u), kMenuPin, zh ? L"保持置顶" : L"Keep on top");
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
        if (command >= kMenuPetKindBase && command < kMenuPetKindBase + kDesktopPetKinds.size()) {
            const std::string kind = kDesktopPetKinds[command - kMenuPetKindBase].id;
            LOG_INFO("[desktop-pet] switched to " + kind + " from context menu");
            if (kind != pet_kind && on_pet_kind) on_pet_kind(kind);
        } else if (command == kMenuPin) {
            set_pinned(!pinned);
        } else if (command == kMenuDock) {
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
        case WM_CLOSE:
            close();
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_SIZE: {
            RECT bounds{};
            ::GetClientRect(hwnd, &bounds);
            if (controller) controller->put_Bounds(bounds);
            if (bounds.right > 0) apply_hit_region(hwnd, bounds.right / static_cast<double>(kDesktopPetSceneWidth), hit_overlays, pet_kind == "office");
            break;
        }
        case WM_ENTERSIZEMOVE:
            // 只有 start_drag 的标题栏拖动会进入这个循环(把手缩放用自己的计时器)。
            drag_tracking = true;
            docked_side = 0;   // 页面收到 start 就离开趴边姿势
            drag_tracker.begin(window_rect().x);
            publish_drag("start");
            break;
        case WM_MOVE:
            if (controller) controller->NotifyParentWindowPositionChanged();
            if (drag_tracking) {
                if (const int direction = drag_tracker.move(window_rect().x))
                    publish_drag("move", direction < 0 ? "left" : "right");
            }
            break;
        case WM_EXITSIZEMOVE:
            if (revealed) user_moved = true;
            if (drag_tracking) {
                drag_tracking = false;
                publish_drag("end");
                settle_dock();
            }
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
            auto* self = static_cast<PetController*>(create->lpCreateParams);
            ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            if (self) self->hwnd = window;
        }
        auto* self = reinterpret_cast<PetController*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
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
        if (on_closed) on_closed();
    }
};

struct DesktopPet::Impl : DesktopOfficeService<WebHost, PetController> {
    using DesktopOfficeService::DesktopOfficeService;
};
#endif

DesktopPet::DesktopPet(WebHost& host) {
#ifdef _WIN32
    impl_ = std::make_shared<Impl>(host, pet_root_dir() / "office.json", ACECODE_VERSION,
        !desktop_pet_disabled_by_env(), [] {
            return std::string(reinterpret_cast<const char*>(acecode::desktop_pet_page_data()),
                               acecode::desktop_pet_page_size());
        });
    impl_->bind_bridge();
#else
    (void)host;
#endif
}

DesktopPet::~DesktopPet() {
#ifdef _WIN32
    impl_.reset();
#endif
}

} // namespace acecode::desktop
