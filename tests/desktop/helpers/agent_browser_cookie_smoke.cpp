// Agent Browser「保留 Cookie」三档的 Windows 端到端冒烟(openspec
// add-agent-browser-cookie-modes)。真实 WebView2 + 真实网络(https://example.com),
// cookie 经 Desktop 代理的 CDP Network.setCookie / getCookies 读写。
//
// 覆盖:共享 → 按会话 → 退出即清 → 共享的切档与页面重建、按会话隔离与子代理归根、
// 退出即清在关掉所有页面后仍保留(保活页)、切到退出即清时清空已存 cookie、重启后
// 持久 cookie 保留 / 退出即清丢失、启动时删除待清空 / 待删除的会话目录、分叉复制、
// 清除浏览数据只影响当前存储、运行中删除会话存储、代理 drain_store_tasks 操作。
//
// 成功输出 SMOKE_COOKIE_MODES_OK;观察项(WebView2 运行时行为)以 SMOKE_INFO 输出。
// 只在 Windows 构建(EXCLUDE_FROM_ALL),手动运行:需要能访问 example.com。

#include "desktop/agent_browser_cookie_jars.hpp"
#include "desktop/agent_browser_host.hpp"
#include "ipc/agent_browser_runtime.hpp"
#include "ipc/agent_browser_store_state.hpp"
#include "tool/agent_browser/cdp_client.hpp"
#include "utils/state_file.hpp"
#include "utils/utf8_path.hpp"

#include <nlohmann/json.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
using acecode::desktop::AgentBrowserHost;
using acecode::desktop::AgentBrowserPageOwner;
using acecode::desktop::AgentBrowserStorageEvent;

constexpr UINT kDispatchMessage = WM_APP + 72;
constexpr const char kSite[] = "https://example.com/";

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == kDispatchMessage) {
        std::unique_ptr<std::function<void()>> task(
            reinterpret_cast<std::function<void()>*>(lparam));
        if (task && *task) (*task)();
        return 0;
    }
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

bool pump_until(const std::function<bool()>& predicate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return predicate();
}

void pump_for(std::chrono::milliseconds duration) {
    pump_until([] { return false; }, duration);
}

struct SmokeFailure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

void require(bool condition, const std::string& message) {
    if (!condition) throw SmokeFailure(message);
}

void info(const std::string& key, const nlohmann::json& value) {
    std::cout << "SMOKE_INFO " << key << " " << value.dump() << '\n';
}

class Smoke {
public:
    explicit Smoke(HWND window) : window_(window) {}

    void start_host() {
        ++generation_;
        const std::string instance = "cookie-smoke-" + std::to_string(GetCurrentProcessId()) +
                                     "-" + std::to_string(generation_);
        HWND window = window_;
        host_ = std::make_unique<AgentBrowserHost>(
            window_, static_cast<std::int64_t>(GetCurrentProcessId()), instance,
            AgentBrowserHost::StateHandler{},
            [window](std::function<void()> task) {
                auto* pending = new std::function<void()>(std::move(task));
                if (!PostMessageW(window, kDispatchMessage, 0,
                                  reinterpret_cast<LPARAM>(pending))) {
                    delete pending;
                }
            });
        host_->set_storage_event_handler(
            [this](const AgentBrowserStorageEvent& event) { events_.push_back(event); });
        require(pump_until([this] {
                    const auto state = host_->state();
                    return state.ready || !state.error.empty();
                }, std::chrono::seconds(30)),
                "WebView2 host did not become ready");
        require(host_->state().ready, "host failed: " + host_->state().error);
    }

    // 模拟 Desktop 退出:销毁宿主,等 WebView2 浏览器进程释放用户数据目录
    // (lockfile 能被独占打开或已删除)。否则下一个宿主会复用仍在运行的浏览器进程,
    // 看不出「重启」的效果。
    void stop_host() {
        const fs::path lockfile = user_data() / "EBWebView" / "lockfile";
        std::error_code exists_error;
        require(fs::exists(lockfile, exists_error),
                "WebView2 lockfile not found; cannot observe browser shutdown");
        host_.reset();
        const bool released = pump_until([&lockfile] {
            std::error_code ec;
            if (!fs::exists(lockfile, ec)) return true;
            HANDLE handle = CreateFileW(lockfile.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle == INVALID_HANDLE_VALUE) return false;
            CloseHandle(handle);
            return true;
        }, std::chrono::seconds(30));
        require(released, "WebView2 browser process did not exit after host shutdown");
        pump_for(std::chrono::milliseconds(500));
    }

    AgentBrowserHost& host() { return *host_; }

    static fs::path user_data() { return acecode::desktop::agent_browser_user_data_path(); }

    static AgentBrowserPageOwner owner(const std::string& session, const std::string& root = {}) {
        AgentBrowserPageOwner value;
        value.session_id = session;
        value.workspace_hash = "smoke-ws";
        value.root_session_id = root;
        return value;
    }

