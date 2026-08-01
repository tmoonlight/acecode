#include "agent_browser_host.hpp"

#include "agent_browser_runtime.hpp"

#include "daemon/platform.hpp"
#include "utils/encoding.hpp"
#include "utils/logger.hpp"
#include "utils/token.hpp"
#include "utils/utf8_path.hpp"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <wrl.h>
#  include <WebView2.h>
#  include <WebView2EnvironmentOptions.h>
#  include <webview/webview.h>
#endif

namespace acecode::desktop {
namespace {

std::int64_t now_unix_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void assign_error(std::string* target, const std::string& value) {
    if (target) *target = value;
}

#ifdef _WIN32
constexpr wchar_t kAgentBrowserWidgetClassName[] =
    L"ACECodeAgentBrowserWidget";

HWND create_agent_browser_widget(HWND parent) {
    const HINSTANCE instance = ::GetModuleHandleW(nullptr);
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.hInstance = instance;
    window_class.lpfnWndProc = ::DefWindowProcW;
    window_class.lpszClassName = kAgentBrowserWidgetClassName;
    window_class.hCursor =
        ::LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    window_class.hbrBackground =
        reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (!::RegisterClassExW(&window_class) &&
        ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return nullptr;
    }

    // Keep the parent HWND visible while WebView2 creates controllers. A
    // hidden parent can leave a controller permanently unpainted on some
    // WebView2 runtimes. The parked 1x1 child is outside the client area and
    // therefore cannot cover the main ACECode WebView.
    return ::CreateWindowExW(
        WS_EX_CONTROLPARENT | WS_EX_NOPARENTNOTIFY,
        kAgentBrowserWidgetClassName,
        L"",
        WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | WS_VISIBLE,
        -1,
        -1,
        1,
        1,
        parent,
        nullptr,
        instance,
        nullptr);
}

void park_agent_browser_widget(HWND widget) {
    if (!widget || !::IsWindow(widget)) return;
    ::SetWindowPos(widget,
                   HWND_TOP,
                   -1,
                   -1,
                   1,
                   1,
                   SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

std::string hresult_text(HRESULT result) {
    return "HRESULT 0x" + [] (unsigned long value) {
        constexpr char digits[] = "0123456789ABCDEF";
        std::string output(8, '0');
        for (int index = 7; index >= 0; --index) {
            output[static_cast<std::size_t>(index)] = digits[value & 0xF];
            value >>= 4;
        }
        return output;
    }(static_cast<unsigned long>(result));
}

bool proxy_aborted(const std::atomic<bool>& stopping,
                   std::chrono::steady_clock::time_point deadline) {
    return stopping.load() || std::chrono::steady_clock::now() >= deadline;
}

bool pipe_transfer(HANDLE pipe,
                   void* buffer,
                   std::size_t size,
                   bool write,
                   const std::atomic<bool>& stopping,
                   std::chrono::steady_clock::time_point deadline) {
    std::size_t offset = 0;
    while (offset < size && !proxy_aborted(stopping, deadline)) {
        OVERLAPPED operation{};
        operation.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!operation.hEvent) return false;
        const DWORD chunk = static_cast<DWORD>((std::min)(
            size - offset, static_cast<std::size_t>(1024u * 1024u)));
        DWORD transferred = 0;
        BOOL started = write
            ? ::WriteFile(pipe,
                          static_cast<const char*>(buffer) + offset,
                          chunk, &transferred, &operation)
            : ::ReadFile(pipe,
                         static_cast<char*>(buffer) + offset,
                         chunk, &transferred, &operation);
        DWORD operation_error = started ? ERROR_SUCCESS : ::GetLastError();
        bool pending_io = false;
        if (!started && operation_error == ERROR_IO_PENDING) {
            pending_io = true;
            while (!proxy_aborted(stopping, deadline)) {
                const DWORD wait = ::WaitForSingleObject(operation.hEvent, 50);
                if (wait == WAIT_OBJECT_0) {
                    started = ::GetOverlappedResult(
                        pipe, &operation, &transferred, FALSE);
                    operation_error = started ? ERROR_SUCCESS : ::GetLastError();
                    break;
                }
                if (wait == WAIT_FAILED) {
                    operation_error = ::GetLastError();
                    break;
                }
            }
        }
        if (!started || proxy_aborted(stopping, deadline)) {
            if (pending_io) {
                ::CancelIoEx(pipe, &operation);
                ::WaitForSingleObject(operation.hEvent, INFINITE);
            }
            ::CloseHandle(operation.hEvent);
            if (!stopping.load() &&
                std::chrono::steady_clock::now() < deadline) {
                LOG_WARN(
                    std::string("[agent-browser] proxy pipe ") +
                    (write ? "write" : "read") + " failed (Windows error " +
                    std::to_string(operation_error) + ")");
            }
            return false;
        }
        ::CloseHandle(operation.hEvent);
        if (transferred == 0) return false;
        offset += transferred;
    }
    return offset == size;
}

bool connect_pipe(HANDLE pipe,
                  const std::atomic<bool>& stopping) {
    OVERLAPPED operation{};
    operation.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!operation.hEvent) return false;
    BOOL connected = ::ConnectNamedPipe(pipe, &operation);
    const DWORD error = connected ? ERROR_SUCCESS : ::GetLastError();
    bool pending_io = false;
    if (!connected && error == ERROR_PIPE_CONNECTED) connected = TRUE;
    if (!connected && error == ERROR_IO_PENDING) {
        pending_io = true;
        while (!stopping.load()) {
            const DWORD wait = ::WaitForSingleObject(operation.hEvent, 50);
            if (wait == WAIT_OBJECT_0) {
                DWORD transferred = 0;
                connected = ::GetOverlappedResult(
                    pipe, &operation, &transferred, FALSE);
                break;
            }
            if (wait == WAIT_FAILED) break;
        }
    }
    if (!connected) {
        if (pending_io) {
            ::CancelIoEx(pipe, &operation);
            ::WaitForSingleObject(operation.hEvent, INFINITE);
        }
    }
    ::CloseHandle(operation.hEvent);
    return connected != FALSE && !stopping.load();
}
#endif

} // namespace

struct AgentBrowserHost::Impl
    : public std::enable_shared_from_this<AgentBrowserHost::Impl> {
    struct PendingProxyCall {
        std::mutex mutex;
        std::condition_variable ready;
        bool completed = false;
        nlohmann::json response;
    };

    void* parent_window = nullptr;
    std::int64_t desktop_pid = 0;
    std::string desktop_instance_id;
    StateHandler state_handler;
    DispatchHandler dispatch_handler;
    mutable std::mutex state_mutex;
    AgentBrowserState host_state;

#ifdef _WIN32
    struct QueuedCdpCall {
        std::string method;
        nlohmann::json params;
        std::shared_ptr<PendingProxyCall> pending;
    };

    struct Page {
        std::string id;
        AgentBrowserState state;
        AgentBrowserBounds requested_bounds;
        bool creation_started = false;
        bool closing = false;
        Microsoft::WRL::ComPtr<ICoreWebView2Controller> controller;
        Microsoft::WRL::ComPtr<ICoreWebView2> webview;
        EventRegistrationToken navigation_starting_token{};
        EventRegistrationToken navigation_completed_token{};
        EventRegistrationToken source_changed_token{};
        EventRegistrationToken history_changed_token{};
        EventRegistrationToken title_changed_token{};
        EventRegistrationToken new_window_token{};
        std::vector<QueuedCdpCall> queued_cdp;
    };

    Microsoft::WRL::ComPtr<ICoreWebView2Environment> environment;
    webview::detail::mswebview2::loader loader;
    HWND browser_widget = nullptr;
    std::unordered_map<std::string, std::shared_ptr<Page>> pages;
    std::vector<std::string> page_order;
    std::string active_page;
    std::uint64_t next_page_sequence = 0;
    std::string proxy_pipe_name;
    std::string proxy_auth_token;
    std::atomic<bool> proxy_stopping{false};
    std::thread proxy_thread;
#endif

    Impl(void* parent,
         std::int64_t pid,
         std::string instance_id,
         StateHandler handler,
         DispatchHandler dispatcher)
        : parent_window(parent),
          desktop_pid(pid),
          desktop_instance_id(std::move(instance_id)),
          state_handler(std::move(handler)),
          dispatch_handler(std::move(dispatcher)) {
#ifdef _WIN32
        host_state.supported = parent_window != nullptr;
#else
        host_state.error =
            "Agent Browser is currently available on Windows Desktop only";
#endif
    }

    ~Impl() {
#ifdef _WIN32
        proxy_stopping.store(true);
        if (proxy_thread.joinable()) proxy_thread.join();
        std::vector<std::shared_ptr<Page>> remaining;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            for (const auto& [id, page] : pages) remaining.push_back(page);
            pages.clear();
            page_order.clear();
            active_page.clear();
        }
        for (const auto& page : remaining) teardown_page(page);
        environment.Reset();
        if (browser_widget && ::IsWindow(browser_widget)) {
            ::DestroyWindow(browser_widget);
        }
        browser_widget = nullptr;
#endif
        cleanup_agent_browser_runtime_manifest(desktop_instance_id);
    }

