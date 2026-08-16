#include "desktop/agent_browser_host.hpp"
#include "tool/agent_browser/browser_tools.hpp"
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

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr UINT kDispatchMessage = WM_APP + 72;

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

void set_demo_status(acecode::agent_browser::AgentBrowserCdpClient& client,
                     const std::string& action,
                     std::string& error) {
    client.command(
        "Runtime.evaluate",
        {
            {"expression",
             "(() => { const status=document.getElementById('demo-status');"
             "if(status)status.textContent='AI action: " + action + "';"
             "return true; })()"},
            {"returnByValue", true},
        },
        std::chrono::seconds(5), nullptr, error);
}

bool dispatch_demo_input(
    acecode::agent_browser::AgentBrowserCdpClient& client,
    double x,
    double y,
    const std::string& action,
    std::string& error) {
    set_demo_status(client, action, error);
    if (!error.empty()) return false;
    client.command(
        "Runtime.evaluate",
        {
            {"expression",
             acecode::agent_browser::agent_browser_pointer_script(
                 x, y, action)},
            {"returnByValue", true},
        },
        std::chrono::seconds(5), nullptr, error);
    if (!error.empty()) return false;

    client.command(
        "Input.dispatchMouseEvent",
        {{"type", "mouseMoved"}, {"x", x}, {"y", y}},
        std::chrono::seconds(5), nullptr, error);
    if (!error.empty()) return false;

    if (action == "click") {
        client.command(
            "Input.dispatchMouseEvent",
            {{"type", "mousePressed"}, {"x", x}, {"y", y},
             {"button", "left"}, {"clickCount", 1}},
            std::chrono::seconds(5), nullptr, error);
        if (!error.empty()) return false;
        client.command(
            "Input.dispatchMouseEvent",
            {{"type", "mouseReleased"}, {"x", x}, {"y", y},
             {"button", "left"}, {"clickCount", 1}},
            std::chrono::seconds(5), nullptr, error);
    } else if (action == "scroll") {
        client.command(
            "Input.dispatchMouseEvent",
            {{"type", "mouseWheel"}, {"x", x}, {"y", y},
             {"deltaX", 0}, {"deltaY", 80}},
            std::chrono::seconds(5), nullptr, error);
    }
    return error.empty();
}

int fail(const std::string& message) {
    std::cerr << "POINTER_DEMO_FAILED: " << message << '\n';
    return 1;
}

} // namespace