    std::string open_page(const std::string& session, const std::string& root = {}) {
        std::string error;
        const std::string page = host_->create_page(&error, owner(session, root));
        require(!page.empty(), "create_page failed: " + error);
        require(host_->set_shared_with_agent(page, true, &error), "share failed: " + error);
        require(host_->set_bounds(page, {12, 12, 860, 580, true}, &error),
                "set_bounds failed: " + error);
        require(pump_until([&] { return host_->state(page).ready; }, std::chrono::seconds(30)),
                "page " + page + " did not become ready: " + host_->state(page).error);
        require(host_->navigate(page, kSite, &error), "navigate failed: " + error);
        wait_live(page);
        return page;
    }

    void wait_live(const std::string& page) {
        require(pump_until([&] {
                    const auto state = host_->state(page);
                    return state.ready && !state.loading &&
                           state.content_state == acecode::desktop::kAgentBrowserContentStateLive;
                }, std::chrono::seconds(45)),
                "page " + page + " did not load " + kSite + " (state=" +
                    host_->state(page).content_state + ", error=" +
                    host_->state(page).error + ")");
    }

    nlohmann::json cdp(const std::string& page, const std::string& method,
                       const nlohmann::json& params) {
        std::atomic<bool> done{false};
        std::string error;
        nlohmann::json result;
        std::thread agent([&] {
            acecode::agent_browser::AgentBrowserCdpClient client;
            if (client.connect(std::chrono::seconds(10), nullptr, error) &&
                client.select_page(page, std::chrono::seconds(10), nullptr, error)) {
                result = client.command(method, params, std::chrono::seconds(20), nullptr, error);
            }
            done.store(true);
        });
        const bool finished = pump_until([&done] { return done.load(); },
                                         std::chrono::seconds(40));
        agent.join();
        require(finished, method + " timed out");
        require(error.empty(), method + " failed: " + error);
        return result;
    }

    void set_cookie(const std::string& page, const std::string& name, bool persistent) {
        nlohmann::json params{{"name", name}, {"value", "1"}, {"url", kSite}};
        if (persistent) {
            params["expires"] = static_cast<double>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count() + 86400);
        }
        const auto result = cdp(page, "Network.setCookie", params);
        require(result.value("success", true), "Network.setCookie rejected " + name);
    }

    std::set<std::string> cookies(const std::string& page) {
        const auto result = cdp(page, "Network.getCookies",
                                {{"urls", nlohmann::json::array({kSite})}});
        std::set<std::string> names;
        for (const auto& cookie : result.value("cookies", nlohmann::json::array())) {
            names.insert(cookie.value("name", ""));
        }
        return names;
    }

    void switch_mode(const std::string& mode) {
        std::string error;
        require(host_->set_cookie_mode(mode, &error), "set_cookie_mode failed: " + error);
        require(host_->cookie_mode() == mode, "cookie mode did not change to " + mode);
    }

    void drain_with_proxy() {
        std::atomic<bool> done{false};
        std::string error;
        std::thread agent([&] {
            acecode::agent_browser::AgentBrowserCdpClient client;
            if (client.connect(std::chrono::seconds(10), nullptr, error)) {
                client.drain_store_tasks(std::chrono::seconds(10), nullptr, error);
            }
            done.store(true);
        });
        const bool finished = pump_until([&done] { return done.load(); },
                                         std::chrono::seconds(30));
        agent.join();
        require(finished && error.empty(), "drain_store_tasks failed: " + error);
    }

    bool wait_event(const std::string& type, const std::string& store,
                    AgentBrowserStorageEvent* out = nullptr) {
        const bool found = pump_until([&] {
            return std::any_of(events_.begin(), events_.end(), [&](const auto& event) {
                return event.type == type && (store.empty() || event.store == store);
            });
        }, std::chrono::seconds(30));
        if (found && out) {
            *out = *std::find_if(events_.begin(), events_.end(), [&](const auto& event) {
                return event.type == type && (store.empty() || event.store == store);
            });
        }
        return found;
    }

    static std::vector<std::string> profile_directories() {
        std::vector<std::string> names;
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(user_data() / "EBWebView", ec)) {
            if (entry.is_directory(ec)) names.push_back(acecode::path_to_utf8(entry.path().filename()));
        }
        std::sort(names.begin(), names.end());
        return names;
    }

    static bool profile_exists(const std::string& session) {
        std::error_code ec;
        return fs::exists(user_data() / "EBWebView" /
                              acecode::desktop::agent_browser_session_profile_directory_name(
                                  session),
                          ec);
    }

private:
    HWND window_ = nullptr;
    int generation_ = 0;
    std::unique_ptr<AgentBrowserHost> host_;
    std::vector<AgentBrowserStorageEvent> events_;
};