    void emit_state(const AgentBrowserState& state) const {
        if (state_handler) state_handler(state);
    }

    AgentBrowserState state(const std::string& requested_page = {}) const {
        std::lock_guard<std::mutex> lock(state_mutex);
#ifdef _WIN32
        const std::string id = requested_page.empty() ? active_page : requested_page;
        const auto found = pages.find(id);
        if (found != pages.end()) return found->second->state;
#else
        (void)requested_page;
#endif
        return host_state;
    }

    std::vector<AgentBrowserState> states() const {
        std::vector<AgentBrowserState> result;
        std::lock_guard<std::mutex> lock(state_mutex);
#ifdef _WIN32
        result.reserve(page_order.size());
        for (const std::string& id : page_order) {
            const auto found = pages.find(id);
            if (found != pages.end()) result.push_back(found->second->state);
        }
#endif
        return result;
    }

    std::string active_page_id() const {
        std::lock_guard<std::mutex> lock(state_mutex);
#ifdef _WIN32
        return active_page;
#else
        return {};
#endif
    }

    void update_host_state(
        const std::function<void(AgentBrowserState&)>& update) {
        AgentBrowserState snapshot;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            update(host_state);
            snapshot = host_state;
        }
        emit_state(snapshot);
    }

#ifdef _WIN32
    std::shared_ptr<Page> find_page(const std::string& requested_page) const {
        std::lock_guard<std::mutex> lock(state_mutex);
        const std::string id = requested_page.empty() ? active_page : requested_page;
        const auto found = pages.find(id);
        return found == pages.end() ? nullptr : found->second;
    }

    void update_page(
        const std::shared_ptr<Page>& page,
        const std::function<void(AgentBrowserState&)>& update) {
        if (!page) return;
        AgentBrowserState snapshot;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (page->closing && !page->state.closed) return;
            update(page->state);
            snapshot = page->state;
        }
        emit_state(snapshot);
    }
#endif

    void fail(const std::string& message) {
        LOG_ERROR("[agent-browser] " + message);
        update_host_state([&](AgentBrowserState& state) {
            state.ready = false;
            state.loading = false;
            state.error = message;
        });
#ifdef _WIN32
        for (const auto& page_state : states()) {
            if (auto page = find_page(page_state.page_id)) {
                update_page(page, [&](AgentBrowserState& state) {
                    state.ready = false;
                    state.loading = false;
                    state.error = message;
                });
            }
        }
#endif
    }

    static void finish_proxy_call(
        const std::shared_ptr<PendingProxyCall>& pending,
        nlohmann::json response) {
        {
            std::lock_guard<std::mutex> lock(pending->mutex);
            if (pending->completed) return;
            pending->completed = true;
            pending->response = std::move(response);
        }
        pending->ready.notify_all();
    }

