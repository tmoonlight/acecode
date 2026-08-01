#include "desktop/agent_browser_host.hpp"
#include "tool/agent_browser/cdp_client.hpp"

#include <nlohmann/json.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>

namespace {

constexpr UINT kDispatchMessage = WM_APP + 71;

LRESULT CALLBACK window_proc(HWND window,
                             UINT message,
                             WPARAM wparam,
                             LPARAM lparam) {
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

bool pump_until(const std::function<bool()>& predicate,
                std::chrono::milliseconds timeout) {
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

int fail(const std::string& message) {
    std::cerr << "SMOKE_FAILED: " << message << '\n';
    return 1;
}

} // namespace

int main() {
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com_result)) return fail("CoInitializeEx failed");

    wchar_t original_user_profile[32768]{};
    const DWORD original_user_profile_size = GetEnvironmentVariableW(
        L"USERPROFILE", original_user_profile,
        static_cast<DWORD>(std::size(original_user_profile)));
    const std::filesystem::path isolated_user_profile =
        std::filesystem::temp_directory_path() /
        ("acecode-agent-browser-host-smoke-" +
         std::to_string(GetCurrentProcessId()));
    std::error_code filesystem_error;
    std::filesystem::create_directories(isolated_user_profile, filesystem_error);
    if (filesystem_error ||
        !SetEnvironmentVariableW(
            L"USERPROFILE", isolated_user_profile.wstring().c_str())) {
        CoUninitialize();
        return fail("failed to isolate the Agent Browser smoke profile");
    }

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    const wchar_t* class_name = L"ACECodeAgentBrowserHostSmoke";
    WNDCLASSW window_class{};
    window_class.hInstance = instance;
    window_class.lpfnWndProc = window_proc;
    window_class.lpszClassName = class_name;
    window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (!RegisterClassW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        CoUninitialize();
        return fail("RegisterClassW failed");
    }