bool contains(const std::set<std::string>& names, const std::string& name) {
    return names.count(name) != 0;
}

bool listed(const std::vector<std::string>& values, const std::string& value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

void run(Smoke& smoke) {
    using acecode::desktop::read_agent_browser_store_state;

    // ---- 宿主 1:默认共享 → 按会话 → 退出即清 → 共享 ----
    smoke.start_host();
    require(smoke.host().cookie_mode() == "shared", "fresh install must default to shared");
    const std::string p1 = smoke.open_page("A");
    smoke.set_cookie(p1, "smk_shared", true);
    smoke.set_cookie(p1, "smk_shared_session", false);

    smoke.switch_mode("per_session");
    smoke.wait_live(p1);  // 重建后按原 page id 回到原地址
    require(smoke.host().state(p1).url.find("example.com") != std::string::npos,
            "rebuilt page did not restore its URL");
    require(!contains(smoke.cookies(p1), "smk_shared"),
            "per-session store of A must not see the shared cookie");
    smoke.set_cookie(p1, "smk_A", true);

    const std::string p2 = smoke.open_page("B");
    auto names = smoke.cookies(p2);
    require(!contains(names, "smk_A") && !contains(names, "smk_shared"),
            "session B must not see session A's or shared cookies");
    smoke.set_cookie(p2, "smk_B", true);

    const std::string p3 = smoke.open_page("A-child", "A");
    require(contains(smoke.cookies(p3), "smk_A"),
            "a sub-agent page must use its root session's store");

    const auto directories = Smoke::profile_directories();
    info("profile_directories", directories);
    require(Smoke::profile_exists("A") && Smoke::profile_exists("B"),
            "named WebView2 profiles are not at <user-data>/EBWebView/WV2Profile_<name>");

    smoke.switch_mode("until_quit");
    for (const auto& page : {p1, p2, p3}) smoke.wait_live(page);
    names = smoke.cookies(p1);
    require(!contains(names, "smk_A") && !contains(names, "smk_shared"),
            "until-quit store must start empty");
    smoke.set_cookie(p1, "smk_quit", true);
    require(pump_until([] { return !read_agent_browser_store_state().shared_wipe_pending; },
                       std::chrono::seconds(30)),
            "shared store was not cleared after switching to until_quit");
    {
        const auto state = read_agent_browser_store_state();
        require(listed(state.pending_wipe, "A") && listed(state.pending_wipe, "B"),
                "per-session stores were not marked for wipe");
        require(state.cookie_mode == "until_quit", "cookie mode was not persisted");
    }

    std::string error;
    for (const auto& page : {p1, p2, p3}) {
        require(smoke.host().close_page(page, &error), "close_page failed: " + error);
    }
    pump_for(std::chrono::seconds(3));
    const std::string p4 = smoke.open_page("A");
    const bool until_quit_survived = contains(smoke.cookies(p4), "smk_quit");
    info("until_quit_after_closing_all_pages", until_quit_survived);
    require(until_quit_survived, "until-quit cookies were lost after closing all pages");

    smoke.switch_mode("shared");
    smoke.wait_live(p4);
    names = smoke.cookies(p4);
    require(!contains(names, "smk_shared") && !contains(names, "smk_quit"),
            "switching to until_quit must have cleared the shared store");
    smoke.set_cookie(p4, "smk_persist", true);
    smoke.set_cookie(p4, "smk_session", false);
    smoke.stop_host();

    // ---- 宿主 2:启动删除待清空目录、重启后的持久 / 会话 cookie、分叉、清除、删除 ----
    smoke.start_host();
    require(!Smoke::profile_exists("A") && !Smoke::profile_exists("B"),
            "startup did not remove per-session stores marked for wipe");
    require(read_agent_browser_store_state().pending_wipe.empty(),
            "startup did not clear pending_wipe");
    const std::string p5 = smoke.open_page("X");
    names = smoke.cookies(p5);
    require(contains(names, "smk_persist"), "persistent shared cookie was lost on restart");
    info("session_cookie_after_restart", contains(names, "smk_session"));

    smoke.switch_mode("per_session");
    const std::string p6 = smoke.open_page("A");
    smoke.set_cookie(p6, "smk_A2", true);
    require(acecode::desktop::record_agent_browser_store_seed("C", "A", 1),
            "failed to record fork seed");
    const std::string p7 = smoke.open_page("C");
    require(contains(smoke.cookies(p7), "smk_A2"), "forked session was not seeded from A");
    smoke.set_cookie(p7, "smk_C", true);
    require(!contains(smoke.cookies(p6), "smk_C"), "fork copy must be independent");
    require(read_agent_browser_store_state().pending_seeds.empty(),
            "fork seed was not consumed");

    require(smoke.host().clear_browsing_data(p6, &error), "clear failed: " + error);
    AgentBrowserStorageEvent cleared;
    require(smoke.wait_event("browsing_data_cleared", "session:A", &cleared) && cleared.ok,
            "browsing data clear did not complete: " + cleared.error);
    smoke.wait_live(p6);
    require(smoke.cookies(p6).empty(), "clearing A left cookies behind");
    require(contains(smoke.cookies(p7), "smk_C"), "clearing A must not touch C");

    require(acecode::desktop::record_agent_browser_store_deletion("C"),
            "failed to record deletion");
    smoke.host().process_store_tasks();
    AgentBrowserStorageEvent deleted;
    require(smoke.wait_event("session_store_deleted", "session:C", &deleted) && deleted.ok,
            "loaded session store was not deleted: " + deleted.error);
    require(smoke.host().states("C").empty(), "pages of the deleted session were not closed");

    // 本次运行没加载过的会话存储:代理提醒后直接删目录并撤掉记录。
    const fs::path never_loaded = Smoke::user_data() / "EBWebView" /
        acecode::desktop::agent_browser_session_profile_directory_name("Z");
    fs::create_directories(never_loaded);
    std::ofstream(never_loaded / "Cookies") << "x";
    require(acecode::desktop::record_agent_browser_store_deletion("Z"),
            "failed to record deletion of Z");
    smoke.drain_with_proxy();
    require(!Smoke::profile_exists("Z"), "drain_store_tasks did not remove an unloaded store");
    require(!listed(read_agent_browser_store_state().pending_deletions, "Z"),
            "drained deletion stayed pending");

    smoke.switch_mode("until_quit");
    const std::string p8 = smoke.open_page("Q");
    smoke.set_cookie(p8, "smk_quit2", true);
    smoke.stop_host();

    // ---- 宿主 3:退出即清跨重启丢失、上次运行中删除的存储在启动时删目录 ----
    smoke.start_host();
    require(!Smoke::profile_exists("C"), "deleted session store survived restart");
    require(!listed(read_agent_browser_store_state().pending_deletions, "C"),
            "deleted session store stayed pending after restart");
    require(smoke.host().cookie_mode() == "until_quit", "persisted cookie mode was not loaded");
    const std::string p9 = smoke.open_page("Q");
    require(!contains(smoke.cookies(p9), "smk_quit2"), "until-quit cookie survived a restart");
    smoke.stop_host();
}

} // namespace