#ifdef _WIN32
    void start() {
        if (!parent_window || !::IsWindow(static_cast<HWND>(parent_window))) {
            fail("Agent Browser parent window is unavailable");
            return;
        }
        browser_widget = create_agent_browser_widget(
            static_cast<HWND>(parent_window));
        if (!browser_widget) {
            fail("failed to create Agent Browser native child host (Windows error " +
                 std::to_string(::GetLastError()) + ")");
            return;
        }
        const auto user_data_path = agent_browser_user_data_path();
        std::error_code ec;
        std::filesystem::create_directories(user_data_path, ec);
        if (ec) {
            fail("failed to create Agent Browser profile: " + ec.message());
            return;
        }
        auto options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
        const auto weak = weak_from_this();
        auto completed = Microsoft::WRL::Callback<
            ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [weak](HRESULT result, ICoreWebView2Environment* value) -> HRESULT {
                if (const auto self = weak.lock()) {
                    self->environment_created(result, value);
                }
                return S_OK;
            });
        const HRESULT result = loader.create_environment_with_options(
            nullptr, user_data_path.wstring().c_str(), options.Get(),
            completed.Get());
        if (FAILED(result)) {
            fail("failed to start Agent Browser WebView2 environment (" +
                 hresult_text(result) + ")");
        }
    }

    bool publish_proxy() {
        if (!dispatch_handler) {
            fail("Agent Browser UI dispatcher is unavailable");
            return false;
        }
        proxy_pipe_name = "\\\\.\\pipe\\ACECode-AgentBrowser-" +
                          std::to_string(desktop_pid) + "-" +
                          desktop_instance_id;
        try {
            proxy_auth_token = acecode::generate_auth_token();
            proxy_thread = std::thread([this] { proxy_loop(); });
        } catch (const std::exception& error) {
            fail(std::string("failed to start Agent Browser proxy: ") +
                 error.what());
            return false;
        }

        AgentBrowserRuntimeManifest manifest;
        manifest.desktop_pid = desktop_pid;
        manifest.desktop_instance_id = desktop_instance_id;
        manifest.user_data_dir = acecode::path_to_utf8(
            agent_browser_user_data_path());
        manifest.pipe_name = proxy_pipe_name;
        manifest.auth_token = proxy_auth_token;
        manifest.ready_at_ms = now_unix_ms();
        if (!write_agent_browser_runtime_manifest(manifest)) {
            fail("failed to publish Agent Browser runtime endpoint");
            return false;
        }
        return true;
    }

    void environment_created(HRESULT result, ICoreWebView2Environment* value) {
        if (FAILED(result) || !value) {
            fail("Agent Browser WebView2 environment initialization failed (" +
                 hresult_text(result) + ")");
            return;
        }
        environment = value;
        if (!publish_proxy()) return;
        update_host_state([](AgentBrowserState& state) {
            state.ready = true;
            state.error.clear();
        });
        std::vector<std::shared_ptr<Page>> pending_pages;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            for (const auto& [id, page] : pages) pending_pages.push_back(page);
        }
        for (const auto& page : pending_pages) begin_create_controller(page);
        LOG_INFO("[agent-browser] WebView2 environment ready; Desktop proxy published");
    }

    std::string create_page_on_ui() {
        auto page = std::make_shared<Page>();
        page->id = "browser-" + std::to_string(desktop_pid) + "-" +
                   std::to_string(++next_page_sequence);
        page->state.page_id = page->id;
        page->state.supported = true;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            pages.emplace(page->id, page);
            page_order.push_back(page->id);
        }
        emit_state(page->state);
        std::string ignored;
        select_page_on_ui(page->id, &ignored);
        if (environment) begin_create_controller(page);
        return page->id;
    }

    void begin_create_controller(const std::shared_ptr<Page>& page) {
        if (!page || page->creation_started || page->closing || !environment) return;
        page->creation_started = true;
        const auto weak = weak_from_this();
        const std::string page_id = page->id;
        auto completed = Microsoft::WRL::Callback<
            ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [weak, page_id](HRESULT result,
                            ICoreWebView2Controller* value) -> HRESULT {
                if (const auto self = weak.lock()) {
                    self->controller_created(page_id, result, value);
                } else if (value) {
                    value->Close();
                }
                return S_OK;
            });
        park_agent_browser_widget(browser_widget);
        const HRESULT result = environment->CreateCoreWebView2Controller(
            browser_widget, completed.Get());
        if (FAILED(result)) {
            page_failed(page, "failed to create Agent Browser controller (" +
                              hresult_text(result) + ")");
        }
    }

    void page_failed(const std::shared_ptr<Page>& page,
                     const std::string& message) {
        LOG_ERROR("[agent-browser] " + page->id + ": " + message);
        update_page(page, [&](AgentBrowserState& state) {
            state.ready = false;
            state.loading = false;
            state.error = message;
        });
        std::vector<QueuedCdpCall> queued;
        queued.swap(page->queued_cdp);
        for (auto& call : queued) {
            finish_proxy_call(call.pending,
                              {{"ok", false},
                               {"page_id", page->id},
                               {"error", message}});
        }
    }

    void controller_created(const std::string& page_id,
                            HRESULT result,
                            ICoreWebView2Controller* value) {
        auto page = find_page(page_id);
        if (!page || page->closing) {
            if (value) value->Close();
            return;
        }
        if (FAILED(result) || !value) {
            page_failed(page,
                        "Agent Browser controller initialization failed (" +
                        hresult_text(result) + ")");
            return;
        }
        page->controller = value;
        if (FAILED(page->controller->get_CoreWebView2(&page->webview)) ||
            !page->webview) {
            page_failed(page, "Agent Browser page initialization failed");
            return;
        }

        Microsoft::WRL::ComPtr<ICoreWebView2Settings> settings;
        if (SUCCEEDED(page->webview->get_Settings(&settings)) && settings) {
            settings->put_AreDevToolsEnabled(TRUE);
            settings->put_AreDefaultContextMenusEnabled(FALSE);
            settings->put_IsStatusBarEnabled(FALSE);
            settings->put_IsScriptEnabled(TRUE);
            settings->put_AreDefaultScriptDialogsEnabled(TRUE);
            Microsoft::WRL::ComPtr<ICoreWebView2Settings3> settings3;
            if (SUCCEEDED(settings.As(&settings3)) && settings3) {
                settings3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
            }
        }
        install_events(page);
        apply_bounds(page);
        update_page(page, [](AgentBrowserState& state) {
            state.ready = true;
            state.error.clear();
        });
        page->webview->NavigateToString(
            LR"HTML(<!doctype html><html><head><meta charset="utf-8"><title>浏览器</title><style>html,body{height:100%;margin:0;font-family:"Segoe UI",sans-serif;color:#5f6368;background:#fff}body{display:grid;place-items:center}.empty{text-align:center}.globe{position:relative;box-sizing:border-box;width:48px;height:48px;margin:auto;border:3px solid #6b6b6b;border-radius:50%}.globe:before{content:"";position:absolute;inset:-3px 10px;border-left:2px solid #6b6b6b;border-right:2px solid #6b6b6b;border-radius:50%}.globe:after{content:"";position:absolute;left:1px;right:1px;top:50%;border-top:2px solid #6b6b6b;box-shadow:0 -12px 0 -1px #6b6b6b,0 12px 0 -1px #6b6b6b}.title{margin-top:18px;font-size:17px;font-weight:600;color:#202124}.hint{margin-top:12px;font-size:14px}</style></head><body><div class="empty"><div class="globe"></div><div class="title">浏览器</div><div class="hint">在上方输入地址开始浏览</div></div></body></html>)HTML");

        std::vector<QueuedCdpCall> queued;
        queued.swap(page->queued_cdp);
        for (auto& call : queued) {
            call_cdp_on_ui(page->id, call.method, call.params, call.pending);
        }
        LOG_INFO("[agent-browser] page ready: " + page->id);
    }

    void install_events(const std::shared_ptr<Page>& page) {
        const auto weak = weak_from_this();
        const std::string page_id = page->id;
        page->webview->add_NavigationStarting(
            Microsoft::WRL::Callback<ICoreWebView2NavigationStartingEventHandler>(
                [weak, page_id](ICoreWebView2*,
                                ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                    const auto self = weak.lock();
                    const auto current = self ? self->find_page(page_id) : nullptr;
                    if (!self || !current || !args) return S_OK;
                    LPWSTR raw = nullptr;
                    args->get_Uri(&raw);
                    const std::string uri = raw ? acecode::wide_to_utf8(raw) : "";
                    ::CoTaskMemFree(raw);
                    std::string error;
                    if (!normalize_agent_browser_url(uri, &error)) {
                        args->put_Cancel(TRUE);
                        self->update_page(current, [&](AgentBrowserState& state) {
                            state.loading = false;
                            state.error = error;
                        });
                        return S_OK;
                    }
                    self->update_page(current, [&](AgentBrowserState& state) {
                        state.loading = true;
                        state.url = uri;
                        state.error.clear();
                    });
                    return S_OK;
                }).Get(),
            &page->navigation_starting_token);

        page->webview->add_NavigationCompleted(
            Microsoft::WRL::Callback<ICoreWebView2NavigationCompletedEventHandler>(
                [weak, page_id](ICoreWebView2*,
                                ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                    const auto self = weak.lock();
                    const auto current = self ? self->find_page(page_id) : nullptr;
                    if (!self || !current || !args) return S_OK;
                    BOOL success = FALSE;
                    COREWEBVIEW2_WEB_ERROR_STATUS status =
                        COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
                    args->get_IsSuccess(&success);
                    if (!success) args->get_WebErrorStatus(&status);
                    self->update_page(current, [&](AgentBrowserState& state) {
                        state.loading = false;
                        state.error = success
                            ? std::string()
                            : "navigation failed (WebView2 status " +
                                  std::to_string(static_cast<int>(status)) + ")";
                    });
                    self->refresh_source_history_title(current);
                    return S_OK;
                }).Get(),
            &page->navigation_completed_token);

        page->webview->add_SourceChanged(
            Microsoft::WRL::Callback<ICoreWebView2SourceChangedEventHandler>(
                [weak, page_id](ICoreWebView2*,
                                ICoreWebView2SourceChangedEventArgs*) -> HRESULT {
                    if (const auto self = weak.lock()) {
                        self->refresh_source_history_title(
                            self->find_page(page_id));
                    }
                    return S_OK;
                }).Get(),
            &page->source_changed_token);
        page->webview->add_HistoryChanged(
            Microsoft::WRL::Callback<ICoreWebView2HistoryChangedEventHandler>(
                [weak, page_id](ICoreWebView2*, IUnknown*) -> HRESULT {
                    if (const auto self = weak.lock()) {
                        self->refresh_source_history_title(
                            self->find_page(page_id));
                    }
                    return S_OK;
                }).Get(),
            &page->history_changed_token);
        page->webview->add_DocumentTitleChanged(
            Microsoft::WRL::Callback<ICoreWebView2DocumentTitleChangedEventHandler>(
                [weak, page_id](ICoreWebView2*, IUnknown*) -> HRESULT {
                    if (const auto self = weak.lock()) {
                        self->refresh_source_history_title(
                            self->find_page(page_id));
                    }
                    return S_OK;
                }).Get(),
            &page->title_changed_token);
        page->webview->add_NewWindowRequested(
            Microsoft::WRL::Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                [weak, page_id](ICoreWebView2*,
                                ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                    const auto self = weak.lock();
                    if (!self || !args) return S_OK;
                    LPWSTR raw = nullptr;
                    args->get_Uri(&raw);
                    const std::string uri = raw ? acecode::wide_to_utf8(raw) : "";
                    ::CoTaskMemFree(raw);
                    args->put_Handled(TRUE);
                    std::string ignored;
                    self->navigate_on_ui(page_id, uri, &ignored);
                    return S_OK;
                }).Get(),
            &page->new_window_token);
    }

    void refresh_source_history_title(const std::shared_ptr<Page>& page) {
        if (!page || !page->webview || page->closing) return;
        LPWSTR raw_source = nullptr;
        LPWSTR raw_title = nullptr;
        BOOL can_back = FALSE;
        BOOL can_forward = FALSE;
        page->webview->get_Source(&raw_source);
        page->webview->get_DocumentTitle(&raw_title);
        page->webview->get_CanGoBack(&can_back);
        page->webview->get_CanGoForward(&can_forward);
        const std::string source = raw_source
            ? acecode::wide_to_utf8(raw_source) : "about:blank";
        const std::string title = raw_title
            ? acecode::wide_to_utf8(raw_title) : "";
        ::CoTaskMemFree(raw_source);
        ::CoTaskMemFree(raw_title);
        update_page(page, [&](AgentBrowserState& state) {
            state.url = source;
            state.title = title;
            state.can_go_back = can_back != FALSE;
            state.can_go_forward = can_forward != FALSE;
        });
    }

    void remove_events(const std::shared_ptr<Page>& page) {
        if (!page || !page->webview) return;
        page->webview->remove_NavigationStarting(page->navigation_starting_token);
        page->webview->remove_NavigationCompleted(page->navigation_completed_token);
        page->webview->remove_SourceChanged(page->source_changed_token);
        page->webview->remove_HistoryChanged(page->history_changed_token);
        page->webview->remove_DocumentTitleChanged(page->title_changed_token);
        page->webview->remove_NewWindowRequested(page->new_window_token);
    }

    void teardown_page(const std::shared_ptr<Page>& page) {
        if (!page) return;
        remove_events(page);
        if (page->controller) {
            page->controller->put_IsVisible(FALSE);
            page->controller->Close();
        }
        page->webview.Reset();
        page->controller.Reset();
    }

    bool select_page_on_ui(const std::string& page_id, std::string* error) {
        auto selected = find_page(page_id);
        if (!selected || selected->closing) {
            assign_error(error, "Agent Browser page was not found");
            return false;
        }
        std::vector<AgentBrowserState> changed;
        std::vector<std::shared_ptr<Page>> all_pages;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            active_page = page_id;
            for (const auto& [id, page] : pages) {
                const bool active = id == page_id;
                if (page->state.active != active) {
                    page->state.active = active;
                    changed.push_back(page->state);
                }
                all_pages.push_back(page);
            }
        }
        for (const auto& snapshot : changed) emit_state(snapshot);
        for (const auto& page : all_pages) apply_bounds(page);
        return true;
    }

    bool close_page_on_ui(const std::string& requested_page,
                          std::string* closed_page_id,
                          std::string* error) {
        const std::string page_id = requested_page.empty()
            ? active_page_id() : requested_page;
        auto page = find_page(page_id);
        if (!page) {
            assign_error(error, "Agent Browser page was not found");
            return false;
        }
        page->closing = true;
        teardown_page(page);
        std::vector<QueuedCdpCall> queued;
        queued.swap(page->queued_cdp);
        std::string next_active;
        bool closed_active = false;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            pages.erase(page_id);
            page_order.erase(
                std::remove(page_order.begin(), page_order.end(), page_id),
                page_order.end());
            if (active_page == page_id) {
                closed_active = true;
                active_page.clear();
                if (!page_order.empty()) next_active = page_order.back();
            }
            page->state.ready = false;
            page->state.loading = false;
            page->state.visible = false;
            page->state.active = false;
            page->state.closed = true;
        }
        emit_state(page->state);
        for (auto& call : queued) {
            finish_proxy_call(call.pending,
                              {{"ok", false},
                               {"page_id", page_id},
                               {"error", "Agent Browser page was closed"}});
        }
        if (closed_active) {
            if (!next_active.empty()) {
                std::string ignored;
                select_page_on_ui(next_active, &ignored);
            } else {
                park_agent_browser_widget(browser_widget);
            }
        }
        if (closed_page_id) *closed_page_id = page_id;
        LOG_INFO("[agent-browser] page closed: " + page_id);
        return true;
    }

    void apply_bounds(const std::shared_ptr<Page>& page) {
        if (!page) return;
        const int width = (std::max)(0, page->requested_bounds.width);
        const int height = (std::max)(0, page->requested_bounds.height);
        const bool requested_show = page->controller && page->state.active &&
                                    page->requested_bounds.visible &&
                                    width > 0 && height > 0;
        bool show = false;
        if (page->controller) {
            if (requested_show && browser_widget &&
                ::SetWindowPos(browser_widget,
                               HWND_TOP,
                               page->requested_bounds.x,
                               page->requested_bounds.y,
                               width,
                               height,
                               SWP_NOACTIVATE | SWP_SHOWWINDOW)) {
                const RECT controller_bounds{0, 0, width, height};
                const HRESULT bounds_result =
                    page->controller->put_Bounds(controller_bounds);
                page->controller->NotifyParentWindowPositionChanged();
                const HRESULT visible_result =
                    page->controller->put_IsVisible(TRUE);
                show = SUCCEEDED(bounds_result) && SUCCEEDED(visible_result);
                if (!show) {
                    LOG_WARN("[agent-browser] failed to show native page " +
                             page->id + " (bounds=" +
                             hresult_text(bounds_result) + ", visible=" +
                             hresult_text(visible_result) + ")");
                }
            } else {
                page->controller->put_IsVisible(FALSE);
            }
        }
        if (page->state.active && !show) {
            park_agent_browser_widget(browser_widget);
        }
        if (page->state.visible != show) {
            update_page(page, [&](AgentBrowserState& state) {
                state.visible = show;
            });
        }
    }

    bool navigate_on_ui(const std::string& page_id,
                        const std::string& input,
                        std::string* error) {
        auto page = find_page(page_id);
        if (!page || !page->webview) {
            assign_error(error, "Agent Browser page is still starting");
            return false;
        }
        std::string normalize_error;
        const auto url = normalize_agent_browser_url(input, &normalize_error);
        if (!url) {
            assign_error(error, normalize_error);
            return false;
        }
        const HRESULT result = page->webview->Navigate(
            acecode::utf8_to_wide(*url).c_str());
        if (FAILED(result)) {
            assign_error(error, "WebView2 navigation failed (" +
                                    hresult_text(result) + ")");
            return false;
        }
        return true;
    }

    void call_cdp_on_ui(
        const std::string& requested_page,
        const std::string& method,
        const nlohmann::json& params,
        const std::shared_ptr<PendingProxyCall>& pending) {
        std::string page_id = requested_page;
        if (page_id.empty()) page_id = active_page_id();
        if (page_id.empty()) page_id = create_page_on_ui();
        auto page = find_page(page_id);
        if (!page || page->closing) {
            finish_proxy_call(pending,
                              {{"ok", false},
                               {"page_id", page_id},
                               {"error", "Agent Browser page was not found"}});
            return;
        }
        if (!page->webview) {
            std::string host_error;
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                host_error = host_state.error;
            }
            if (!host_error.empty()) {
                finish_proxy_call(pending,
                                  {{"ok", false},
                                   {"page_id", page_id},
                                   {"error", host_error}});
                return;
            }
            page->queued_cdp.push_back({method, params, pending});
            if (environment) begin_create_controller(page);
            return;
        }
        const std::wstring wide_method = acecode::utf8_to_wide(method);
        const std::wstring wide_params = acecode::utf8_to_wide(
            (params.is_object() ? params : nlohmann::json::object()).dump());
        auto completed = Microsoft::WRL::Callback<
            ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>(
            [pending, page_id](HRESULT result, LPCWSTR raw) -> HRESULT {
                if (FAILED(result)) {
                    finish_proxy_call(
                        pending,
                        {{"ok", false},
                         {"page_id", page_id},
                         {"error", "WebView2 CDP call failed (" +
                                       hresult_text(result) + ")"}});
                    return S_OK;
                }
                const std::string text = raw
                    ? acecode::wide_to_utf8(raw) : std::string("{}");
                auto value = nlohmann::json::parse(text, nullptr, false);
                if (value.is_discarded()) {
                    finish_proxy_call(
                        pending,
                        {{"ok", false},
                         {"page_id", page_id},
                         {"error", "WebView2 returned malformed CDP JSON"}});
                    return S_OK;
                }
                finish_proxy_call(
                    pending,
                    {{"ok", true},
                     {"page_id", page_id},
                     {"result", std::move(value)}});
                return S_OK;
            });
        const HRESULT started = page->webview->CallDevToolsProtocolMethod(
            wide_method.c_str(), wide_params.c_str(), completed.Get());
        if (FAILED(started)) {
            finish_proxy_call(
                pending,
                {{"ok", false},
                 {"page_id", page_id},
                 {"error", "failed to dispatch WebView2 CDP call (" +
                               hresult_text(started) + ")"}});
        }
    }

    void execute_proxy_request_on_ui(
        const nlohmann::json& request,
        const std::shared_ptr<PendingProxyCall>& pending) {
        const std::string operation = request.value("operation", "cdp");
        const std::string requested_page = request.value("page_id", "");
        if (operation == "create_page") {
            const std::string page_id = create_page_on_ui();
            finish_proxy_call(pending,
                              {{"ok", true},
                               {"page_id", page_id},
                               {"result", {{"page_id", page_id}}}});
            return;
        }
        if (operation == "claim_page") {
            std::string page_id = requested_page.empty()
                ? active_page_id() : requested_page;
            if (page_id.empty()) page_id = create_page_on_ui();
            if (!find_page(page_id)) {
                finish_proxy_call(pending,
                                  {{"ok", false},
                                   {"page_id", page_id},
                                   {"error", "Agent Browser page was not found"}});
            } else {
                finish_proxy_call(pending,
                                  {{"ok", true},
                                   {"page_id", page_id},
                                   {"result", {{"page_id", page_id}}}});
            }
            return;
        }
        if (operation == "close_page") {
            std::string closed_page;
            std::string error;
            if (!close_page_on_ui(requested_page, &closed_page, &error)) {
                finish_proxy_call(pending,
                                  {{"ok", false},
                                   {"page_id", requested_page},
                                   {"error", error}});
            } else {
                finish_proxy_call(pending,
                                  {{"ok", true},
                                   {"page_id", closed_page},
                                   {"result", {{"closed", true},
                                               {"page_id", closed_page}}}});
            }
            return;
        }
        if (operation == "select_page") {
            std::string error;
            if (!select_page_on_ui(requested_page, &error)) {
                finish_proxy_call(pending,
                                  {{"ok", false},
                                   {"page_id", requested_page},
                                   {"error", error}});
            } else {
                finish_proxy_call(pending,
                                  {{"ok", true},
                                   {"page_id", requested_page},
                                   {"result", {{"page_id", requested_page}}}});
            }
            return;
        }
        call_cdp_on_ui(
            requested_page,
            request.value("method", ""),
            request.contains("params") && request["params"].is_object()
                ? request["params"] : nlohmann::json::object(),
            pending);
    }

    nlohmann::json execute_proxy_request(const nlohmann::json& request) {
        if (!request.is_object()) {
            return {{"ok", false}, {"error", "proxy request must be an object"}};
        }
        if (!request.contains("auth_token") ||
            !request["auth_token"].is_string() ||
            request["auth_token"].get_ref<const std::string&>() !=
                proxy_auth_token) {
            return {{"ok", false},
                    {"error", "Agent Browser proxy authentication failed"}};
        }
        const std::string operation = request.value("operation", "cdp");
        if (operation != "cdp" && operation != "create_page" &&
            operation != "claim_page" &&
            operation != "close_page" && operation != "select_page") {
            return {{"ok", false},
                    {"error", "Agent Browser proxy operation is invalid"}};
        }
        if (operation == "cdp") {
            if (!request.contains("method") || !request["method"].is_string()) {
                return {{"ok", false},
                        {"error", "Agent Browser CDP method is invalid"}};
            }
            const std::string method = request["method"].get<std::string>();
            if (method.empty() || method.size() > 256) {
                return {{"ok", false},
                        {"error", "Agent Browser CDP method is invalid"}};
            }
        }
        const int requested_timeout = request.contains("timeout_ms") &&
                request["timeout_ms"].is_number_integer()
            ? request["timeout_ms"].get<int>() : 15000;
        const int timeout_ms = (std::max)(
            100, (std::min)(120000, requested_timeout));
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeout_ms);
        auto pending = std::make_shared<PendingProxyCall>();
        const auto weak = weak_from_this();
        try {
            dispatch_handler([weak, request, pending] {
                if (const auto self = weak.lock()) {
                    self->execute_proxy_request_on_ui(request, pending);
                } else {
                    finish_proxy_call(
                        pending,
                        {{"ok", false}, {"error", "Agent Browser stopped"}});
                }
            });
        } catch (const std::exception& error) {
            return {{"ok", false},
                    {"error", std::string("Agent Browser dispatch failed: ") +
                                  error.what()}};
        }

        std::unique_lock<std::mutex> lock(pending->mutex);
        while (!pending->completed && !proxy_stopping.load() &&
               std::chrono::steady_clock::now() < deadline) {
            pending->ready.wait_for(lock, std::chrono::milliseconds(50));
        }
        if (pending->completed) return pending->response;
        return {{"ok", false},
                {"page_id", request.value("page_id", "")},
                {"error", proxy_stopping.load()
                    ? "Agent Browser stopped"
                    : "WebView2 CDP call timed out"}};
    }

    void handle_proxy_connection(HANDLE pipe) {
        const auto request_deadline = std::chrono::steady_clock::now() +
                                      std::chrono::seconds(5);
        std::uint32_t request_size = 0;
        if (!pipe_transfer(pipe, &request_size, sizeof(request_size), false,
                           proxy_stopping, request_deadline) ||
            request_size == 0 ||
            request_size > kAgentBrowserProxyMaxRequestBytes) {
            return;
        }
        std::string payload(request_size, '\0');
        if (!pipe_transfer(pipe, payload.data(), payload.size(), false,
                           proxy_stopping, request_deadline)) {
            return;
        }
        const auto request = nlohmann::json::parse(payload, nullptr, false);
        std::string response_text = execute_proxy_request(request).dump();
        if (response_text.size() > kAgentBrowserProxyMaxResponseBytes) return;
        std::uint32_t response_size =
            static_cast<std::uint32_t>(response_text.size());
        const auto response_deadline = std::chrono::steady_clock::now() +
                                       std::chrono::seconds(10);
        if (!pipe_transfer(pipe, &response_size,
                           sizeof(response_size), true,
                           proxy_stopping, response_deadline)) {
            return;
        }
        if (!pipe_transfer(pipe, response_text.data(),
                           response_text.size(), true,
                           proxy_stopping, response_deadline)) {
            return;
        }

        // DisconnectNamedPipe discards unread buffered data. Wait for a
        // bounded acknowledgement before proxy_loop disconnects the client.
        std::uint8_t acknowledgement = 0;
        const auto acknowledgement_deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(2);
        if (!pipe_transfer(pipe, &acknowledgement,
                           sizeof(acknowledgement), false,
                           proxy_stopping, acknowledgement_deadline) ||
            acknowledgement != kAgentBrowserProxyResponseAck) {
            return;
        }
    }

    void report_proxy_failure(const std::string& message) {
        const auto weak = weak_from_this();
        try {
            dispatch_handler([weak, message] {
                if (const auto self = weak.lock()) self->fail(message);
            });
        } catch (const std::exception& error) {
            LOG_ERROR("[agent-browser] failed to dispatch proxy error: " +
                      std::string(error.what()));
        }
    }

    void proxy_loop() {
        const std::wstring pipe_name = acecode::utf8_to_wide(proxy_pipe_name);
        while (!proxy_stopping.load()) {
            HANDLE pipe = ::CreateNamedPipeW(
                pipe_name.c_str(),
                PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
                    PIPE_REJECT_REMOTE_CLIENTS,
                1,
                64u * 1024u,
                64u * 1024u,
                0,
                nullptr);
            if (pipe == INVALID_HANDLE_VALUE) {
                if (!proxy_stopping.load()) {
                    report_proxy_failure(
                        "failed to create Agent Browser proxy pipe (" +
                        std::to_string(::GetLastError()) + ")");
                }
                return;
            }
            if (connect_pipe(pipe, proxy_stopping)) {
                handle_proxy_connection(pipe);
                ::DisconnectNamedPipe(pipe);
            }
            ::CloseHandle(pipe);
        }
    }

