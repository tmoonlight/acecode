#pragma once

// 主窗口 WebView 的故障观察与恢复接线(Windows;其它平台是空实现)。
//
// 判定逻辑都在 webview_health.hpp / renderer_recovery.hpp,这里只做三件事:
//   1. 把宿主窗口收到的会话 / 电源 / 关机消息记进日志,并在解锁、远程重连、
//      休眠唤醒后探一次页面(observe_host_window_message);
//   2. 按探活结论处置:页面没回音或一片空白就重新加载,控件已失效就走恢复;
//   3. 浏览器主进程退出后,等用户回到窗口再调用恢复回调(由 main 重启程序)。
//
// 全部状态只在 GUI 线程上读写:窗口消息、WebView2 事件回调、托盘菜单回调都在
// 这个线程。定时用宿主窗口的 SetTimer,不另起线程。

#include <functional>
#include <utility>

#ifdef _WIN32
#include "renderer_recovery.hpp"
#include "utils/encoding.hpp"
#include "utils/lifetime_token.hpp"
#include "utils/logger.hpp"
#include "webview_health.hpp"
#include <webview/webview.h>

#include <WebView2.h>
#include <wrl.h>
#include <wtsapi32.h>
#include <chrono>
#include <cstdint>
#include <string>

#pragma comment(lib, "wtsapi32.lib")