int main() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        std::cerr << "SMOKE_FAILED: CoInitializeEx failed\n";
        return 1;
    }
    wchar_t original_user_profile[32768]{};
    const DWORD original_size = GetEnvironmentVariableW(
        L"USERPROFILE", original_user_profile,
        static_cast<DWORD>(std::size(original_user_profile)));
    const fs::path isolated = fs::temp_directory_path() /
        ("acecode-agent-browser-cookie-smoke-" + std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    fs::create_directories(isolated / ".acecode", ec);
    if (ec || !SetEnvironmentVariableW(L"USERPROFILE", isolated.wstring().c_str())) {
        std::cerr << "SMOKE_FAILED: cannot isolate USERPROFILE\n";
        CoUninitialize();
        return 1;
    }
    // 状态文件显式落在隔离目录,绝不写到真实的 ~/.acecode/state.json。
    acecode::set_state_file_path_for_test(
        acecode::path_to_utf8(isolated / ".acecode" / "state.json"));

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW window_class{};
    window_class.hInstance = instance;
    window_class.lpfnWndProc = window_proc;
    window_class.lpszClassName = L"ACECodeAgentBrowserCookieSmoke";
    RegisterClassW(&window_class);
    HWND window = CreateWindowExW(0, window_class.lpszClassName, L"ACECode cookie smoke",
                                  WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 900, 640,
                                  nullptr, nullptr, instance, nullptr);
    int exit_code = 0;
    if (!window) {
        std::cerr << "SMOKE_FAILED: CreateWindowExW failed\n";
        exit_code = 1;
    } else {
        ShowWindow(window, SW_SHOW);
        Smoke smoke(window);
        try {
            run(smoke);
            std::cout << "SMOKE_COOKIE_MODES_OK\n";
        } catch (const std::exception& error) {
            std::cerr << "SMOKE_FAILED: " << error.what() << '\n';
            exit_code = 1;
        }
        DestroyWindow(window);
    }
    if (original_size > 0 && original_size < std::size(original_user_profile)) {
        SetEnvironmentVariableW(L"USERPROFILE", original_user_profile);
    } else {
        SetEnvironmentVariableW(L"USERPROFILE", nullptr);
    }
    if (exit_code == 0) {
        fs::remove_all(isolated, ec);
    } else {
        std::cerr << "SMOKE_PROFILE: " << isolated.string() << '\n';
    }
    CoUninitialize();
    return exit_code;
}