#endif
};

AgentBrowserHost::AgentBrowserHost(void* parent_window,
                                   std::int64_t desktop_pid,
                                   std::string desktop_instance_id,
                                   StateHandler state_handler,
                                   DispatchHandler dispatch_handler)
    : impl_(std::make_shared<Impl>(parent_window,
                                  desktop_pid,
                                  std::move(desktop_instance_id),
                                  std::move(state_handler),
                                  std::move(dispatch_handler))) {
#ifdef _WIN32
    impl_->start();
#endif
}

AgentBrowserHost::~AgentBrowserHost() = default;

bool AgentBrowserHost::supported() const {
    return impl_ && impl_->state().supported;
}

AgentBrowserState AgentBrowserHost::state(const std::string& page_id) const {
    return impl_ ? impl_->state(page_id) : AgentBrowserState{};
}

std::vector<AgentBrowserState> AgentBrowserHost::states() const {
    return impl_ ? impl_->states() : std::vector<AgentBrowserState>{};
}

std::string AgentBrowserHost::active_page_id() const {
    return impl_ ? impl_->active_page_id() : std::string{};
}

std::string AgentBrowserHost::create_page(std::string* error) {
#ifdef _WIN32
    if (impl_ && impl_->state().supported) return impl_->create_page_on_ui();
#endif
    assign_error(error, "Agent Browser is unavailable on this platform");
    return {};
}