    HWND window = CreateWindowExW(
        0, class_name, L"ACECode Agent Browser smoke",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 900, 640,
        nullptr, nullptr, instance, nullptr);
    if (!window) {
        CoUninitialize();
        return fail("CreateWindowExW failed");
    }
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);

    // Simulate ACECode's full-window main `webview_widget`. It is deliberately
    // raised after AgentBrowserHost construction below; a browser layout update
    // must bring the dedicated native Browser host back above this sibling.
    HWND covering_widget = CreateWindowExW(
        WS_EX_NOPARENTNOTIFY,
        L"STATIC",
        L"main-webview-cover",
        WS_CHILD | WS_CLIPSIBLINGS | WS_VISIBLE,
        0,
        0,
        900,
        640,
        window,
        nullptr,
        instance,
        nullptr);
    if (!covering_widget) {
        DestroyWindow(window);
        CoUninitialize();
        return fail("failed to create main WebView cover sibling");
    }

    int exit_code = 0;
    {
        const std::string instance_id =
            "agent-browser-smoke-" + std::to_string(GetCurrentProcessId());
        acecode::desktop::AgentBrowserState observed;
        acecode::desktop::AgentBrowserHost host(
            window,
            static_cast<std::int64_t>(GetCurrentProcessId()),
            instance_id,
            [&observed](const acecode::desktop::AgentBrowserState& state) {
                observed = state;
            },
            [window](std::function<void()> task) {
                auto* pending = new std::function<void()>(std::move(task));
                if (!PostMessageW(
                        window, kDispatchMessage, 0,
                        reinterpret_cast<LPARAM>(pending))) {
                    delete pending;
                }
            });
        SetWindowPos(covering_widget,
                     HWND_TOP,
                     0,
                     0,
                     900,
                     640,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        std::string error;
        if (!pump_until(
                       [&host] {
                           const auto state = host.state();
                           return state.ready || !state.error.empty();
                       },
                       std::chrono::seconds(20))) {
            exit_code = fail("WebView2 host did not become ready");
        } else if (!host.state().ready) {
            exit_code = fail(host.state().error);
        } else {
            const std::string first_page = host.create_page(&error);
            const std::string second_page = host.create_page(&error);
            if (first_page.empty() || second_page.empty() ||
                first_page == second_page) {
                exit_code = fail(error.empty()
                    ? "host did not create two distinct Browser pages" : error);
            } else if (!host.set_bounds(
                           first_page, {12, 12, 860, 580, true}, &error) ||
                       !host.set_bounds(
                           second_page, {12, 12, 860, 580, true}, &error)) {
                exit_code = fail(error);
            } else if (!pump_until(
                           [&host, &first_page, &second_page] {
                               return host.state(first_page).ready &&
                                      host.state(second_page).ready;
                           },
                           std::chrono::seconds(20))) {
                exit_code = fail("two WebView2 pages did not become ready");
            } else {
                HWND browser_widget = FindWindowExW(
                    window,
                    nullptr,
                    L"ACECodeAgentBrowserWidget",
                    nullptr);
                if (!browser_widget || GetWindow(window, GW_CHILD) != browser_widget) {
                    exit_code = fail(
                        "native Browser host was covered by the main WebView sibling");
                }
            }
            if (exit_code == 0 &&
                (!host.select_page(first_page, &error) ||
                 !host.state(first_page).visible ||
                 host.state(second_page).visible)) {
                exit_code = fail(error.empty()
                    ? "selecting the first page did not swap the visible controller"
                    : error);
            }
            if (exit_code == 0 &&
                (!host.select_page(second_page, &error) ||
                 host.state(first_page).visible ||
                 !host.state(second_page).visible)) {
                exit_code = fail(error.empty()
                    ? "selecting the second page did not swap the visible controller"
                    : error);
            }
            if (exit_code == 0 && (!host.navigate(
                           first_page, "https://example.com", &error) ||
                       !host.navigate(
                           second_page, "https://example.org", &error))) {
                exit_code = fail(error);
            } else if (exit_code == 0 && !pump_until(
                           [&host, &first_page, &second_page] {
                               const auto first = host.state(first_page);
                               const auto second = host.state(second_page);
                               return !first.loading && !second.loading &&
                                      first.url.find("example.com") !=
                                          std::string::npos &&
                                      second.url.find("example.org") !=
                                          std::string::npos;
                           },
                           std::chrono::seconds(20))) {
                exit_code = fail("independent page navigation did not settle");
            }
            if (exit_code != 0) {
                // Keep the rest of the smoke guarded while still unwinding the
                // native host on this STA thread.
            } else {
            std::atomic<bool> agent_done{false};
            std::atomic<int> agent_exit_code{0};
            std::thread agent([&, first_page, second_page] {
                std::string agent_error;
                acecode::agent_browser::AgentBrowserCdpClient client;
                if (!client.connect(
                        std::chrono::seconds(30), nullptr, agent_error)) {
                    agent_exit_code.store(fail(agent_error));
                    agent_done.store(true);
                    return;
                }
                if (!client.claim_page(
                        std::chrono::seconds(10), nullptr, agent_error) ||
                    client.page_id() != second_page) {
                    agent_exit_code.store(fail(agent_error.empty()
                        ? "default proxy routing did not claim the active page"
                        : agent_error));
                    agent_done.store(true);
                    return;
                }
                const auto read_page = [&](const std::string& page_id) {
                    nlohmann::json page = nlohmann::json::object();
                    if (!client.select_page(
                            page_id, std::chrono::seconds(10), nullptr,
                            agent_error)) {
                        return page;
                    }
                    const auto response = client.command(
                        "Runtime.evaluate",
                        {
                            {"expression",
                             "(() => ({url:location.href,title:document.title,"
                             "bindings:Object.keys(window).filter(k=>"
                             "k==='aceDesktop'||k.startsWith('aceDesktop_')).sort()}))()"},
                            {"returnByValue", true},
                        },
                        std::chrono::seconds(10), nullptr, agent_error);
                    const auto result = response.value(
                        "result", nlohmann::json::object());
                    if (result.is_object()) {
                        page = result.value(
                            "value", nlohmann::json::object());
                    }
                    return page;
                };
                const nlohmann::json first = read_page(first_page);
                const nlohmann::json second = read_page(second_page);
                if (!agent_error.empty()) {
                    agent_exit_code.store(fail(agent_error));
                } else if (first.value("url", "").find("example.com") ==
                               std::string::npos ||
                           second.value("url", "").find("example.org") ==
                               std::string::npos) {
                    agent_exit_code.store(
                        fail("CDP page-id routing crossed Browser pages"));
                } else if (!first.value(
                                "bindings", nlohmann::json::array()).empty() ||
                           !second.value(
                                "bindings", nlohmann::json::array()).empty()) {
                    agent_exit_code.store(
                        fail("the arbitrary page received ACECode host bindings"));
                } else {
                    const auto screenshot = client.command(
                        "Page.captureScreenshot",
                        {{"format", "png"}, {"fromSurface", true}},
                        std::chrono::seconds(20),
                        nullptr,
                        agent_error);
                    const auto bytes = agent_error.empty()
                        ? acecode::agent_browser::decode_agent_browser_base64(
                              screenshot.value("data", ""))
                        : std::nullopt;
                    const auto dimensions = bytes
                        ? acecode::agent_browser::agent_browser_png_dimensions(
                              *bytes)
                        : std::nullopt;
                    if (!agent_error.empty() || !dimensions) {
                        agent_exit_code.store(fail(agent_error.empty()
                            ? "CDP screenshot was not a valid PNG"
                            : agent_error));
                    } else {
                        client.select_page(
                            first_page, std::chrono::seconds(10), nullptr,
                            agent_error);
                        client.close_page(
                            std::chrono::seconds(10), nullptr, agent_error);
                        if (!agent_error.empty() ||
                            host.state(second_page).url.find("example.org") ==
                                std::string::npos) {
                            agent_exit_code.store(fail(agent_error.empty()
                                ? "closing one page damaged the other page"
                                : agent_error));
                            agent_done.store(true);
                            return;
                        }
                        std::cout
                            << "SMOKE_OK "
                            << nlohmann::json({
                                   {"first_page_id", first_page},
                                   {"second_page_id", second_page},
                                   {"first_url", first.value("url", "")},
                                   {"second_url", second.value("url", "")},
                                   {"remaining_pages", host.states().size()},
                                   {"width", dimensions->first},
                                    {"height", dimensions->second},
                                    {"acecode_bindings", 0},
                                    {"native_widget_top", true},
                                }).dump()
                            << '\n';
                    }
                }
                agent_done.store(true);
            });
            if (!pump_until(
                    [&agent_done] { return agent_done.load(); },
                    std::chrono::seconds(45))) {
                exit_code = fail("Agent Browser proxy smoke timed out");
            }
            agent.join();
            if (exit_code == 0) exit_code = agent_exit_code.load();
            }
        }
    }

    DestroyWindow(window);
    if (original_user_profile_size > 0 &&
        original_user_profile_size < std::size(original_user_profile)) {
        SetEnvironmentVariableW(L"USERPROFILE", original_user_profile);
    } else {
        SetEnvironmentVariableW(L"USERPROFILE", nullptr);
    }
    if (exit_code == 0) {
        std::filesystem::remove_all(isolated_user_profile, filesystem_error);
    } else {
        std::cerr << "SMOKE_PROFILE: " << isolated_user_profile.string() << '\n';
    }
    CoUninitialize();
    return exit_code;
}