int main() {
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com_result)) return fail("CoInitializeEx failed");
    const bool automated = GetEnvironmentVariableW(
        L"ACECODE_POINTER_DEMO_AUTOMATED", nullptr, 0) > 0;

    wchar_t original_user_profile[32768]{};
    const DWORD original_user_profile_size = GetEnvironmentVariableW(
        L"USERPROFILE", original_user_profile,
        static_cast<DWORD>(std::size(original_user_profile)));
    const std::filesystem::path isolated_user_profile =
        std::filesystem::temp_directory_path() /
        ("acecode-agent-browser-pointer-demo-" +
         std::to_string(GetCurrentProcessId()));
    std::error_code filesystem_error;
    std::filesystem::create_directories(isolated_user_profile, filesystem_error);
    if (filesystem_error ||
        !SetEnvironmentVariableW(
            L"USERPROFILE", isolated_user_profile.wstring().c_str())) {
        CoUninitialize();
        return fail("failed to isolate the demo profile");
    }

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    const wchar_t* class_name = L"ACECodeAgentBrowserPointerDemo";
    WNDCLASSW window_class{};
    window_class.hInstance = instance;
    window_class.lpfnWndProc = window_proc;
    window_class.lpszClassName = class_name;
    window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (!RegisterClassW(&window_class) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        CoUninitialize();
        return fail("RegisterClassW failed");
    }

    HWND window = CreateWindowExW(
        automated ? (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED) : 0,
        class_name, L"ACECode Agent Browser - AI Pointer Demo",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1000, 700,
        nullptr, nullptr, instance, nullptr);
    if (!window) {
        CoUninitialize();
        return fail("CreateWindowExW failed");
    }
    if (automated) {
        SetLayeredWindowAttributes(window, 0, 0, LWA_ALPHA);
        ShowWindow(window, SW_SHOWNOACTIVATE);
    } else {
        ShowWindow(window, SW_SHOW);
    }
    UpdateWindow(window);

    int exit_code = 0;
    {
        const std::string instance_id =
            "agent-browser-pointer-demo-" +
            std::to_string(GetCurrentProcessId());
        acecode::desktop::AgentBrowserHost host(
            window,
            static_cast<std::int64_t>(GetCurrentProcessId()),
            instance_id,
            [](const acecode::desktop::AgentBrowserState&) {},
            [window](std::function<void()> task) {
                auto* pending = new std::function<void()>(std::move(task));
                if (!PostMessageW(
                        window, kDispatchMessage, 0,
                        reinterpret_cast<LPARAM>(pending))) {
                    delete pending;
                }
            });

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
            std::string error;
            const std::string page_id = host.create_page(&error);
            if (page_id.empty()) {
                exit_code = fail(error.empty()
                    ? "failed to create the demo page" : error);
            } else if (!pump_until(
                           [&host, &page_id] {
                               const auto state = host.state(page_id);
                               return state.ready || !state.error.empty();
                           },
                           std::chrono::seconds(20)) ||
                       !host.state(page_id).ready) {
                exit_code = fail(host.state(page_id).error.empty()
                    ? "demo page did not become ready"
                    : host.state(page_id).error);
            } else if (
                !host.set_bounds(page_id, {12, 12, 960, 620, true}, &error) ||
                !host.select_page(page_id, &error) ||
                !host.set_shared_with_agent(page_id, true, &error) ||
                !host.navigate(page_id, "https://example.org", &error)) {
                exit_code = fail(error.empty()
                    ? "failed to prepare the demo page" : error);
            } else if (!pump_until(
                           [&host, &page_id, &automated] {
                               const auto state = host.state(page_id);
                               return state.ready && !state.loading &&
                                   state.content_state ==
                                       acecode::desktop::
                                           kAgentBrowserContentStateLive &&
                                   (automated || state.visible);
                           },
                           std::chrono::seconds(20))) {
                exit_code = fail("demo page navigation did not settle");
            } else {
                std::atomic<bool> agent_done{false};
                std::atomic<int> agent_exit_code{0};
                std::thread agent([&] {
                    std::string agent_error;
                    acecode::agent_browser::AgentBrowserCdpClient client;
                    if (!client.connect(
                            std::chrono::seconds(20), nullptr, agent_error)) {
                        agent_exit_code.store(fail(
                            "proxy connect failed: " + agent_error));
                        agent_done.store(true);
                        return;
                    }
                    if (!client.claim_page(
                            std::chrono::seconds(10), nullptr, agent_error)) {
                        agent_exit_code.store(fail(
                            "page claim failed: " + agent_error));
                        agent_done.store(true);
                        return;
                    }

                    client.command(
                        "Runtime.evaluate",
                        {
                            {"expression", R"JS((() => {
 document.documentElement.innerHTML=`<head><title>ACECode AI Pointer Demo</title><style>
  *{box-sizing:border-box}html,body{margin:0;min-height:100%;font-family:Segoe UI,Arial,sans-serif;color:#172033}
  body{min-height:920px;background:radial-gradient(circle at 18% 10%,#dbeafe 0,transparent 36%),linear-gradient(145deg,#f8fafc,#eef2ff)}
  main{width:min(900px,calc(100% - 56px));margin:0 auto;padding:58px 0 180px}
  .eyebrow{color:#4f46e5;font-size:13px;font-weight:800;letter-spacing:.14em;text-transform:uppercase}
  h1{margin:12px 0 10px;font-size:42px;line-height:1.08;letter-spacing:-.035em}
  .lead{margin:0;color:#64748b;font-size:17px}.status{display:inline-flex;margin-top:24px;padding:9px 14px;border:1px solid #c7d2fe;border-radius:999px;background:#fff;color:#4338ca;font-size:13px;font-weight:700;box-shadow:0 8px 24px rgba(79,70,229,.1)}
  .cards{display:grid;grid-template-columns:repeat(3,1fr);gap:22px;margin-top:48px}
  .card{height:205px;padding:26px;border:1px solid rgba(148,163,184,.28);border-radius:22px;background:rgba(255,255,255,.86);box-shadow:0 18px 50px rgba(51,65,85,.12);transition:transform .18s,box-shadow .18s}
  .card:hover{transform:translateY(-5px);box-shadow:0 24px 58px rgba(79,70,229,.18)}
  .number{display:grid;width:38px;height:38px;place-items:center;border-radius:12px;background:#eef2ff;color:#4f46e5;font-weight:800}
  h2{margin:22px 0 8px;font-size:22px}.card p{margin:0;color:#64748b;line-height:1.55}
  .footer{margin-top:70px;padding:30px;border-radius:24px;background:#172033;color:white}.footer b{color:#a5b4fc}
 </style></head><body><main>
  <div class="eyebrow">ACECode Agent Browser</div>
  <h1>Visible AI pointer</h1>
  <p class="lead">The marker follows the exact coordinates used by browser tools.</p>
  <div id="demo-status" class="status">Preparing live actions...</div>
  <section class="cards">
   <article class="card"><div class="number">1</div><h2>Hover</h2><p>Move over an interactive target.</p></article>
   <article class="card"><div class="number">2</div><h2>Click</h2><p>Pulse before the real click lands.</p></article>
   <article class="card"><div class="number">3</div><h2>Scroll</h2><p>Show where the wheel event begins.</p></article>
  </section>
  <div class="footer"><b>AI badge</b> + cursor + action ring, isolated in Shadow DOM.</div>
 </main></body>`;
 return {width:innerWidth,height:innerHeight};
})())JS"},
                            {"returnByValue", true},
                        },
                        std::chrono::seconds(10), nullptr, agent_error);
                    if (!agent_error.empty()) {
                        agent_exit_code.store(fail(agent_error));
                        agent_done.store(true);
                        return;
                    }

                    const auto runtime_value = [&](const std::string& expression) {
                        const auto response = client.command(
                            "Runtime.evaluate",
                            {{"expression", expression}, {"returnByValue", true}},
                            std::chrono::seconds(10), nullptr, agent_error);
                        if (response.contains("exceptionDetails")) {
                            agent_error = response["exceptionDetails"].dump();
                            return nlohmann::json(nullptr);
                        }
                        return response.value(
                            "result", nlohmann::json::object()).value(
                                "value", nlohmann::json(nullptr));
                    };

                    runtime_value(
                        "document.getElementById('__acecode_agent_browser_ai_pointer_v1')?.remove();true");
                    runtime_value(
                        acecode::agent_browser::
                            agent_browser_evaluate_pointer_observer_script(true));
                    const auto read_only = runtime_value("({answer:42})");
                    const auto pointer_after_read = runtime_value(
                        "Boolean(document.getElementById('__acecode_agent_browser_ai_pointer_v1'))");
                    runtime_value(R"JS((() => {
 const target=document.elementFromPoint(310,260);
 const options={bubbles:true,cancelable:true,composed:true,clientX:310,clientY:260,button:0,view:window};
 target.dispatchEvent(new MouseEvent('mousedown',options));
 target.dispatchEvent(new MouseEvent('mouseup',options));
 target.dispatchEvent(new MouseEvent('click',options));
 return true;
})())JS");
                    runtime_value(
                        acecode::agent_browser::
                            agent_browser_evaluate_pointer_observer_script(false));
                    const auto observed_event = runtime_value(R"JS((() => {
 const host=document.getElementById('__acecode_agent_browser_ai_pointer_v1');
 if(!host||!host.shadowRoot)return {ok:false};
 const rect=host.getBoundingClientRect();
 return {ok:true,left:rect.left,top:rect.top,action:host.shadowRoot.querySelector('.ace-pointer-wrap')?.dataset.action,observer:Boolean(globalThis.__acecodeEvaluatePointerObserverV1)};
})())JS");
                    if (!agent_error.empty() ||
                        !read_only.is_object() ||
                        read_only.value("answer", 0) != 42 ||
                        !pointer_after_read.is_boolean() ||
                        pointer_after_read.get<bool>() ||
                        !observed_event.is_object() ||
                        !observed_event.value("ok", false) ||
                        std::abs(observed_event.value("left", 0.0) - 310.0) > 0.5 ||
                        std::abs(observed_event.value("top", 0.0) - 260.0) > 0.5 ||
                        observed_event.value("action", "") != "click" ||
                        observed_event.value("observer", true)) {
                        agent_exit_code.store(fail(agent_error.empty()
                            ? "browser_evaluate pointer observer contract failed"
                            : agent_error));
                        agent_done.store(true);
                        return;
                    }

                    runtime_value(R"JS((() => {
 document.getElementById('__acecode_agent_browser_ai_pointer_v1')?.remove();
 const button=document.createElement('button');
 button.id='observer-fallback-target';
 button.style.cssText='position:fixed;left:620px;top:120px;width:140px;height:60px';
 document.body.append(button);
 return true;
})())JS");
                    runtime_value(
                        acecode::agent_browser::
                            agent_browser_evaluate_pointer_observer_script(true));
                    runtime_value(
                        "document.getElementById('observer-fallback-target').click();true");
                    runtime_value(
                        acecode::agent_browser::
                            agent_browser_evaluate_pointer_observer_script(false));
                    const auto fallback_event = runtime_value(R"JS((() => {
 const host=document.getElementById('__acecode_agent_browser_ai_pointer_v1');
 const rect=host?.getBoundingClientRect();
 return {ok:Boolean(rect),left:rect?.left,top:rect?.top};
})())JS");
                    if (!agent_error.empty() ||
                        !fallback_event.is_object() ||
                        !fallback_event.value("ok", false) ||
                        std::abs(fallback_event.value("left", 0.0) - 690.0) > 0.5 ||
                        std::abs(fallback_event.value("top", 0.0) - 150.0) > 0.5) {
                        agent_exit_code.store(fail(agent_error.empty()
                            ? "coordinate-less click fallback contract failed"
                            : agent_error));
                        agent_done.store(true);
                        return;
                    }
                    runtime_value(
                        "document.getElementById('observer-fallback-target')?.remove();true");

                    struct DemoAction {
                        double x;
                        double y;
                        const char* action;
                    };
                    const std::vector<DemoAction> actions{
                        {180, 390, "hover"},
                        {470, 390, "click"},
                        {755, 390, "scroll"},
                        {470, 210, "hover"},
                        {180, 390, "click"},
                        {755, 390, "scroll"},
                        {470, 390, "click"},
                    };

                    for (std::size_t index = 0;
                         index < actions.size();
                         ++index) {
                        const auto& current = actions[index];
                        if (!dispatch_demo_input(
                                client, current.x, current.y,
                                current.action, agent_error)) {
                            agent_exit_code.store(fail(agent_error));
                            agent_done.store(true);
                            return;
                        }

                        if (index == 1) {
                            std::this_thread::sleep_for(
                                std::chrono::milliseconds(260));
                            const auto layout = client.command(
                                "Runtime.evaluate",
                                {
                                    {"expression", R"JS((() => {
 const host=document.getElementById('__acecode_agent_browser_ai_pointer_v1');
 if(!host||!host.shadowRoot)return {ok:false};
 const cursor=host.shadowRoot.querySelector('.ace-pointer-cursor').getBoundingClientRect();
 const badge=host.shadowRoot.querySelector('.ace-pointer-badge').getBoundingClientRect();
 const rect=host.getBoundingClientRect();
 return {ok:true,position:getComputedStyle(host).position,left:rect.left,top:rect.top,cursorWidth:cursor.width,badgeWidth:badge.width,badge:host.shadowRoot.querySelector('.ace-pointer-badge').textContent};
})())JS"},
                                    {"returnByValue", true},
                                },
                                std::chrono::seconds(5), nullptr, agent_error);
                            const auto value = layout.value(
                                "result", nlohmann::json::object()).value(
                                    "value", nlohmann::json::object());
                            if (!agent_error.empty() ||
                                !value.value("ok", false) ||
                                value.value("position", "") != "fixed" ||
                                value.value("badge", "") != "AI" ||
                                value.value("cursorWidth", 0.0) < 20.0 ||
                                value.value("badgeWidth", 0.0) < 20.0) {
                                agent_exit_code.store(fail(agent_error.empty()
                                    ? "visible pointer layout contract failed"
                                    : agent_error));
                                agent_done.store(true);
                                return;
                            }

                            const auto screenshot = client.command(
                                "Page.captureScreenshot",
                                {{"format", "png"}, {"fromSurface", true}},
                                std::chrono::seconds(20), nullptr, agent_error);
                            const auto bytes = agent_error.empty()
                                ? acecode::agent_browser::
                                      decode_agent_browser_base64(
                                          screenshot.value("data", ""))
                                : std::nullopt;
                            const auto dimensions = bytes
                                ? acecode::agent_browser::
                                      agent_browser_png_dimensions(*bytes)
                                : std::nullopt;
                            if (!agent_error.empty() || !bytes || !dimensions) {
                                agent_exit_code.store(fail(agent_error.empty()
                                    ? "pointer screenshot was not a valid PNG"
                                    : agent_error));
                                agent_done.store(true);
                                return;
                            }
                            const auto screenshot_path =
                                std::filesystem::temp_directory_path() /
                                "acecode-agent-browser-pointer-demo.png";
                            std::ofstream output(
                                screenshot_path, std::ios::binary | std::ios::trunc);
                            output.write(
                                reinterpret_cast<const char*>(bytes->data()),
                                static_cast<std::streamsize>(bytes->size()));
                            output.close();
                            if (!output) {
                                agent_exit_code.store(
                                    fail("failed to save pointer screenshot"));
                                agent_done.store(true);
                                return;
                            }
                        }

                        std::this_thread::sleep_for(
                            automated ? std::chrono::milliseconds(50)
                                      : std::chrono::milliseconds(1500));
                    }
                    if (!automated) {
                        std::this_thread::sleep_for(std::chrono::seconds(3));
                    }
                    std::cout << "POINTER_DEMO_OK" << '\n';
                    agent_done.store(true);
                });

                if (!pump_until(
                        [&agent_done] { return agent_done.load(); },
                        std::chrono::seconds(40))) {
                    exit_code = fail("pointer demo timed out");
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
        std::cerr << "POINTER_DEMO_PROFILE: "
                  << isolated_user_profile.string() << '\n';
    }
    CoUninitialize();
    return exit_code;
}