bool AgentBrowserHost::close_page(const std::string& page_id,
                                  std::string* error) {
#ifdef _WIN32
    std::string ignored;
    return impl_ && impl_->close_page_on_ui(page_id, &ignored, error);
#else
    (void)page_id;
    assign_error(error, "Agent Browser is unavailable on this platform");
    return false;
#endif
}

bool AgentBrowserHost::select_page(const std::string& page_id,
                                   std::string* error) {
#ifdef _WIN32
    return impl_ && impl_->select_page_on_ui(page_id, error);
#else
    (void)page_id;
    assign_error(error, "Agent Browser is unavailable on this platform");
    return false;
#endif
}

bool AgentBrowserHost::set_bounds(const std::string& page_id,
                                  const AgentBrowserBounds& bounds,
                                  std::string* error) {
    if (!impl_ || !impl_->state().supported) {
        assign_error(error, "Agent Browser is unavailable on this platform");
        return false;
    }
    if (bounds.x < 0 || bounds.y < 0 || bounds.width < 0 || bounds.height < 0 ||
        bounds.width > 32768 || bounds.height > 32768) {
        assign_error(error, "Agent Browser bounds are invalid");
        return false;
    }
#ifdef _WIN32
    auto page = impl_->find_page(page_id);
    if (!page) {
        assign_error(error, "Agent Browser page was not found");
        return false;
    }
    page->requested_bounds = bounds;
    impl_->apply_bounds(page);
#endif
    return true;
}