namespace acecode::desktop::detail {

// 宿主窗口上的定时器编号。web_host.cpp 与 webview 库都不在这个窗口上用定时器。
inline constexpr UINT_PTR kHealthProbeTimer = 0xACE1;     // 到点执行一次探针脚本
inline constexpr UINT_PTR kHealthReplyTimer = 0xACE2;     // 等探针回音的超时
inline constexpr UINT_PTR kHealthRecoveryTimer = 0xACE3;  // 重新评估「控件已失效」

struct MainWebviewHealthTimings {
    UINT settle_ms = 3000;          // 解锁后先等桌面稳定,再探
    UINT second_sample_ms = 2000;   // 两次采样的间隔
    // 没回音多久算卡死。解锁后的头几十秒系统可能在换页、页面可能在集中回收内存,
    // 给得太短会把忙碌的页面误判成卡死。
    UINT reply_timeout_ms = 25000;
    // 浏览器主进程退出后先等这么久再处置:注销 / 关机时它可能比我们先被结束,
    // 留出时间让关机消息到达。
    UINT loss_grace_ms = 5000;
    // 等用户回到窗口期间多久重新看一次。锁屏前就是活动窗口的话,解锁后不一定再收到
    // 激活消息,前台窗口也未必立刻恢复,不能只靠消息触发。定时器只在控件已失效时存在。
    UINT loss_recheck_ms = 2000;
    std::int64_t min_page_age_ms = kMinimumProbedPageAgeMs;
};

enum class ProbeStep { Idle, Settling, AwaitingFirst, BetweenSamples, AwaitingSecond };

struct MainWebviewHealth {
    HWND host = nullptr;
    LifetimeRef<webview::webview> view;
    MainWebviewHealthTimings timings;
    RendererRecoveryTracker reloads{};  // 所有自动重载共用一份限次
    bool session_notifications = false;
    bool session_locked = false;
    bool session_disconnected = false;
    bool session_ending = false;
    bool page_loaded = false;
    std::int64_t page_loaded_at_ms = 0;
    ProbeStep step = ProbeStep::Idle;
    unsigned probe_round = 0;  // 每轮递增,迟到的回音凭它作废
    HostWakeEvent probe_reason = HostWakeEvent::SessionUnlock;
    PageProbeSample first_sample;
    bool webview_lost = false;
    std::int64_t webview_lost_at_ms = 0;
    bool recovery_requested = false;
    int last_logged_loss_action = -1;
    std::function<void(bool user_initiated)> recovery_handler;
    // 测试观察点:每轮探活下结论时调用(tests/desktop/helpers/webview_health_smoke.cpp)。
    std::function<void(PageHealthVerdict)> probe_observer;
};

inline MainWebviewHealth& main_webview_health() {
    static MainWebviewHealth state;
    return state;
}

inline std::int64_t health_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline std::string health_hresult(HRESULT hr) {
    return "hr=" + std::to_string(static_cast<long>(hr));
}

inline const char* health_window_label(HWND host) {
    if (!::IsWindowVisible(host)) return "hidden";
    return ::IsIconic(host) ? "minimized" : "visible";
}

// 取主页面的 ICoreWebView2 并在其上执行 fn。WebHost 已析构或控件不在时返回 false。
template <typename Fn>
bool with_main_webview_core(Fn&& fn) {
    bool reached = false;
    main_webview_health().view.with([&fn, &reached](webview::webview& host) {
        auto controller_result = host.browser_controller();
        if (!controller_result.ok() || !controller_result.value()) return;
        auto* controller = static_cast<ICoreWebView2Controller*>(controller_result.value());
        Microsoft::WRL::ComPtr<ICoreWebView2> core;
        if (FAILED(controller->get_CoreWebView2(&core)) || !core) return;
        fn(*core.Get());
        reached = true;
    });
    return reached;
}

inline void evaluate_webview_loss(bool user_initiated);

inline void stop_page_probe() {
    auto& state = main_webview_health();
    state.step = ProbeStep::Idle;
    if (!state.host) return;
    ::KillTimer(state.host, kHealthProbeTimer);
    ::KillTimer(state.host, kHealthReplyTimer);
}

// 记下「控件已失效」。处置推迟到宽限期之后,见 MainWebviewHealthTimings::loss_grace_ms。
inline void note_webview_lost() {
    auto& state = main_webview_health();
    if (state.webview_lost) return;
    state.webview_lost = true;
    state.webview_lost_at_ms = health_now_ms();
    stop_page_probe();
    if (state.host) ::SetTimer(state.host, kHealthRecoveryTimer, state.timings.loss_grace_ms, nullptr);
}

// 重新加载主页面。调用失败说明控件已经失效,转入恢复流程。
inline bool reload_main_page(const std::string& why) {
    HRESULT hr = E_FAIL;
    const bool reached = with_main_webview_core(
        [&hr](ICoreWebView2& core) { hr = core.Reload(); });
    if (reached && SUCCEEDED(hr)) {
        LOG_WARN("[desktop] reloading the page: " + why);
        return true;
    }
    LOG_ERROR("[desktop] page reload failed (" + why + ", " +
              (reached ? health_hresult(hr) : std::string("webview unavailable")) + ")");
    note_webview_lost();
    return false;
}

inline void reload_main_page_within_budget(const std::string& why) {
    auto& state = main_webview_health();
    if (!state.reloads.admit_reload(health_now_ms())) {
        LOG_ERROR("[desktop] page needs a reload (" + why +
                  ") but the automatic reload limit is reached");
        return;
    }
    reload_main_page(why);
}

// 结束主页面所在环境里的渲染进程,返回结束了几个。Agent Browser 与桌宠各用各的
// 用户数据目录,不在这个环境里。主 WebView 的用户数据目录按可执行文件名取,所以
// 同时开着的另一个桌面壳实例(开发时多开,或开发版与安装版并存)会共用同一个环境,
// 它的页面也会跟着重新加载一次。
inline int terminate_main_render_processes() {
    int terminated = 0;
    with_main_webview_core([&terminated](ICoreWebView2& core) {
        Microsoft::WRL::ComPtr<ICoreWebView2_2> core2;
        if (FAILED(core.QueryInterface(IID_PPV_ARGS(&core2))) || !core2) return;
        Microsoft::WRL::ComPtr<ICoreWebView2Environment> environment;
        if (FAILED(core2->get_Environment(&environment)) || !environment) return;
        Microsoft::WRL::ComPtr<ICoreWebView2Environment8> environment8;
        if (FAILED(environment.As(&environment8)) || !environment8) return;
        Microsoft::WRL::ComPtr<ICoreWebView2ProcessInfoCollection> processes;
        if (FAILED(environment8->GetProcessInfos(&processes)) || !processes) return;
        UINT count = 0;
        processes->get_Count(&count);
        for (UINT index = 0; index < count; ++index) {
            Microsoft::WRL::ComPtr<ICoreWebView2ProcessInfo> info;
            if (FAILED(processes->GetValueAtIndex(index, &info)) || !info) continue;
            COREWEBVIEW2_PROCESS_KIND kind = COREWEBVIEW2_PROCESS_KIND_BROWSER;
            INT32 pid = 0;
            if (FAILED(info->get_Kind(&kind)) || kind != COREWEBVIEW2_PROCESS_KIND_RENDERER) continue;
            if (FAILED(info->get_ProcessId(&pid)) || pid <= 0) continue;
            const HANDLE renderer = ::OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
            if (!renderer) continue;
            if (::TerminateProcess(renderer, 1)) ++terminated;
            ::CloseHandle(renderer);
        }
    });
    return terminated;
}

// 页面没有回音时的处置。卡死的渲染进程不响应 Reload,所以先结束它:WebView2 随后
// 上报「渲染进程退出」,由 install_renderer_recovery_handler 按限次重新加载。
inline void restart_unresponsive_renderer() {
    const int terminated = terminate_main_render_processes();
    if (terminated > 0) {
        LOG_WARN("[desktop] ended " + std::to_string(terminated) +
                 " unresponsive render process(es); the page reloads once WebView2 reports the exit");
        return;
    }
    // 列不出渲染进程(运行时太旧等)时退回普通重载,能救一部分情况。
    LOG_WARN("[desktop] render processes could not be listed; falling back to a plain reload");
    reload_main_page_within_budget("no-reply");
}

inline void conclude_page_probe(PageHealthVerdict verdict, const std::string& detail) {
    auto& state = main_webview_health();
    const std::string line =
        std::string("[desktop] page probe after ") + host_wake_event_name(state.probe_reason) +
        ": " + page_health_verdict_name(verdict) + " (" + detail +
        " window=" + (state.host ? health_window_label(state.host) : "gone") + ")";
    stop_page_probe();
    if (state.probe_observer) state.probe_observer(verdict);
    switch (action_for_page_health(verdict)) {
    case PageHealthAction::None:
        if (verdict == PageHealthVerdict::Healthy) {
            LOG_INFO(line);
        } else {
            LOG_WARN(line);
        }
        break;
    case PageHealthAction::ReloadPage:
        LOG_WARN(line);
        reload_main_page_within_budget(page_health_verdict_name(verdict));
        break;
    case PageHealthAction::RestartRenderer:
        LOG_WARN(line);
        restart_unresponsive_renderer();
        break;
    case PageHealthAction::WebviewLost:
        LOG_ERROR(line);
        note_webview_lost();
        break;
    }
}

inline void on_page_probe_reply(unsigned round, HRESULT error, const std::string& result_json) {
    auto& state = main_webview_health();
    if (round != state.probe_round || !state.host) return;
    if (state.step != ProbeStep::AwaitingFirst && state.step != ProbeStep::AwaitingSecond) return;
    ::KillTimer(state.host, kHealthReplyTimer);
    if (FAILED(error)) {
        // 脚本执行失败不是卡死的证据(探活途中页面整页跳转就会这样),而「没回音」
        // 的处置是结束渲染进程,所以这里只记一行并放弃本轮;只有超时才算没回音。
        LOG_WARN(std::string("[desktop] page probe after ") +
                 host_wake_event_name(state.probe_reason) + " abandoned: script failed " +
                 health_hresult(error));
        stop_page_probe();
        return;
    }
    const PageProbeSample sample = parse_page_probe_reply(result_json);
    if (state.step == ProbeStep::AwaitingFirst) {
        state.first_sample = sample;
        state.step = ProbeStep::BetweenSamples;
        ::SetTimer(state.host, kHealthProbeTimer, state.timings.second_sample_ms, nullptr);
        return;
    }
    const HostWindowState window{::IsWindowVisible(state.host) != FALSE,
                                 ::IsIconic(state.host) != FALSE};
    conclude_page_probe(judge_page_health(state.first_sample, sample, window),
                        describe_page_probe(state.first_sample, sample));
}

inline void run_page_probe_sample() {
    auto& state = main_webview_health();
    if (!state.host) return;
    const unsigned round = state.probe_round;
    // 先进入等待状态再发起调用:回音凭「正在等」才会被接受。
    state.step = state.step == ProbeStep::Settling ? ProbeStep::AwaitingFirst
                                                   : ProbeStep::AwaitingSecond;
    ::SetTimer(state.host, kHealthReplyTimer, state.timings.reply_timeout_ms, nullptr);
    HRESULT hr = E_FAIL;
    const bool reached = with_main_webview_core([&hr, round](ICoreWebView2& core) {
        // The WebView-owned completion handler captures only the round number.
        hr = core.ExecuteScript(
            utf8_to_wide(kPageProbeScript).c_str(),
            Microsoft::WRL::Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
                [round](HRESULT error, LPCWSTR result) -> HRESULT {
                    on_page_probe_reply(round, error,
                                        result ? wide_to_utf8(result) : std::string{});
                    return S_OK;
                }).Get());
    });
    if (!reached || FAILED(hr)) {
        conclude_page_probe(PageHealthVerdict::ControllerUnavailable,
                            reached ? health_hresult(hr) : std::string("webview unavailable"));
    }
}

