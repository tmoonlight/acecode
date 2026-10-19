#pragma once

#ifdef _WIN32
#include "microphone_permission.hpp"
#include "renderer_recovery.hpp"
#include "utils/encoding.hpp"
#include "utils/logger.hpp"
#include "utils/lifetime_token.hpp"
#include <webview/webview.h>

#include <WebView2.h>
#include <wrl.h>
#include <chrono>
#include <memory>

namespace acecode::desktop::detail {

inline HRESULT install_microphone_permission_handler(
    ICoreWebView2* core, const std::shared_ptr<std::string>& trusted_origin) {
    EventRegistrationToken token{};
    // The WebHost and its WebView callback share this GUI-thread-only origin.
    // Capture no host or COM owner; destroying the WebView releases the callback.
    return core->add_PermissionRequested(
        Microsoft::WRL::Callback<ICoreWebView2PermissionRequestedEventHandler>(
            [trusted_origin](ICoreWebView2* sender,
                             ICoreWebView2PermissionRequestedEventArgs* args) -> HRESULT {
                COREWEBVIEW2_PERMISSION_KIND kind{};
                if (FAILED(args->get_PermissionKind(&kind)) ||
                    kind != COREWEBVIEW2_PERMISSION_KIND_MICROPHONE) return S_OK;
                LPWSTR request = nullptr;
                LPWSTR current = nullptr;
                const HRESULT request_result = args->get_Uri(&request);
                const HRESULT current_result = sender->get_Source(&current);
                const std::unique_ptr<wchar_t, decltype(&::CoTaskMemFree)>
                    request_owner(request, ::CoTaskMemFree), current_owner(current, ::CoTaskMemFree);
                const bool allow = SUCCEEDED(request_result) && SUCCEEDED(current_result) &&
                    request && current && may_grant_desktop_microphone(
                        *trusted_origin, wide_to_utf8(current), wide_to_utf8(request));
                if (!allow) return S_OK;

                Microsoft::WRL::ComPtr<ICoreWebView2PermissionRequestedEventArgs3> args3;
                if (SUCCEEDED(args->QueryInterface(IID_PPV_ARGS(&args3)))) {
                    const HRESULT result = args3->put_SavesInProfile(FALSE);
                    if (FAILED(result)) return result;
                }
                return args->put_State(COREWEBVIEW2_PERMISSION_STATE_ALLOW);
            }).Get(), &token);
}

inline void log_webview_setting_failure(const char* operation, HRESULT hr) {
    LOG_WARN(std::string("[desktop] ") + operation +
             " failed, hr=" + std::to_string(static_cast<long>(hr)));
}

inline RendererFailure classify_webview2_process_failure(
    COREWEBVIEW2_PROCESS_FAILED_KIND kind) {
    switch (kind) {
    case COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED:
        return RendererFailure::RenderProcessExited;
    case COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_UNRESPONSIVE:
        return RendererFailure::RenderProcessUnresponsive;
    case COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED:
        return RendererFailure::BrowserProcessExited;
    default:
        return RendererFailure::Other;
    }
}

// 主页面渲染进程退出(挂机后内存耗尽最常见)时自动 Reload,不再停在 WebView2
// 自带的错误页等用户点刷新。判定与限次见 renderer_recovery.hpp。Reload 按
// WebView2 官方示例投递到事件处理之后执行,不在 ProcessFailed 回调里重入。
inline HRESULT install_renderer_recovery_handler(LifetimeRef<webview::webview> host_ref,
                                                 ICoreWebView2* core) {
    // Shared by the WebView-owned handler across failures; it owns no host.
    auto tracker = std::make_shared<RendererRecoveryTracker>();
    EventRegistrationToken token{};
    return core->add_ProcessFailed(
        Microsoft::WRL::Callback<ICoreWebView2ProcessFailedEventHandler>(
            [tracker, host_ref](ICoreWebView2* sender,
                                ICoreWebView2ProcessFailedEventArgs* args) -> HRESULT {
                if (!sender || !args) return S_OK;
                COREWEBVIEW2_PROCESS_FAILED_KIND kind =
                    COREWEBVIEW2_PROCESS_FAILED_KIND_UNKNOWN_PROCESS_EXITED;
                COREWEBVIEW2_PROCESS_FAILED_REASON reason =
                    COREWEBVIEW2_PROCESS_FAILED_REASON_UNEXPECTED;
                int exit_code = 0;
                args->get_ProcessFailedKind(&kind);
                Microsoft::WRL::ComPtr<ICoreWebView2ProcessFailedEventArgs2> details;
                if (SUCCEEDED(args->QueryInterface(IID_PPV_ARGS(&details))) && details) {
                    details->get_Reason(&reason);
                    details->get_ExitCode(&exit_code);
                }
                const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                const auto failure = classify_webview2_process_failure(kind);
                const auto action = tracker->on_failure(failure, now_ms);
                const std::string detail =
                    "kind=" + std::to_string(static_cast<int>(kind)) +
                    " reason=" + std::to_string(static_cast<int>(reason)) +
                    " exit=" + std::to_string(exit_code);
                switch (action) {
                case RendererRecoveryAction::Reload: {
                    LOG_WARN("[desktop] WebView render process exited (" + detail +
                             "); reloading the page");
                    Microsoft::WRL::ComPtr<ICoreWebView2> target(sender);
                    host_ref.with([target, host_ref](webview::webview& host) {
                        host.dispatch([target, host_ref] {
                            host_ref.with([target](webview::webview&) {
                                const HRESULT hr = target->Reload();
                                if (FAILED(hr)) log_webview_setting_failure("Reload after render process exit", hr);
                            });
                        });
                    });
                    break;
                }
                case RendererRecoveryAction::GiveUp:
                    LOG_ERROR("[desktop] WebView render process exited again (" + detail +
                              "); automatic reload limit reached, leaving the error page");
                    break;
                case RendererRecoveryAction::Ignore:
                    if (failure == RendererFailure::BrowserProcessExited) {
                        LOG_ERROR("[desktop] WebView browser process exited (" + detail +
                                  "); restart ACECode Desktop to recover");
                    } else {
                        LOG_WARN("[desktop] WebView process failed (" + detail + ")");
                    }
                    break;
                }
                return S_OK;
            }).Get(),
        &token);
}

inline bool set_dev_tools_enabled(ICoreWebView2& core, BOOL enabled) {
    ICoreWebView2Settings* settings = nullptr;
    HRESULT hr = core.get_Settings(&settings);
    if (FAILED(hr) || !settings) {
        log_webview_setting_failure("get_Settings for DevTools", hr);
        return false;
    }
    hr = settings->put_AreDevToolsEnabled(enabled);
    settings->Release();
    if (FAILED(hr)) {
        log_webview_setting_failure("put_AreDevToolsEnabled", hr);
        return false;
    }
    return true;
}

inline bool open_dev_tools_for_core(ICoreWebView2* core) {
    if (!core) return false;
    // webview/webview disables DevTools when constructed with debug=false.
    // Re-enable it only for ACECode's explicit F11/bridge entry point.
    if (!set_dev_tools_enabled(*core, TRUE)) return false;
    HRESULT hr = core->OpenDevToolsWindow();
    if (FAILED(hr)) {
        log_webview_setting_failure("OpenDevToolsWindow", hr);
        return false;
    }
    return true;
}

inline void install_dev_tools_shortcut(ICoreWebView2Controller* controller) {
    if (!controller) return;
    using Microsoft::WRL::Callback;
    EventRegistrationToken token{};
    HRESULT hr = controller->add_AcceleratorKeyPressed(
        Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>(
            [](ICoreWebView2Controller* sender,
               ICoreWebView2AcceleratorKeyPressedEventArgs* args) -> HRESULT {
                if (!sender || !args) return S_OK;

                COREWEBVIEW2_KEY_EVENT_KIND kind{};
                UINT virtual_key = 0;
                if (FAILED(args->get_KeyEventKind(&kind)) ||
                    FAILED(args->get_VirtualKey(&virtual_key))) return S_OK;
                if (virtual_key != VK_F11 ||
                    (kind != COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN &&
                     kind != COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN)) return S_OK;
                // Modified F11 belongs to the application; bare F11 opens DevTools.
                if ((GetKeyState(VK_CONTROL) | GetKeyState(VK_MENU) | GetKeyState(VK_SHIFT) |
                     GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) return S_OK;

                args->put_Handled(TRUE);
                ICoreWebView2* core = nullptr;
                if (SUCCEEDED(sender->get_CoreWebView2(&core)) && core) {
                    open_dev_tools_for_core(core);
                    core->Release();
                }
                return S_OK;
            })
            .Get(),
        &token);
    if (FAILED(hr)) {
        log_webview_setting_failure("add_AcceleratorKeyPressed for F11 DevTools", hr);
    }
}

inline void configure_browser_defaults(webview::webview& host,
                                const std::shared_ptr<std::string>& microphone_origin,
                                LifetimeRef<webview::webview> host_ref) {
    auto controller_result = host.browser_controller();
    if (!controller_result.ok()) {
        LOG_WARN("[desktop] browser_controller unavailable; browser defaults not configured");
        return;
    }
    auto* controller = static_cast<ICoreWebView2Controller*>(controller_result.value());
    if (!controller) return;

    HRESULT hr = controller->put_ZoomFactor(1.0);
    if (FAILED(hr)) {
        log_webview_setting_failure("put_ZoomFactor", hr);
    }

    ICoreWebView2* core = nullptr;
    hr = controller->get_CoreWebView2(&core);
    if (FAILED(hr) || !core) {
        log_webview_setting_failure("get_CoreWebView2", hr);
        return;
    }
    install_dev_tools_shortcut(controller);
    hr = install_microphone_permission_handler(core, microphone_origin);
    if (FAILED(hr)) log_webview_setting_failure("add_PermissionRequested for microphone", hr);
    hr = install_renderer_recovery_handler(host_ref, core);
    if (FAILED(hr)) log_webview_setting_failure("add_ProcessFailed for renderer recovery", hr);

    ICoreWebView2Settings* settings = nullptr;
    hr = core->get_Settings(&settings);
    core->Release();
    if (FAILED(hr) || !settings) {
        log_webview_setting_failure("get_Settings", hr);
        return;
    }

    hr = settings->put_IsZoomControlEnabled(FALSE);
    if (FAILED(hr)) {
        log_webview_setting_failure("put_IsZoomControlEnabled", hr);
    }

    ICoreWebView2Settings5* settings5 = nullptr;
    hr = settings->QueryInterface(IID_PPV_ARGS(&settings5));
    if (SUCCEEDED(hr) && settings5) {
        const HRESULT pinch_hr = settings5->put_IsPinchZoomEnabled(FALSE);
        if (FAILED(pinch_hr)) {
            log_webview_setting_failure("put_IsPinchZoomEnabled", pinch_hr);
        }
        settings5->Release();
    }

    ICoreWebView2Settings6* settings6 = nullptr;
    hr = settings->QueryInterface(IID_PPV_ARGS(&settings6));
    if (SUCCEEDED(hr) && settings6) {
        const HRESULT swipe_hr = settings6->put_IsSwipeNavigationEnabled(FALSE);
        if (FAILED(swipe_hr)) {
            log_webview_setting_failure("put_IsSwipeNavigationEnabled", swipe_hr);
        }
        settings6->Release();
    }

    settings->Release();
}

} // namespace acecode::desktop::detail
#endif