bool AgentBrowserHost::navigate(const std::string& page_id,
                                const std::string& input,
                                std::string* error) {
#ifdef _WIN32
    return impl_ && impl_->navigate_on_ui(page_id, input, error);
#else
    (void)page_id;
    (void)input;
    assign_error(error, "Agent Browser is unavailable on this platform");
    return false;
#endif
}

bool AgentBrowserHost::go_back(const std::string& page_id,
                               std::string* error) {
#ifdef _WIN32
    auto page = impl_ ? impl_->find_page(page_id) : nullptr;
    if (page && page->webview && page->state.can_go_back &&
        SUCCEEDED(page->webview->GoBack())) return true;
    assign_error(error, "Agent Browser cannot go back");
#else
    (void)page_id;
    assign_error(error, "Agent Browser is unavailable on this platform");
#endif
    return false;
}

bool AgentBrowserHost::go_forward(const std::string& page_id,
                                  std::string* error) {
#ifdef _WIN32
    auto page = impl_ ? impl_->find_page(page_id) : nullptr;
    if (page && page->webview && page->state.can_go_forward &&
        SUCCEEDED(page->webview->GoForward())) return true;
    assign_error(error, "Agent Browser cannot go forward");
#else
    (void)page_id;
    assign_error(error, "Agent Browser is unavailable on this platform");
#endif
    return false;
}