// 发起一轮探活。已有一轮在跑、控件已失效、页面刚加载不久时不发起。
inline void request_page_probe(HostWakeEvent reason) {
    auto& state = main_webview_health();
    if (!state.host || state.webview_lost || state.step != ProbeStep::Idle) return;
    const std::int64_t page_age_ms = health_now_ms() - state.page_loaded_at_ms;
    if (!state.page_loaded || page_age_ms < state.timings.min_page_age_ms) {
        LOG_INFO(std::string("[desktop] page probe skipped after ") +
                 host_wake_event_name(reason) + ": no settled page yet");
        return;
    }
    state.probe_reason = reason;
    ++state.probe_round;
    state.step = ProbeStep::Settling;
    ::SetTimer(state.host, kHealthProbeTimer, state.timings.settle_ms, nullptr);
}

// 控件失效后的处置:会话正在结束就不管;用户不在窗口前就等;否则调用恢复回调。
// 用户从托盘主动要求时不看窗口状态。
inline void evaluate_webview_loss(bool user_initiated) {
    auto& state = main_webview_health();
    if (!state.webview_lost || !state.host) return;
    if (state.recovery_requested && !user_initiated) return;
    if (!user_initiated) {
        // 宽限期内被窗口激活等消息提前叫醒时,补足剩余时间再评估。
        const std::int64_t waited_ms = health_now_ms() - state.webview_lost_at_ms;
        if (waited_ms < static_cast<std::int64_t>(state.timings.loss_grace_ms)) {
            ::SetTimer(state.host, kHealthRecoveryTimer,
                       static_cast<UINT>(state.timings.loss_grace_ms - waited_ms), nullptr);
            return;
        }
    }
    WebviewLossContext context;
    context.session_ending = state.session_ending || ::GetSystemMetrics(SM_SHUTTINGDOWN) != 0;
    context.session_locked = state.session_locked || state.session_disconnected;
    context.window_visible = ::IsWindowVisible(state.host) != FALSE;
    context.window_minimized = ::IsIconic(state.host) != FALSE;
    context.window_foreground = ::GetForegroundWindow() == state.host;
    WebviewLossAction action = decide_webview_loss(context);
    if (user_initiated && action == WebviewLossAction::Defer) action = WebviewLossAction::Recover;
    if (action == WebviewLossAction::Recover && !state.recovery_handler) {
        action = WebviewLossAction::Defer;
    }
    const int action_code = static_cast<int>(action);
    const bool first_report = state.last_logged_loss_action != action_code;
    state.last_logged_loss_action = action_code;
    switch (action) {
    case WebviewLossAction::Ignore:
        if (first_report) LOG_INFO("[desktop] WebView is gone while the session is ending; leaving it");
        break;
    case WebviewLossAction::Defer:
        if (state.recovery_handler) {
            ::SetTimer(state.host, kHealthRecoveryTimer, state.timings.loss_recheck_ms, nullptr);
        }
        if (first_report) {
            LOG_WARN(std::string("[desktop] WebView is gone; recovery waits for the user (") +
                     "locked=" + (state.session_locked ? "1" : "0") +
                     " disconnected=" + (state.session_disconnected ? "1" : "0") +
                     " window=" + health_window_label(state.host) +
                     " foreground=" + (context.window_foreground ? "1" : "0") +
                     " handler=" + (state.recovery_handler ? "1" : "0") + ")");
        }
        break;
    case WebviewLossAction::Recover: {
        state.recovery_requested = true;
        LOG_WARN(std::string("[desktop] WebView is gone; requesting recovery (") +
                 (user_initiated ? "user request" : "user is at the window") + ")");
        // Copy first: the handler may tear the host down.
        const auto handler = state.recovery_handler;
        handler(user_initiated);
        break;
    }
    }
}

