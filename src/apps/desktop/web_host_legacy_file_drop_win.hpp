#pragma once

#ifdef _WIN32
#include "web_host.hpp"
#include "web_host_browser_win.hpp"
#include "web_host_file_drop_win.hpp"

namespace acecode::desktop::detail {

inline std::optional<WebHost::FileDropLocation> win_file_drop_location(HWND widget, POINTL screen) {
    RECT bounds{};
    POINT point{screen.x, screen.y};
    if (!::GetClientRect(widget, &bounds) || !::ScreenToClient(widget, &point) ||
        !::PtInRect(&bounds, point) || bounds.right <= 0 || bounds.bottom <= 0) return {};
    return WebHost::FileDropLocation{
        static_cast<double>(point.x) / bounds.right,
        static_cast<double>(point.y) / bounds.bottom};
}

inline std::unique_ptr<LegacyWindowsFileDrop> install_legacy_windows_file_drop(
    webview::webview& host, LifetimeToken& lifetime, WebHost::FileDropHandler handler) {
    auto controller_result = host.browser_controller();
    auto widget_result = host.widget();
    if (!controller_result.ok() || !widget_result.ok() || !widget_result.value()) return {};
    auto* controller = static_cast<ICoreWebView2Controller*>(controller_result.value());
    Microsoft::WRL::ComPtr<ICoreWebView2> core;
    if (!controller || FAILED(controller->get_CoreWebView2(core.GetAddressOf()))) return {};
    Microsoft::WRL::ComPtr<ICoreWebView2_2> core2;
    Microsoft::WRL::ComPtr<ICoreWebView2Environment> environment;
    if (FAILED(core.As(&core2)) ||
        FAILED(core2->get_Environment(environment.GetAddressOf()))) return {};
    LPWSTR raw_version = nullptr;
    const HRESULT version_hr = environment->get_BrowserVersionString(&raw_version);
    const std::unique_ptr<wchar_t, decltype(&::CoTaskMemFree)> version(
        raw_version, ::CoTaskMemFree);
    if (FAILED(version_hr) || !version ||
        !needs_legacy_windows_file_drop(version.get())) return {};

    const HWND widget = static_cast<HWND>(widget_result.value());
    const auto host_ref = lifetime.ref(host);
    auto legacy = std::make_unique<LegacyWindowsFileDrop>(widget,
        [widget, handler = std::move(handler)](std::vector<std::string> paths, POINTL screen_point) {
            if (!handler) return;
            WebHost::FileDropContext context;
            context.coordinate_required = true;
            context.location = win_file_drop_location(widget, screen_point);
            handler(std::move(paths), context);
        },
        [widget, host_ref](std::optional<POINTL> point) {
            const auto location = point ? win_file_drop_location(widget, *point) : std::nullopt;
            const std::string payload = location
                ? "{xRatio:" + std::to_string(location->x_ratio) +
                    ",yRatio:" + std::to_string(location->y_ratio) + "}"
                : "null";
            host_ref.with([payload](webview::webview& target) {
                target.eval("if(typeof window.__aceRouteNativeFileDrag==='function')"
                    "window.__aceRouteNativeFileDrag(" + payload + ");");
            });
        });
    if (!legacy->ready()) {
        LOG_WARN("[file-drop] Windows legacy OLE bridge initialization failed");
        return {};
    }
    const auto receiver = lifetime.ref(*legacy);
    EventRegistrationToken token{};
    const HRESULT navigation_hr = core->add_NavigationCompleted(
        Microsoft::WRL::Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [receiver](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                receiver.with([](LegacyWindowsFileDrop& target) { target.refresh(); });
                return S_OK;
            }).Get(), &token);
    if (FAILED(navigation_hr)) {
        log_webview_setting_failure("install legacy file drop navigation handler", navigation_hr);
    }
    legacy->refresh();
    LOG_INFO("[file-drop] Windows legacy OLE bridge installed");
    return legacy;
}

} // namespace acecode::desktop::detail
#endif