bool AgentBrowserHost::reload(const std::string& page_id,
                              std::string* error) {
#ifdef _WIN32
    auto page = impl_ ? impl_->find_page(page_id) : nullptr;
    if (page && page->webview && SUCCEEDED(page->webview->Reload())) return true;
    assign_error(error, "Agent Browser page is still starting");
#else
    (void)page_id;
    assign_error(error, "Agent Browser is unavailable on this platform");
#endif
    return false;
}

bool AgentBrowserHost::focus(const std::string& page_id,
                             std::string* error) {
#ifdef _WIN32
    auto page = impl_ ? impl_->find_page(page_id) : nullptr;
    if (page && page->controller &&
        SUCCEEDED(page->controller->MoveFocus(
            COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC))) return true;
    assign_error(error, "Agent Browser page is still starting");
#else
    (void)page_id;
    assign_error(error, "Agent Browser is unavailable on this platform");
#endif
    return false;
}

void AgentBrowserHost::hide(const std::string& page_id) {
    if (!impl_) return;
#ifdef _WIN32
    if (!page_id.empty()) {
        if (auto page = impl_->find_page(page_id)) {
            page->requested_bounds.visible = false;
            impl_->apply_bounds(page);
        }
        return;
    }
    for (const auto& state : impl_->states()) {
        if (auto page = impl_->find_page(state.page_id)) {
            page->requested_bounds.visible = false;
            impl_->apply_bounds(page);
        }
    }
#else
    (void)page_id;
#endif
}

} // namespace acecode::desktop