inline void schedule_webview_loss_evaluation() {
    auto& state = main_webview_health();
    if (state.webview_lost && state.host) ::SetTimer(state.host, kHealthRecoveryTimer, 300, nullptr);
}

inline void set_main_webview_recovery_handler(std::function<void(bool)> handler) {
    main_webview_health().recovery_handler = std::move(handler);
}

// 用户从原生入口(托盘菜单)要求重新加载界面。用户点它说明页面已经不对劲,而卡死的
// 页面不响应 Reload,所以控件还在时直接结束渲染进程,由「渲染进程退出」那条路径重新
// 加载;列不出渲染进程才退回普通重载。控件已失效就走恢复。用户明确要求的不受自动
// 重载的限次约束。
inline bool reload_main_page_for_user() {
    auto& state = main_webview_health();
    if (!state.host) return false;
    if (!state.webview_lost) {
        state.reloads = RendererRecoveryTracker{};
        stop_page_probe();
        const int terminated = terminate_main_render_processes();
        if (terminated > 0) {
            LOG_WARN("[desktop] user asked to reload the page: ended " +
                     std::to_string(terminated) + " render process(es)");
            return true;
        }
        if (reload_main_page("user request")) return true;
    }
    // reload_main_page 失败时已记下控件失效;用户明确要求,不再等宽限期。
    ::KillTimer(state.host, kHealthRecoveryTimer);
    evaluate_webview_loss(/*user_initiated=*/true);
    return state.recovery_requested;
}

