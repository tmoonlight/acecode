// 生产 Desktop WebHost + 独立 WebView2 配置目录(由 webview_health_smoke.py 提供),
// 在真实 WebView2 上走一遍主页面的探活与恢复。单测只能守判定逻辑,下面三件事
// 只有真实控件才测得出来:
//   healthy       收到「会话解锁」后探活,正常页面的结论是 healthy,不重载;
//   nav-cancelled 一次被宿主取消的跳转(页面没有离开)之后,解锁照样探活;
//   hung          页面脚本死循环,探针没有回音,宿主结束渲染进程后页面恢复;
//   hung-user     页面脚本死循环,用户从原生入口要求重新加载,页面恢复;
//   browser-exit  浏览器主进程被结束,宿主记下控件失效;没人在窗口前时不自动恢复,
//                 用户从原生入口要求重新加载时调用恢复回调。
// 用户反馈的场景:机器长时间不关机,解锁屏幕后整个窗口只剩底色,只能重启程序。
#include "desktop/web_host.hpp"
#include "desktop/web_host_health.hpp"
#include "utils/scope_exit.hpp"

#include <windows.h>
#include <nlohmann/json.hpp>
#include <iostream>
#include <string>

namespace {

using acecode::desktop::PageHealthVerdict;
using acecode::desktop::WebHost;
namespace detail = acecode::desktop::detail;

// GUI-thread-only state of this single-window runner.
struct Run {
    std::string scenario;
    HWND window = nullptr;
    WebHost* host = nullptr;  // Borrowed from main; outlives the message loop.
    int result = 1;
    int loads = 0;
    int probes = 0;
    int recoveries = 0;
    bool recovery_was_user_initiated = false;
    PageHealthVerdict last_verdict = PageHealthVerdict::Healthy;
};

Run& run() {
    static Run state;
    return state;
}

void finish(bool ok, const std::string& note) {
    auto& state = run();
    std::cout << nlohmann::json{{"scenario", state.scenario},
                                {"ok", ok},
                                {"note", note},
                                {"loads", state.loads},
                                {"probes", state.probes},
                                {"verdict", acecode::desktop::page_health_verdict_name(
                                                state.last_verdict)},
                                {"recoveries", state.recoveries}}
                     .dump()
              << std::endl;
    state.result = ok ? 0 : 1;
    ::PostQuitMessage(0);
}

void post_session_unlock() {
    ::PostMessageW(run().window, WM_WTSSESSION_CHANGE, WTS_SESSION_UNLOCK, 0);
}

void kill_browser_process() {
    UINT32 pid = 0;
    detail::with_main_webview_core([&pid](ICoreWebView2& core) { core.get_BrowserProcessId(&pid); });
    if (pid == 0) {
        finish(false, "browser process id unavailable");
        return;
    }
    // Only the process this WebView reported; never by name.
    const HANDLE process = ::OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (!process) {
        finish(false, "OpenProcess failed");
        return;
    }
    ::TerminateProcess(process, 1);
    ::CloseHandle(process);
}

void check_browser_exit() {
    auto& state = run();
    if (!detail::main_webview_health().webview_lost) {
        finish(false, "browser exit was not noticed");
        return;
    }
    if (state.recoveries != 0) {
        finish(false, "recovered while nobody was at the window");
        return;
    }
    state.host->reload_page();
    finish(state.recoveries == 1 && state.recovery_was_user_initiated,
           "recovery handler after a user request");
}

// Cancel navigations to .../cancel-me the way the shell cancels external links:
// WebView2 reports them as failed although the page never left.
void cancel_marked_navigations() {
    detail::with_main_webview_core([](ICoreWebView2& core) {
        EventRegistrationToken token{};
        core.add_NavigationStarting(
            Microsoft::WRL::Callback<ICoreWebView2NavigationStartingEventHandler>(
                [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                    LPWSTR uri = nullptr;
                    if (FAILED(args->get_Uri(&uri)) || !uri) return S_OK;
                    const bool marked = ::wcsstr(uri, L"cancel-me") != nullptr;
                    ::CoTaskMemFree(uri);
                    if (marked) args->put_Cancel(TRUE);
                    return S_OK;
                }).Get(),
            &token);
    });
}

void on_page_ready(int loads) {
    auto& state = run();
    state.loads = loads;
    if ((state.scenario == "healthy" || state.scenario == "nav-cancelled") && loads == 1) {
        // Give the navigation time to complete: only a loaded page is probed.
        ::SetTimer(nullptr, 0, 1000, +[](HWND, UINT, UINT_PTR id, DWORD) {
            ::KillTimer(nullptr, id);
            post_session_unlock();
        });
    } else if (state.scenario == "hung" && loads == 1) {
        // The page starts spinning 600 ms after it reported in.
        ::SetTimer(nullptr, 0, 1500, +[](HWND, UINT, UINT_PTR id, DWORD) {
            ::KillTimer(nullptr, id);
            post_session_unlock();
        });
    } else if (state.scenario == "hung-user" && loads == 1) {
        ::SetTimer(nullptr, 0, 1500, +[](HWND, UINT, UINT_PTR id, DWORD) {
            ::KillTimer(nullptr, id);
            if (!run().host->reload_page()) finish(false, "reload_page refused");
        });
    } else if (state.scenario == "hung-user" && loads == 2) {
        finish(state.probes == 0, "page came back after the user asked for a reload");
    } else if (state.scenario == "hung" && loads == 2) {
        finish(state.probes == 1 && state.last_verdict == PageHealthVerdict::NoReply,
               "page came back after the automatic reload");
    } else if (state.scenario == "browser-exit" && loads == 1) {
        ::SetTimer(nullptr, 0, 500, +[](HWND, UINT, UINT_PTR id, DWORD) {
            ::KillTimer(nullptr, id);
            kill_browser_process();
        });
        ::SetTimer(nullptr, 0, 4000, +[](HWND, UINT, UINT_PTR id, DWORD) {
            ::KillTimer(nullptr, id);
            check_browser_exit();
        });
    }
}

void on_probe(PageHealthVerdict verdict) {
    auto& state = run();
    ++state.probes;
    state.last_verdict = verdict;
    if (state.scenario == "healthy" || state.scenario == "nav-cancelled") {
        finish(verdict == PageHealthVerdict::Healthy, "probe after session unlock");
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    auto& state = run();
    state.scenario = argv[2];
    WebHost host(false, WebHost::StartupWindowMode::OffscreenUntilReady);
    state.host = &host;
    state.window = static_cast<HWND>(host.native_window());

    auto& health = detail::main_webview_health();
    health.timings.settle_ms = 200;
    health.timings.second_sample_ms = 500;
    health.timings.reply_timeout_ms = 4000;
    health.timings.loss_grace_ms = 300;
    health.timings.min_page_age_ms = 0;
    health.probe_observer = +[](PageHealthVerdict verdict) { on_probe(verdict); };
    host.set_webview_recovery_handler(+[](bool user_initiated) {
        ++run().recoveries;
        run().recovery_was_user_initiated = user_initiated;
    });

    cancel_marked_navigations();
    host.bind("aceHealthReady", [](const std::string& message) {
        on_page_ready(nlohmann::json::parse(message).at(0).get<int>());
        return std::string("null");
    });
    const auto timer = ::SetTimer(nullptr, 0, 45000,
        +[](HWND, UINT, UINT_PTR, DWORD) { finish(false, "timed out"); });
    if (!timer) return 3;
    acecode::ScopeExit stop_timer([timer] { ::KillTimer(nullptr, timer); });
    host.init_script(std::string("window.__aceScenario=") +
                     nlohmann::json(state.scenario).dump() + ";" + R"JS(
        addEventListener('DOMContentLoaded', () => {
            const loads = Number(localStorage.getItem('ace-health-loads') || '0') + 1;
            localStorage.setItem('ace-health-loads', String(loads));
            const root = document.createElement('div');
            root.id = 'root';
            root.appendChild(document.createElement('p'));
            document.body.appendChild(root);
            aceHealthReady(loads);
            if (window.__aceScenario === 'nav-cancelled') {
                setTimeout(() => { location.href = '/cancel-me'; }, 300);
            }
            if (window.__aceScenario.startsWith('hung') && loads === 1) {
                setTimeout(() => { for (;;) {} }, 600);
            }
        }, { once: true });
    )JS");
    host.navigate(argv[1]);
    host.run();
    return state.result;
}