inline void forget_main_webview_health(HWND hwnd) {
    auto& state = main_webview_health();
    ::KillTimer(hwnd, kHealthProbeTimer);
    ::KillTimer(hwnd, kHealthReplyTimer);
    ::KillTimer(hwnd, kHealthRecoveryTimer);
    if (state.session_notifications) ::WTSUnRegisterSessionNotification(hwnd);
    state = MainWebviewHealth{};
}

// host_window_proc 的第一行调用它:只看不拦,消息照常往下走。
inline void observe_host_window_message(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    auto& state = main_webview_health();
    if (!state.host || hwnd != state.host) return;
    switch (msg) {
    case WM_TIMER:
        if (wparam == kHealthProbeTimer) {
            ::KillTimer(hwnd, kHealthProbeTimer);
            if (state.step == ProbeStep::Settling || state.step == ProbeStep::BetweenSamples) {
                run_page_probe_sample();
            }
        } else if (wparam == kHealthReplyTimer) {
            ::KillTimer(hwnd, kHealthReplyTimer);
            if (state.step == ProbeStep::AwaitingFirst || state.step == ProbeStep::AwaitingSecond) {
                conclude_page_probe(PageHealthVerdict::NoReply,
                                    "no reply within " +
                                        std::to_string(state.timings.reply_timeout_ms) + " ms");
            }
        } else if (wparam == kHealthRecoveryTimer) {
            ::KillTimer(hwnd, kHealthRecoveryTimer);
            evaluate_webview_loss(/*user_initiated=*/false);
        }
        break;
    case WM_WTSSESSION_CHANGE: {
        const SessionChange change = classify_session_change(static_cast<std::uint32_t>(wparam));
        LOG_INFO(std::string("[desktop] session event: ") + change.name +
                 " (code=" + std::to_string(static_cast<unsigned>(wparam)) + ")");
        if (change.locks) state.session_locked = true;
        if (change.unlocks) state.session_locked = false;
        if (change.disconnects) state.session_disconnected = true;
        if (change.connects) state.session_disconnected = false;
        if (change.wakes) {
            request_page_probe(change.wake);
            schedule_webview_loss_evaluation();
        }
        break;
    }
    case WM_POWERBROADCAST:
        if (wparam == PBT_APMSUSPEND) {
            LOG_INFO("[desktop] power event: suspend");
            // 休眠期间等回音的计时照走,醒来会把没机会作答的页面误判成卡死。
            stop_page_probe();
        } else if (wparam == PBT_APMRESUMEAUTOMATIC) {
            LOG_INFO("[desktop] power event: resume");
            request_page_probe(HostWakeEvent::PowerResume);
        }
        break;
    case WM_QUERYENDSESSION:
        state.session_ending = true;
        LOG_INFO("[desktop] session end requested");
        break;
    case WM_ENDSESSION:
        // wParam 为 FALSE 表示关机 / 注销被取消。
        state.session_ending = wparam != FALSE;
        LOG_INFO(std::string("[desktop] session end ") + (wparam != FALSE ? "confirmed" : "cancelled"));
        if (wparam == FALSE) schedule_webview_loss_evaluation();
        break;
    case WM_ACTIVATE:
        if (LOWORD(wparam) != WA_INACTIVE) schedule_webview_loss_evaluation();
        break;
    case WM_SHOWWINDOW:
        if (wparam != FALSE) schedule_webview_loss_evaluation();
        break;
    case WM_SIZE:
        if (wparam != SIZE_MINIMIZED) schedule_webview_loss_evaluation();
        break;
    case WM_DESTROY:
        forget_main_webview_health(hwnd);
        break;
    default:
        (void)lparam;
        break;
    }
}

// 页面加载完成的时刻是探活的前提。没完成的跳转只记一行、不改「已加载」:被宿主
// 取消的外链、文件拖放兜底、下载、未知协议都按失败上报,而页面并没有离开;若把它们
// 当成「页面没了」,单页应用此后再没有成功的顶层导航,探活就永久关掉了。真停在
// 错误页时探针自己会报 not-app-page。
inline HRESULT install_main_navigation_observer(ICoreWebView2* core) {
    EventRegistrationToken token{};
    return core->add_NavigationCompleted(
        Microsoft::WRL::Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                if (!args) return S_OK;
                BOOL succeeded = FALSE;
                COREWEBVIEW2_WEB_ERROR_STATUS status = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
                args->get_IsSuccess(&succeeded);
                args->get_WebErrorStatus(&status);
                auto& state = main_webview_health();
                if (succeeded) {
                    // 换了文档:帧计数从零开始,在途的一轮探活作废。
                    stop_page_probe();
                    state.page_loaded = true;
                    state.page_loaded_at_ms = health_now_ms();
                } else {
                    LOG_WARN("[desktop] main page navigation did not complete (web_error_status=" +
                             std::to_string(static_cast<int>(status)) + ")");
                }
                return S_OK;
            }).Get(),
        &token);
}

// configure_browser_defaults 调用:记下宿主窗口,注册会话通知与导航观察。
inline void install_main_webview_health(webview::webview& host,
                                        LifetimeRef<webview::webview> host_ref,
                                        ICoreWebView2* core) {
    auto& state = main_webview_health();
    const auto window = host.window();
    const auto timings = state.timings;
    auto handler = std::move(state.recovery_handler);
    auto observer = std::move(state.probe_observer);
    state = MainWebviewHealth{};
    state.timings = timings;
    state.recovery_handler = std::move(handler);
    state.probe_observer = std::move(observer);
    state.host = window.ok() ? static_cast<HWND>(window.value()) : nullptr;
    state.view = std::move(host_ref);
    if (state.host) {
        state.session_notifications =
            ::WTSRegisterSessionNotification(state.host, NOTIFY_FOR_THIS_SESSION) != FALSE;
        if (!state.session_notifications) {
            LOG_WARN("[desktop] session notifications unavailable, last_error=" +
                     std::to_string(::GetLastError()));
        }
    }
    const HRESULT hr = install_main_navigation_observer(core);
    if (FAILED(hr)) LOG_WARN("[desktop] add_NavigationCompleted failed, " + health_hresult(hr));
}

} // namespace acecode::desktop::detail

#else  // !_WIN32

namespace acecode::desktop::detail {

inline void set_main_webview_recovery_handler(std::function<void(bool)>) {}
inline bool reload_main_page_for_user() { return false; }

} // namespace acecode::desktop::detail

#endif
