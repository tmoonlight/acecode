#include "agent_browser_host.hpp"

#include "agent_browser_cookie_jars.hpp"
#include "agent_browser_navigation_state.hpp"
#include "ipc/agent_browser_runtime.hpp"
#include "ipc/agent_browser_store_state.hpp"
#include "window_chrome.hpp"

#include "platform/process/os_process.hpp"
#include "utils/encoding.hpp"
#include "utils/logger.hpp"
#include "utils/token.hpp"
#include "utils/utf8_path.hpp"

#include <atomic>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include "utils/joining_thread.hpp"
#include <unordered_map>
#include <unordered_set>
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
#  include <shellapi.h>
#  include <wrl.h>
#  include <WebView2.h>
#  include <WebView2EnvironmentOptions.h>
#  include <webview/webview.h>
#endif

#include "agent_browser_windows_helpers.hpp"
#include "agent_browser_host_common.hpp"

namespace acecode::desktop {
using namespace agent_browser_common;
#ifdef _WIN32
using namespace agent_browser_windows;
#endif
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
    std::string acecode_dir;
    StateHandler state_handler;
    DispatchHandler dispatch_handler;
    StorageEventHandler storage_handler;
    mutable std::mutex state_mutex;
    AgentBrowserState host_state;
    bool parent_surface_visible = true;
    // 「保留 Cookie」档位;启动时从状态文件读取,之后只在 UI 线程上改。
    AgentBrowserCookieMode cookie_mode = AgentBrowserCookieMode::Shared;

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
        Microsoft::WRL::ComPtr<ICoreWebView2DevToolsProtocolEventReceiver>
            runtime_console_receiver;
        Microsoft::WRL::ComPtr<ICoreWebView2DevToolsProtocolEventReceiver>
            runtime_exception_receiver;
        Microsoft::WRL::ComPtr<ICoreWebView2DevToolsProtocolEventReceiver>
            log_entry_receiver;
        EventRegistrationToken navigation_starting_token{};
        EventRegistrationToken navigation_completed_token{};
        EventRegistrationToken source_changed_token{};
        EventRegistrationToken history_changed_token{};
        EventRegistrationToken title_changed_token{};
        EventRegistrationToken new_window_token{};
        EventRegistrationToken process_failed_token{};
        EventRegistrationToken runtime_console_token{};
        EventRegistrationToken runtime_exception_token{};
        EventRegistrationToken log_entry_token{};
        EventRegistrationToken server_certificate_error_token{};
        EventRegistrationToken launching_external_uri_token{};
        Microsoft::WRL::ComPtr<ICoreWebView2_14> permissive_certificates;
        Microsoft::WRL::ComPtr<ICoreWebView2_18> external_uri_schemes;
        AgentBrowserNavigationTracker navigation;
        // 已经交给系统 shell 打开的外部 URI。NavigationStarting 与
        // LaunchingExternalUriScheme 谁先到都可能触发交接,用它去重防止启动两次。
        std::string external_handoff_uri;
        // 证书放行后需要重新发起的顶层地址。
        std::string current_navigation_uri;
        std::vector<std::string> console_logs;
        std::uint64_t element_selection_generation = 0;
        std::uint64_t favicon_generation = 0;
        std::uint64_t current_navigation_id = 0;
        std::string content_state_before_navigation =
            kAgentBrowserContentStateEmpty;
        std::string favicon_before_navigation;
        std::vector<QueuedCdpCall> queued_cdp;
        // 当前 controller 所在的 cookie 存储(建 controller 时按档位与 owner 决定)。
        AgentBrowserCookieStoreKey store;
        // 存储准备好(按会话存储的先清空 / 从父会话复制做完)之前,CDP 调用与
        // 地址栏导航都要排队,否则会落在还没准备好的存储上。
        bool store_prepared = false;
        // 切档重建时记下的地址,新 controller 就绪后导航回去。
        std::string restore_url;
        // 每次(重新)创建 controller 自增;迟到的旧 controller 回调按它丢弃。
        std::uint64_t controller_generation = 0;
    };

    Microsoft::WRL::ComPtr<ICoreWebView2Environment> environment;
    webview::detail::mswebview2::loader loader;
    HWND browser_widget = nullptr;
    std::unordered_map<std::string, std::shared_ptr<Page>> pages;
    // 页序、显示页与每个会话的 Agent 默认目标都在这里;它是跨平台纯逻辑,
    // 见 agent_browser_page_directory.hpp。所有读写都在 state_mutex 下。
    AgentBrowserPageDirectory directory;
    std::uint64_t next_page_sequence = 0;
    std::string proxy_pipe_name;
    std::string proxy_auth_token;
    std::atomic<bool> proxy_stopping{false};
    // 退出即清档:InPrivate Profile 在最后一个使用者关闭后就会销毁,用一个隐藏
    // 页面把它保活到 Desktop 退出或切离该档(规格:关掉所有页面再打开仍保持登录)。
    std::shared_ptr<ProfileLease> until_quit_keepalive;
    bool until_quit_keepalive_requested = false;
    // 本次运行建过 controller 的存储(浏览器进程可能还占着它的目录)。
    std::unordered_set<std::string> loaded_store_ids;
    // 本次运行已经准备好的按会话存储,以及正在准备的(等待者回调)。
    std::unordered_set<std::string> prepared_session_stores;
    std::unordered_map<std::string, std::vector<StoreDone>> session_store_waiters;
    // 启动时状态文件里还挂着「共享存储待清空」,environment 就绪后补清。
    bool startup_shared_wipe_pending = false;
    acecode::JoiningThread proxy_thread;
#endif

    Impl(void* parent,
         std::int64_t pid,
         std::string instance_id,
         StateHandler handler,
         DispatchHandler dispatcher,
         std::string data_dir)
        : parent_window(parent),
          desktop_pid(pid),
          desktop_instance_id(std::move(instance_id)),
          acecode_dir(std::move(data_dir)),
          state_handler(std::move(handler)),
          dispatch_handler(std::move(dispatcher)) {
#ifdef _WIN32
        host_state.supported = parent_window != nullptr;
#else
        host_state.error =
            "Agent Browser is available on Windows and macOS 14+ Desktop only";
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
            directory.clear();
        }
        for (const auto& page : remaining) teardown_page(page);
        until_quit_keepalive_requested = false;
        until_quit_keepalive.reset();
        environment.Reset();
        if (browser_widget && ::IsWindow(browser_widget)) {
            ::DestroyWindow(browser_widget);
        }
        browser_widget = nullptr;
#endif
        cleanup_agent_browser_runtime_manifest(desktop_instance_id, acecode_dir);
    }

    void emit_state(const AgentBrowserState& state) const {
        if (state_handler) state_handler(state);
    }

    void emit_storage_event(AgentBrowserStorageEvent event) const {
        if (event.cookie_mode.empty()) {
            event.cookie_mode = agent_browser_cookie_mode_name(cookie_mode);
        }
        if (storage_handler) storage_handler(event);
    }

    AgentBrowserState state(const std::string& requested_page = {}) const {
        std::lock_guard<std::mutex> lock(state_mutex);
#ifdef _WIN32
        const std::string id = requested_page.empty()
            ? directory.displayed_page_id() : requested_page;
        const auto found = pages.find(id);
        if (found != pages.end()) return found->second->state;
#else
        (void)requested_page;
#endif
        return host_state;
    }

    std::vector<AgentBrowserState> states(
        const std::string& owner_session_id = {}) const {
        std::vector<AgentBrowserState> result;
        std::lock_guard<std::mutex> lock(state_mutex);
#ifdef _WIN32
        result.reserve(directory.size());
        for (const std::string& id : directory.ordered_page_ids()) {
            const auto found = pages.find(id);
            if (found == pages.end()) continue;
            if (!owner_session_id.empty() &&
                found->second->state.owner.session_id != owner_session_id) {
                continue;
            }
            result.push_back(found->second->state);
        }
#else
        (void)owner_session_id;
#endif
        return result;
    }

    std::string active_page_id() const {
        std::lock_guard<std::mutex> lock(state_mutex);
#ifdef _WIN32
        return directory.displayed_page_id();
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
        const std::string id = requested_page.empty()
            ? directory.displayed_page_id() : requested_page;
        const auto found = pages.find(id);
        return found == pages.end() ? nullptr : found->second;
    }

    // 目录里的 agent target 变化后,把每页状态上的 agent_target 位对齐并返回需要
    // 广播的快照。调用方必须持有 state_mutex。
    std::vector<AgentBrowserState> sync_agent_target_flags_locked() {
        std::vector<AgentBrowserState> changed;
        for (const auto& [id, page] : pages) {
            const bool target = directory.is_agent_target(id);
            if (page->state.agent_target != target) {
                page->state.agent_target = target;
                changed.push_back(page->state);
            }
        }
        return changed;
    }

    // 代理请求落到哪一页(见 AgentBrowserPageDirectory::resolve_agent_page)。
    // allow_create=false 时没有现成页面就返回空,调用方按「未找到」处理。
    std::string resolve_agent_page_id(const std::string& requested_page,
                                      const AgentBrowserPageOwner& owner,
                                      bool allow_create) {
        AgentBrowserPageResolution resolution;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            resolution = directory.resolve_agent_page(requested_page, owner);
        }
        if (!resolution.create) return resolution.page_id;
        if (!allow_create) return {};
        return create_page_on_ui(true, owner, true);
    }

    // 显式 select 或隐式 claim 之后把该会话的默认目标钉在这一页上。
    void set_agent_target_on_ui(const AgentBrowserPageOwner& owner,
                                const std::string& page_id) {
        if (owner.empty()) return;
        std::vector<AgentBrowserState> changed;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (!directory.set_agent_target(owner, page_id)) return;
            changed = sync_agent_target_flags_locked();
        }
        for (const auto& snapshot : changed) emit_state(snapshot);
    }

    bool page_shared_with_agent(const std::shared_ptr<Page>& page) const {
        if (!page) return false;
        std::lock_guard<std::mutex> lock(state_mutex);
        return !page->closing && page->state.shared_with_agent;
    }

    bool require_agent_shared_page(
        const std::shared_ptr<Page>& page,
        const std::string& page_id,
        const std::shared_ptr<PendingProxyCall>& pending) const {
        if (page_shared_with_agent(page)) return true;
        finish_proxy_call(
            pending,
            {{"ok", false},
             {"page_id", page_id},
             {"error", "page_not_shared_with_agent"}});
        return false;
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
        const auto user_data_path = agent_browser_user_data_path(acecode_dir);
        std::error_code ec;
        std::filesystem::create_directories(user_data_path, ec);
        if (ec) {
            fail("failed to create Agent Browser profile: " + ec.message());
            return;
        }
        run_startup_store_tasks(user_data_path);
        auto options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
        const HRESULT browser_arguments_result =
            options->put_AdditionalBrowserArguments(
                L"--allow-file-access-from-files");
        if (FAILED(browser_arguments_result)) {
            fail("failed to enable Agent Browser local-file access (" +
                 hresult_text(browser_arguments_result) + ")");
            return;
        }
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

    // WebView2 把命名 Profile 放在 <user-data>/EBWebView/WV2Profile_<ProfileName>。
    // 目录名由会话 id 的哈希派生,不会越出用户数据目录。
    // 建 environment 之前:此时 Desktop 是单实例、还没有浏览器进程占用这些目录,
    // 待清空与待删除的按会话存储直接删目录(比逐个开隐藏页面清空快得多)。
    void run_startup_store_tasks(const std::filesystem::path& user_data_path) {
        const AgentBrowserStoreState state = read_agent_browser_store_state();
        cookie_mode = effective_agent_browser_cookie_mode(state.cookie_mode);
        startup_shared_wipe_pending = state.shared_wipe_pending;
        std::vector<std::string> removed;
        for (const auto* list : {&state.pending_wipe, &state.pending_deletions}) {
            for (const auto& session_id : *list) {
                if (std::find(removed.begin(), removed.end(), session_id) != removed.end()) {
                    continue;
                }
                if (remove_session_profile_directory(user_data_path, session_id)) {
                    removed.push_back(session_id);
                }
            }
        }
        if (!removed.empty()) {
            const auto drop = [&removed](std::vector<std::string>& values) {
                values.erase(
                    std::remove_if(values.begin(), values.end(),
                                   [&removed](const std::string& value) {
                                       return std::find(removed.begin(), removed.end(),
                                                        value) != removed.end();
                                   }),
                    values.end());
            };
            update_agent_browser_store_state([&drop](AgentBrowserStoreState& current) {
                drop(current.pending_wipe);
                drop(current.pending_deletions);
                drop(current.session_stores);
                return true;
            });
            LOG_INFO("[agent-browser] removed " + std::to_string(removed.size()) +
                     " session cookie store(s) at startup");
        }
        LOG_INFO(std::string("[agent-browser] cookie mode: ") +
                 agent_browser_cookie_mode_name(cookie_mode));
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
            proxy_thread = acecode::JoiningThread([this] { proxy_loop(); });
        } catch (const std::exception& error) {
            fail(std::string("failed to start Agent Browser proxy: ") +
                 error.what());
            return false;
        }

        AgentBrowserRuntimeManifest manifest;
        manifest.desktop_pid = desktop_pid;
        manifest.desktop_instance_id = desktop_instance_id;
        manifest.user_data_dir = acecode::path_to_utf8(
            agent_browser_user_data_path(acecode_dir));
        manifest.pipe_name = proxy_pipe_name;
        manifest.auth_token = proxy_auth_token;
        manifest.ready_at_ms = now_unix_ms();
        if (!write_agent_browser_runtime_manifest(manifest, acecode_dir)) {
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
        if (startup_shared_wipe_pending) {
            startup_shared_wipe_pending = false;
            clear_shared_store_and_mark_done();
        }
        std::vector<std::shared_ptr<Page>> pending_pages;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            for (const auto& [id, page] : pages) pending_pages.push_back(page);
        }
        for (const auto& page : pending_pages) begin_create_controller(page);
        LOG_INFO("[agent-browser] WebView2 environment ready; Desktop proxy published");
    }

    std::string create_page_on_ui(bool shared_with_agent,
                                  const AgentBrowserPageOwner& owner = {},
                                  bool agent_created = false) {
        auto page = std::make_shared<Page>();
        page->id = "browser-" + std::to_string(desktop_pid) + "-" +
                   std::to_string(++next_page_sequence);
        page->state.page_id = page->id;
        page->state.supported = true;
        page->state.shared_with_agent = shared_with_agent;
        page->state.owner = owner;
        std::vector<AgentBrowserState> changed;
        AgentBrowserState created;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            pages.emplace(page->id, page);
            directory.add_page(page->id, owner, agent_created);
            changed = sync_agent_target_flags_locked();
            created = page->state;
        }
        emit_state(created);
        for (const auto& snapshot : changed) {
            if (snapshot.page_id != page->id) emit_state(snapshot);
        }
        // 有归属的 Agent 建页不抢显示:显示页由 Web UI 按当前会话决定,否则
        // 后台会话的 browser_open 会把用户正在看的页面挤掉。旧协议(无 owner)
        // 与 UI 自建页沿用「新页即显示」。
        if (!agent_created || owner.empty()) {
            std::string ignored;
            select_page_on_ui(page->id, &ignored);
        }
        if (environment) begin_create_controller(page);
        return page->id;
    }

    // 按存储键建 controller。共享存储沿用无选项的默认 Profile(升级前的登录数据
    // 就在那里);其它存储走 ControllerOptions 选命名 Profile / InPrivate。
    void begin_create_controller(const std::shared_ptr<Page>& page) {
        if (!page || page->creation_started || page->closing || !environment) return;
        page->creation_started = true;
        page->store_prepared = false;
        page->store = agent_browser_cookie_store_key(cookie_mode, page->state.owner);
        const std::uint64_t generation = ++page->controller_generation;
        if (page->store.kind == AgentBrowserCookieStoreKind::UntilQuit) {
            ensure_until_quit_keepalive();
        }
        const auto weak = weak_from_this();
        const std::string page_id = page->id;
        auto completed = Microsoft::WRL::Callback<
            ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [weak, page_id, generation](HRESULT result,
                                        ICoreWebView2Controller* value) -> HRESULT {
                if (const auto self = weak.lock()) {
                    self->controller_created(page_id, generation, result, value);
                } else if (value) {
                    value->Close();
                }
                return S_OK;
            });
        park_agent_browser_widget(browser_widget);
        const HRESULT result = create_controller_for_store(environment, browser_widget, page->store, completed.Get());
        if (FAILED(result)) {
            page_failed(page, "failed to create Agent Browser controller (" +
                              hresult_text(result) + ")");
        }
    }

    void ensure_until_quit_keepalive() {
        if (until_quit_keepalive || until_quit_keepalive_requested || !environment) return;
        until_quit_keepalive_requested = true;
        AgentBrowserCookieStoreKey key;
        key.kind = AgentBrowserCookieStoreKind::UntilQuit;
        const auto weak = weak_from_this();
        auto completed = Microsoft::WRL::Callback<
            ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [weak](HRESULT result, ICoreWebView2Controller* value) -> HRESULT {
                const auto self = weak.lock();
                if (!self || !self->until_quit_keepalive_requested ||
                    FAILED(result) || !value) {
                    if (value) value->Close();
                    if (self && FAILED(result)) {
                        self->until_quit_keepalive_requested = false;
                        LOG_WARN("[agent-browser] until-quit keepalive failed (" +
                                 hresult_text(result) + ")");
                    }
                    return S_OK;
                }
                auto lease = std::make_shared<ProfileLease>();
                lease->controller = value;
                lease->controller->put_IsVisible(FALSE);
                self->until_quit_keepalive = std::move(lease);
                return S_OK;
            });
        const HRESULT result = create_controller_for_store(environment, browser_widget, key, completed.Get());
        if (FAILED(result)) {
            until_quit_keepalive_requested = false;
            LOG_WARN("[agent-browser] failed to start until-quit keepalive (" +
                     hresult_text(result) + ")");
        }
    }

    void release_until_quit_keepalive() {
        until_quit_keepalive_requested = false;
        until_quit_keepalive.reset();
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
                            std::uint64_t generation,
                            HRESULT result,
                            ICoreWebView2Controller* value) {
        auto page = find_page(page_id);
        if (!page || page->closing || page->controller_generation != generation) {
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
            // The main ACECode WebView suppresses its default context menu,
            // but Browser pages are ordinary websites and must retain the
            // browser-native menu supplied by the installed WebView2 runtime.
            settings->put_AreDefaultContextMenusEnabled(TRUE);
            settings->put_IsStatusBarEnabled(FALSE);
            settings->put_IsScriptEnabled(TRUE);
            settings->put_AreDefaultScriptDialogsEnabled(TRUE);
            // Agent Browser 是开发浏览器:WebView2 的内建错误文档不能露出,真实
            // 失败一律由 ACECode 自己的内容状态承载,避免原生错误页在证书继续和
            // 重定向的中间态里闪一下。
            settings->put_IsBuiltInErrorPageEnabled(FALSE);
            Microsoft::WRL::ComPtr<ICoreWebView2Settings3> settings3;
            if (SUCCEEDED(settings.As(&settings3)) && settings3) {
                settings3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
            }
            // SmartScreen 信誉拦截页同样是挡板。共享 Agent Browser user-data
            // folder 的每个页面都要关,否则任何一个新页面的默认值会把它带回来。
            Microsoft::WRL::ComPtr<ICoreWebView2Settings8> settings8;
            if (SUCCEEDED(settings.As(&settings8)) && settings8) {
                settings8->put_IsReputationCheckingRequired(FALSE);
            } else {
                LOG_WARN("[agent-browser] WebView2 runtime lacks ICoreWebView2Settings8; "
                         "SmartScreen reputation checking stays enabled for " +
                         page->id);
            }
        }
        install_events(page);
        install_console_capture(page);
        const auto weak = weak_from_this();
        prepare_page_store(page, [weak, page_id, generation](bool ok,
                                                             const std::string& error) {
            const auto self = weak.lock();
            if (!self) return;
            const auto ready_page = self->find_page(page_id);
            if (!ready_page || ready_page->closing ||
                ready_page->controller_generation != generation) {
                return;
            }
            if (!ok) {
                self->page_failed(ready_page, error);
                return;
            }
            self->finish_page_ready(ready_page);
        });
    }

    void finish_page_ready(const std::shared_ptr<Page>& page) {
        page->store_prepared = true;
        apply_bounds(page);
        update_page(page, [](AgentBrowserState& state) {
            state.ready = true;
            state.error.clear();
        });

        std::vector<QueuedCdpCall> queued;
        queued.swap(page->queued_cdp);
        for (auto& call : queued) {
            // 排队的调用在入队时已经解析到这一页,显式 page_id 下 owner 不参与。
            call_cdp_on_ui(page->id, {}, call.method, call.params, call.pending);
        }
        if (!page->restore_url.empty() && page->webview) {
            const std::string url = std::move(page->restore_url);
            page->restore_url.clear();
            const HRESULT navigated =
                page->webview->Navigate(acecode::utf8_to_wide(url).c_str());
            if (FAILED(navigated)) {
                LOG_WARN("[agent-browser] failed to restore " + page->id +
                         " after cookie store change (" + hresult_text(navigated) + ")");
            }
        }
        LOG_INFO("[agent-browser] page ready: " + page->id + " (store " +
                 page->store.id() + ")");
    }

    // ---- cookie 存储:取 Profile、清空、复制、准备 ----

    std::shared_ptr<Page> live_page_in_store(const AgentBrowserCookieStoreKey& key) const {
        std::lock_guard<std::mutex> lock(state_mutex);
        for (const auto& [id, page] : pages) {
            if (!page->closing && page->webview && page->store == key) return page;
        }
        return nullptr;
    }

    std::vector<AgentBrowserPageStoreBinding> page_store_bindings() const {
        std::vector<AgentBrowserPageStoreBinding> bindings;
        std::lock_guard<std::mutex> lock(state_mutex);
        for (const std::string& id : directory.ordered_page_ids()) {
            const auto found = pages.find(id);
            if (found == pages.end() || found->second->closing) continue;
            bindings.push_back({id, found->second->state.owner, found->second->store});
        }
        return bindings;
    }

    // 拿到某份存储的 Profile:优先借用该存储里活着的页面,否则建一个隐藏的临时
    // controller(lease 交给回调,异步操作完成前由回调持有)。
    void with_profile(const AgentBrowserCookieStoreKey& key, ProfileCallback callback) {
        if (const auto page = live_page_in_store(key)) {
            if (const auto profile = profile_of(page->webview)) {
                callback(profile, {}, nullptr);
                return;
            }
        }
        if (!environment) {
            callback(nullptr, "Agent Browser environment is not ready", nullptr);
            return;
        }
        auto lease = std::make_shared<ProfileLease>();
        auto completed = Microsoft::WRL::Callback<
            ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [lease, callback](HRESULT result, ICoreWebView2Controller* value) -> HRESULT {
                if (FAILED(result) || !value) {
                    callback(nullptr,
                             "failed to open Agent Browser cookie store (" +
                                 hresult_text(result) + ")",
                             nullptr);
                    return S_OK;
                }
                lease->controller = value;
                lease->controller->put_IsVisible(FALSE);
                if (FAILED(lease->controller->get_CoreWebView2(&lease->webview)) ||
                    !lease->webview) {
                    callback(nullptr, "failed to open Agent Browser cookie store", nullptr);
                    return S_OK;
                }
                const auto profile = profile_of(lease->webview);
                callback(profile,
                         profile ? std::string()
                                 : std::string("WebView2 runtime does not expose profiles"),
                         lease);
                return S_OK;
            });
        const HRESULT result = create_controller_for_store(environment, browser_widget, key, completed.Get());
        if (FAILED(result)) {
            callback(nullptr,
                     "failed to open Agent Browser cookie store (" + hresult_text(result) + ")",
                     nullptr);
        }
    }

    void clear_store(const AgentBrowserCookieStoreKey& key, StoreDone done) {
        with_profile(key, [done](const Microsoft::WRL::ComPtr<ICoreWebView2Profile>& profile,
                                 const std::string& error,
                                 const std::shared_ptr<ProfileLease>& lease) {
            if (!profile) {
                done(false, error);
                return;
            }
            clear_profile(profile, lease, done);
        });
    }

    // 切到「退出即清」或启动补清:清空共享存储,成功后撤掉状态里的待清空标记。
    void clear_shared_store_and_mark_done() {
        clear_store(AgentBrowserCookieStoreKey{}, [](bool ok, const std::string& error) {
            if (!ok) {
                LOG_WARN("[agent-browser] failed to clear shared cookie store: " + error);
                return;
            }
            update_agent_browser_store_state([](AgentBrowserStoreState& state) {
                if (!state.shared_wipe_pending) return false;
                state.shared_wipe_pending = false;
                return true;
            });
            LOG_INFO("[agent-browser] shared cookie store cleared");
        });
    }

    // 分叉会话第一次建存储:把父会话存储里的全部 cookie 复制进来。复制失败不阻断
    // 页面(子会话只是从未登录开始),只记日志。
    void seed_store_cookies(const std::string& parent_session_id,
                            const Microsoft::WRL::ComPtr<ICoreWebView2>& target_webview,
                            const std::function<void()>& done) {
        const auto target = cookie_manager_of(profile_of(target_webview));
        if (!target) {
            LOG_WARN("[agent-browser] cannot seed forked cookie store: no cookie manager");
            done();
            return;
        }
        AgentBrowserCookieStoreKey parent;
        parent.kind = AgentBrowserCookieStoreKind::Session;
        parent.session_id = parent_session_id;
        with_profile(parent, [target, done, parent_session_id](
                                 const Microsoft::WRL::ComPtr<ICoreWebView2Profile>& profile,
                                 const std::string& error,
                                 const std::shared_ptr<ProfileLease>& lease) {
            const auto source = cookie_manager_of(profile);
            if (!source) {
                LOG_WARN("[agent-browser] cannot read parent cookie store " +
                         parent_session_id + ": " + error);
                done();
                return;
            }
            copy_profile_cookies(source, target, lease, parent_session_id, done);
        });
    }

    // 页面的 controller 建好之后、对外可用之前:按会话存储第一次被用到时按状态文件
    // 先清空(待清空)/ 从父会话复制(分叉),再登记。同一存储并发建多页时只做一次。
    void prepare_page_store(const std::shared_ptr<Page>& page, StoreDone done) {
        const AgentBrowserCookieStoreKey key = page->store;
        loaded_store_ids.insert(key.id());
        if (key.kind != AgentBrowserCookieStoreKind::Session ||
            prepared_session_stores.count(key.session_id) != 0) {
            done(true, {});
            return;
        }
        auto& waiters = session_store_waiters[key.session_id];
        waiters.push_back(std::move(done));
        if (waiters.size() > 1) return;

        const std::string session_id = key.session_id;
        const AgentBrowserSessionStorePreparation preparation =
            prepare_agent_browser_session_store(session_id, read_agent_browser_store_state());
        const auto weak = weak_from_this();
        const auto finish = [weak, session_id](bool ok, const std::string& error) {
            const auto self = weak.lock();
            if (!self) return;
            if (ok) {
                self->prepared_session_stores.insert(session_id);
                if (!update_agent_browser_store_state([&session_id](AgentBrowserStoreState& state) {
                        apply_agent_browser_session_store_preparation(session_id, state);
                        return true;
                    })) {
                    LOG_WARN("[agent-browser] failed to record cookie store " + session_id);
                }
            }
            auto pending = std::move(self->session_store_waiters[session_id]);
            self->session_store_waiters.erase(session_id);
            for (auto& waiter : pending) waiter(ok, error);
        };
        const std::string seed_from = preparation.seed_from;
        const auto webview = page->webview;
        const auto seed_then_finish = [weak, seed_from, webview, finish]() {
            const auto self = weak.lock();
            if (!self) return;
            if (seed_from.empty()) {
                finish(true, {});
                return;
            }
            self->seed_store_cookies(seed_from, webview, [finish] { finish(true, {}); });
        };
        if (!preparation.wipe_first) {
            seed_then_finish();
            return;
        }
        clear_profile(profile_of(webview), nullptr,
                      [seed_then_finish, finish](bool ok, const std::string& error) {
                          if (!ok) {
                              finish(false, error);
                              return;
                          }
                          seed_then_finish();
                      });
    }

    void append_console_log(const std::shared_ptr<Page>& page,
                            std::string entry) {
        if (!page || entry.empty()) return;
        entry = clip_agent_browser_text(
            std::move(entry), kAgentBrowserMaxConsoleEntryBytes);
        std::lock_guard<std::mutex> lock(state_mutex);
        if (page->closing) return;
        page->console_logs.push_back(std::move(entry));
        if (page->console_logs.size() > kAgentBrowserMaxConsoleEntries) {
            page->console_logs.erase(
                page->console_logs.begin(),
                page->console_logs.begin() + static_cast<std::ptrdiff_t>(
                    page->console_logs.size() -
                    kAgentBrowserMaxConsoleEntries));
        }
    }

    void clear_console_logs(const std::shared_ptr<Page>& page) {
        if (!page) return;
        std::lock_guard<std::mutex> lock(state_mutex);
        page->console_logs.clear();
    }

    std::string console_logs_for_page(
        const std::shared_ptr<Page>& page) const {
        if (!page) return {};
        std::lock_guard<std::mutex> lock(state_mutex);
        std::string joined;
        for (const auto& entry : page->console_logs) {
            if (!joined.empty()) joined.push_back('\n');
            joined += entry;
        }
        return joined;
    }

    void capture_runtime_console(const std::string& page_id,
                                 const std::string& raw) {
        const auto page = find_page(page_id);
        auto event = nlohmann::json::parse(raw, nullptr, false);
        if (!page || event.is_discarded() || !event.is_object()) return;
        const std::string level = event.value("type", "log");
        std::string message;
        if (event.contains("args") && event["args"].is_array()) {
            for (const auto& argument : event["args"]) {
                if (!message.empty()) message.push_back(' ');
                message += cdp_remote_object_text(argument);
            }
        }
        if (message.empty()) message = "console." + level;
        append_console_log(page, "[" + level + "] " + message);
    }

    void capture_runtime_exception(const std::string& page_id,
                                   const std::string& raw) {
        const auto page = find_page(page_id);
        auto event = nlohmann::json::parse(raw, nullptr, false);
        if (!page || event.is_discarded() || !event.is_object()) return;
        const auto details = event.value(
            "exceptionDetails", nlohmann::json::object());
        std::string message = details.value("text", "Uncaught exception");
        if (details.contains("exception") && details["exception"].is_object()) {
            const std::string description =
                cdp_remote_object_text(details["exception"]);
            if (!description.empty() && description != message) {
                message += ": " + description;
            }
        }
        append_console_log(page, "[error] " + message);
    }

    void capture_log_entry(const std::string& page_id,
                           const std::string& raw) {
        const auto page = find_page(page_id);
        auto event = nlohmann::json::parse(raw, nullptr, false);
        if (!page || event.is_discarded() || !event.is_object()) return;
        const auto entry = event.value("entry", nlohmann::json::object());
        if (!entry.is_object()) return;
        const std::string level = entry.value("level", "info");
        std::string message = entry.value("text", "");
        const std::string url = entry.value("url", "");
        if (!url.empty()) message += " (" + url + ")";
        append_console_log(page, "[" + level + "] " + message);
    }

    void install_console_capture(const std::shared_ptr<Page>& page) {
        if (!page || !page->webview) return;
        const auto weak = weak_from_this();
        const std::string page_id = page->id;
        const auto add_receiver = [&](
            const wchar_t* event_name,
            Microsoft::WRL::ComPtr<
                ICoreWebView2DevToolsProtocolEventReceiver>& receiver,
            EventRegistrationToken& token,
            auto capture) {
            if (FAILED(page->webview->GetDevToolsProtocolEventReceiver(
                    event_name, receiver.ReleaseAndGetAddressOf())) ||
                !receiver) {
                return;
            }
            receiver->add_DevToolsProtocolEventReceived(
                Microsoft::WRL::Callback<
                    ICoreWebView2DevToolsProtocolEventReceivedEventHandler>(
                    [weak, page_id, capture](
                        ICoreWebView2*,
                        ICoreWebView2DevToolsProtocolEventReceivedEventArgs*
                            args) -> HRESULT {
                        if (!args) return S_OK;
                        LPWSTR raw = nullptr;
                        args->get_ParameterObjectAsJson(&raw);
                        const std::string text = raw
                            ? acecode::wide_to_utf8(raw) : std::string("{}");
                        ::CoTaskMemFree(raw);
                        if (const auto self = weak.lock()) {
                            (self.get()->*capture)(page_id, text);
                        }
                        return S_OK;
                    }).Get(),
                &token);
        };
        add_receiver(
            L"Runtime.consoleAPICalled",
            page->runtime_console_receiver,
            page->runtime_console_token,
            &Impl::capture_runtime_console);
        add_receiver(
            L"Runtime.exceptionThrown",
            page->runtime_exception_receiver,
            page->runtime_exception_token,
            &Impl::capture_runtime_exception);
        add_receiver(
            L"Log.entryAdded",
            page->log_entry_receiver,
            page->log_entry_token,
            &Impl::capture_log_entry);

        auto enabled = Microsoft::WRL::Callback<
            ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>(
            [page_id](HRESULT result, LPCWSTR) -> HRESULT {
                if (FAILED(result)) {
                    LOG_WARN("[agent-browser] failed to enable console capture for " +
                             page_id + " (" + hresult_text(result) + ")");
                }
                return S_OK;
            });
        page->webview->CallDevToolsProtocolMethod(
            L"Runtime.enable", L"{}", enabled.Get());
        page->webview->CallDevToolsProtocolMethod(
            L"Log.enable", L"{}", enabled.Get());
    }

    void invalidate_element_selection(const std::shared_ptr<Page>& page) {
        if (!page) return;
        AgentBrowserState snapshot;
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (page->state.element_selection_active) {
                page->state.element_selection_active = false;
                ++page->element_selection_generation;
                snapshot = page->state;
                changed = true;
            }
        }
        if (changed) emit_state(snapshot);
    }

    void apply_favicon_result(const std::shared_ptr<Page>& page,
                              std::uint64_t generation,
                              std::string favicon) {
        if (!valid_agent_browser_favicon(favicon)) favicon.clear();
        AgentBrowserState snapshot;
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (!page || page->closing ||
                page->favicon_generation != generation ||
                page->state.content_state != kAgentBrowserContentStateLive ||
                page->state.favicon == favicon) {
                return;
            }
            page->state.favicon = std::move(favicon);
            snapshot = page->state;
            changed = true;
        }
        if (changed) emit_state(snapshot);
    }

    void refresh_favicon(const std::shared_ptr<Page>& page) {
        if (!page || !page->webview || page->closing) return;
        std::uint64_t generation = 0;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (page->state.content_state != kAgentBrowserContentStateLive) {
                return;
            }
            generation = ++page->favicon_generation;
        }

        const auto params = nlohmann::json{
            {"expression", agent_browser_favicon_expression()},
            {"awaitPromise", true},
            {"returnByValue", true},
        }.dump();
        const std::wstring wide_params = acecode::utf8_to_wide(params);
        const auto weak = weak_from_this();
        const std::string page_id = page->id;
        const auto completed = Microsoft::WRL::Callback<
            ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>(
            [weak, page_id, generation](HRESULT result,
                                        LPCWSTR raw_result) -> HRESULT {
                const auto self = weak.lock();
                const auto current = self ? self->find_page(page_id) : nullptr;
                if (!self || !current) return S_OK;
                if (FAILED(result) || !raw_result) {
                    LOG_WARN("[agent-browser] failed to resolve favicon for " +
                             page_id + " (" + hresult_text(result) + ")");
                    return S_OK;
                }
                const auto response = nlohmann::json::parse(
                    acecode::wide_to_utf8(raw_result), nullptr, false);
                if (response.is_discarded() || !response.is_object()) {
                    return S_OK;
                }
                const auto remote = response.value(
                    "result", nlohmann::json::object());
                const std::string favicon =
                    remote.is_object() && remote.contains("value") &&
                        remote["value"].is_string()
                    ? remote["value"].get<std::string>()
                    : std::string();
                self->apply_favicon_result(
                    current, generation, favicon);
                return S_OK;
            });
        const HRESULT dispatched = page->webview->CallDevToolsProtocolMethod(
            L"Runtime.evaluate", wide_params.c_str(), completed.Get());
        if (FAILED(dispatched)) {
            LOG_WARN("[agent-browser] failed to request favicon for " +
                     page_id + " (" + hresult_text(dispatched) + ")");
        }
    }

    // 把系统已注册的外部 URI/SSO scheme 交给 Windows shell,不弹 ACECode 也不弹
    // WebView2 的确认框。NavigationStarting 与 LaunchingExternalUriScheme 都可能
    // 先到,用 external_handoff_uri 去重。
    bool handoff_external_uri(const std::shared_ptr<Page>& page,
                              const std::string& uri) {
        if (!page || uri.empty()) return false;
        if (!agent_browser_external_handoff_candidate(uri)) return false;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (page->closing) return false;
            if (page->external_handoff_uri == uri) {
                page->navigation.note_external_handoff(0);
                return true;
            }
            page->external_handoff_uri = uri;
            page->navigation.note_external_handoff(0);
        }
        const std::wstring wide_uri = acecode::utf8_to_wide(uri);
        HINSTANCE result = ::ShellExecuteW(
            nullptr, L"open", wide_uri.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        const auto code = reinterpret_cast<std::intptr_t>(result);
        const bool launched = code > 32;
        if (launched) {
            LOG_INFO("[agent-browser] launched external URI scheme for " +
                     page->id);
        } else {
            // 没有注册 handler 就是一次真实的外部启动失败,如实记录 —— 不能把它
            // 说成受保护的 HTTP(S) 页面自己出了证书问题。
            std::lock_guard<std::mutex> lock(state_mutex);
            page->external_handoff_uri.clear();
            LOG_WARN("[agent-browser] external URI launch failed for " +
                     page->id + " (ShellExecute " + std::to_string(code) + ")");
        }
        return launched;
    }

    // 证书放行之后平台要求重新发起顶层请求。全页最多一次,由纯逻辑层的
    // AgentBrowserNavigationTracker 保证不会形成重试循环。
    void retry_navigation_after_certificate(
        const std::shared_ptr<Page>& page) {
        if (!page || !page->webview) return;
        std::string uri;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (page->closing) return;
            uri = page->current_navigation_uri;
        }
        if (uri.empty()) {
            page->webview->Reload();
            return;
        }
        LOG_INFO("[agent-browser] retrying navigation after certificate "
                 "continuation for " + page->id);
        page->webview->Navigate(acecode::utf8_to_wide(uri).c_str());
    }

    void install_permissive_certificate_events(
        const std::shared_ptr<Page>& page) {
        const auto weak = weak_from_this();
        const std::string page_id = page->id;
        if (FAILED(page->webview.As(&page->permissive_certificates)) ||
            !page->permissive_certificates) {
            LOG_WARN("[agent-browser] WebView2 runtime lacks ICoreWebView2_14; "
                     "server certificate errors keep the default TLS handling "
                     "for " + page_id);
        } else {
            page->permissive_certificates->add_ServerCertificateErrorDetected(
                Microsoft::WRL::Callback<
                    ICoreWebView2ServerCertificateErrorDetectedEventHandler>(
                    [weak, page_id](
                        ICoreWebView2*,
                        ICoreWebView2ServerCertificateErrorDetectedEventArgs* args)
                        -> HRESULT {
                        if (!args) return S_OK;
                        // 顶层页面与子资源用同一个放行动作。
                        args->put_Action(
                            COREWEBVIEW2_SERVER_CERTIFICATE_ERROR_ACTION_ALWAYS_ALLOW);
                        const auto self = weak.lock();
                        const auto current =
                            self ? self->find_page(page_id) : nullptr;
                        if (!self || !current) return S_OK;
                        BrowserNavigationDecision decision =
                            BrowserNavigationDecision::kIgnore;
                        {
                            std::lock_guard<std::mutex> lock(self->state_mutex);
                            if (current->closing) return S_OK;
                            decision =
                                current->navigation.on_certificate_allowed(0);
                        }
                        if (decision == BrowserNavigationDecision::kRetryOnce) {
                            self->retry_navigation_after_certificate(current);
                        }
                        return S_OK;
                    }).Get(),
                &page->server_certificate_error_token);
        }

        if (FAILED(page->webview.As(&page->external_uri_schemes)) ||
            !page->external_uri_schemes) {
            LOG_WARN("[agent-browser] WebView2 runtime lacks ICoreWebView2_18; "
                     "external URI schemes keep the default confirmation for " +
                     page_id);
            return;
        }
        page->external_uri_schemes->add_LaunchingExternalUriScheme(
            Microsoft::WRL::Callback<
                ICoreWebView2LaunchingExternalUriSchemeEventHandler>(
                [weak, page_id](
                    ICoreWebView2*,
                    ICoreWebView2LaunchingExternalUriSchemeEventArgs* args)
                    -> HRESULT {
                    if (!args) return S_OK;
                    // 取消 WebView2 自带的确认对话框,由我们直接交给系统 shell。
                    args->put_Cancel(TRUE);
                    LPWSTR raw = nullptr;
                    args->get_Uri(&raw);
                    const std::string uri =
                        raw ? acecode::wide_to_utf8(raw) : "";
                    ::CoTaskMemFree(raw);
                    const auto self = weak.lock();
                    const auto current =
                        self ? self->find_page(page_id) : nullptr;
                    if (!self || !current) return S_OK;
                    self->handoff_external_uri(current, uri);
                    return S_OK;
                }).Get(),
            &page->launching_external_uri_token);
    }

    void install_events(const std::shared_ptr<Page>& page) {
        const auto weak = weak_from_this();
        const std::string page_id = page->id;
        // 必须在任何导航之前注册,否则第一条 TLS 拦截页就已经露出来了。
        install_permissive_certificate_events(page);
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
                    std::uint64_t navigation_id = 0;
                    args->get_NavigationId(&navigation_id);
                    {
                        std::lock_guard<std::mutex> lock(self->state_mutex);
                        if (current->current_navigation_id != navigation_id &&
                            current->state.content_state !=
                                kAgentBrowserContentStateLoading) {
                            current->content_state_before_navigation =
                                current->state.content_state;
                            current->favicon_before_navigation =
                                current->state.favicon;
                        }
                        current->current_navigation_id = navigation_id;
                        current->navigation.begin_navigation(navigation_id);
                        ++current->favicon_generation;
                    }
                    std::string error;
                    if (!normalize_agent_browser_url(uri, &error)) {
                        args->put_Cancel(TRUE);
                        // 系统已注册的外部 URI/SSO scheme 静默交给操作系统,并
                        // 保留当前页面 —— 这是一次有意的交接,不是导航失败。
                        if (self->handoff_external_uri(current, uri)) {
                            return S_OK;
                        }
                        self->update_page(current, [&](AgentBrowserState& state) {
                            state.loading = false;
                            state.error = error;
                        });
                        self->apply_bounds(current);
                        return S_OK;
                    }
                    {
                        std::lock_guard<std::mutex> lock(self->state_mutex);
                        current->current_navigation_uri = uri;
                    }
                    self->update_page(current, [&](AgentBrowserState& state) {
                        state.loading = true;
                        state.url = uri;
                        state.favicon.clear();
                        state.content_state = kAgentBrowserContentStateLoading;
                        state.failure_kind.clear();
                        state.error.clear();
                    });
                    self->apply_bounds(current);
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
                    std::uint64_t navigation_id = 0;
                    args->get_IsSuccess(&success);
                    args->get_NavigationId(&navigation_id);
                    if (!success) args->get_WebErrorStatus(&status);
                    std::string previous_content_state;
                    std::string previous_favicon;
                    bool process_already_failed = false;
                    BrowserNavigationDecision decision =
                        BrowserNavigationDecision::kIgnore;
                    const std::string failure_kind = success
                        ? std::string{}
                        : std::string(agent_browser_web_error_kind(status));
                    {
                        std::lock_guard<std::mutex> lock(self->state_mutex);
                        previous_content_state =
                            current->content_state_before_navigation;
                        previous_favicon = current->favicon_before_navigation;
                        process_already_failed =
                            current->state.content_state ==
                                kAgentBrowserContentStateProcessFailed;
                        decision = current->navigation.on_navigation_completed(
                            navigation_id,
                            classify_windows_navigation_failure(
                                failure_kind,
                                current->navigation.external_handoff_pending()));
                    }
                    if (process_already_failed) return S_OK;
                    if (decision == BrowserNavigationDecision::kIgnore) {
                        // 陈旧回调或已关闭的页面:当前可见状态保持不动。
                        return S_OK;
                    }
                    if (decision == BrowserNavigationDecision::kHoldPending) {
                        // WebView2 会先报证书失败的 NavigationCompleted,再发出
                        // ServerCertificateErrorDetected。这里只挂起等待放行,不
                        // 发布最终失败,否则中间会闪一帧错误页。
                        return S_OK;
                    }
                    if (decision == BrowserNavigationDecision::kRetryOnce) {
                        self->retry_navigation_after_certificate(current);
                        return S_OK;
                    }
                    LPWSTR raw_source = nullptr;
                    current->webview->get_Source(&raw_source);
                    const std::string source = raw_source
                        ? acecode::wide_to_utf8(raw_source) : "about:blank";
                    ::CoTaskMemFree(raw_source);
                    self->update_page(current, [&](AgentBrowserState& state) {
                        state.loading = false;
                        if (decision ==
                            BrowserNavigationDecision::kPublishSuccess) {
                            const bool empty = source.empty() ||
                                source == "about:blank";
                            state.url = empty ? "about:blank" : source;
                            state.title = empty
                                ? std::string(kAgentBrowserDefaultTitle)
                                : state.title;
                            state.content_state = empty
                                ? kAgentBrowserContentStateEmpty
                                : kAgentBrowserContentStateLive;
                            state.failure_kind.clear();
                            state.error.clear();
                        } else if (decision ==
                                   BrowserNavigationDecision::kRestorePrevious) {
                            // 被新导航取代、被重定向替换,或已交给操作系统的外部
                            // scheme。恢复导航前的页面,不显示失败。
                            state.content_state = previous_content_state;
                            state.favicon = previous_favicon;
                            state.failure_kind.clear();
                            state.error.clear();
                        } else {
                            state.content_state =
                                kAgentBrowserContentStateNavigationError;
                            state.failure_kind = failure_kind;
                            state.error = "navigation failed (WebView2 status " +
                                std::to_string(static_cast<int>(status)) + ")";
                        }
                    });
                    if (decision ==
                        BrowserNavigationDecision::kRestorePrevious) {
                        std::lock_guard<std::mutex> lock(self->state_mutex);
                        current->external_handoff_uri.clear();
                    }
                    self->refresh_source_history_title(current);
                    if (decision == BrowserNavigationDecision::kPublishSuccess) {
                        self->refresh_favicon(current);
                    }
                    self->apply_bounds(current);
                    return S_OK;
                }).Get(),
            &page->navigation_completed_token);

        page->webview->add_SourceChanged(
            Microsoft::WRL::Callback<ICoreWebView2SourceChangedEventHandler>(
                [weak, page_id](ICoreWebView2*,
                                ICoreWebView2SourceChangedEventArgs* args) -> HRESULT {
                    if (const auto self = weak.lock()) {
                        const auto current = self->find_page(page_id);
                        BOOL new_document = FALSE;
                        if (args) args->get_IsNewDocument(&new_document);
                        if (new_document) {
                            self->clear_console_logs(current);
                            self->invalidate_element_selection(current);
                        }
                        self->refresh_source_history_title(current);
                    }
                    return S_OK;
                }).Get(),
            &page->source_changed_token);
        page->webview->add_HistoryChanged(
            Microsoft::WRL::Callback<ICoreWebView2HistoryChangedEventHandler>(
                [weak, page_id](ICoreWebView2*, IUnknown*) -> HRESULT {
                    if (const auto self = weak.lock()) {
                        const auto current = self->find_page(page_id);
                        self->refresh_source_history_title(current);
                        self->refresh_favicon(current);
                    }
                    return S_OK;
                }).Get(),
            &page->history_changed_token);
        page->webview->add_DocumentTitleChanged(
            Microsoft::WRL::Callback<ICoreWebView2DocumentTitleChangedEventHandler>(
                [weak, page_id](ICoreWebView2*, IUnknown*) -> HRESULT {
                    if (const auto self = weak.lock()) {
                        const auto current = self->find_page(page_id);
                        self->refresh_source_history_title(current);
                        self->refresh_favicon(current);
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
        page->webview->add_ProcessFailed(
            Microsoft::WRL::Callback<ICoreWebView2ProcessFailedEventHandler>(
                [weak, page_id](ICoreWebView2*,
                                ICoreWebView2ProcessFailedEventArgs* args) -> HRESULT {
                    const auto self = weak.lock();
                    const auto current = self ? self->find_page(page_id) : nullptr;
                    if (!self || !current || !args) return S_OK;

                    COREWEBVIEW2_PROCESS_FAILED_KIND kind =
                        COREWEBVIEW2_PROCESS_FAILED_KIND_UNKNOWN_PROCESS_EXITED;
                    COREWEBVIEW2_PROCESS_FAILED_REASON reason =
                        COREWEBVIEW2_PROCESS_FAILED_REASON_UNEXPECTED;
                    int exit_code = 0;
                    args->get_ProcessFailedKind(&kind);
                    Microsoft::WRL::ComPtr<
                        ICoreWebView2ProcessFailedEventArgs2> details;
                    if (SUCCEEDED(args->QueryInterface(IID_PPV_ARGS(&details))) &&
                        details) {
                        details->get_Reason(&reason);
                        details->get_ExitCode(&exit_code);
                    }
                    const std::string failure_kind =
                        agent_browser_process_failure_kind(kind, reason);
                    const std::string technical =
                        "process failed (kind " +
                        std::to_string(static_cast<int>(kind)) + ", reason " +
                        std::to_string(static_cast<int>(reason)) + ", exit " +
                        std::to_string(exit_code) + ")";
                    self->append_console_log(
                        current, "[browser] " + technical);
                    if (failure_kind.empty()) return S_OK;

                    self->invalidate_element_selection(current);
                    self->update_page(current, [&](AgentBrowserState& state) {
                        state.loading = false;
                        state.content_state =
                            kAgentBrowserContentStateProcessFailed;
                        state.failure_kind = failure_kind;
                        state.error = technical;
                    });
                    self->apply_bounds(current);
                    return S_OK;
                }).Get(),
            &page->process_failed_token);
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
        const std::string title = agent_browser_title_or_default(
            raw_title ? acecode::wide_to_utf8(raw_title) : "");
        ::CoTaskMemFree(raw_source);
        ::CoTaskMemFree(raw_title);
        update_page(page, [&](AgentBrowserState& state) {
            if (state.content_state == kAgentBrowserContentStateLive) {
                state.url = source;
                state.title = title;
            }
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
        page->webview->remove_ProcessFailed(page->process_failed_token);
        // 宽松证书与外部 URI 的 handler 捕获了页面状态,页面拆掉之前必须注销,
        // 否则迟到的事件会去访问已经失效的 Page。
        if (page->permissive_certificates) {
            page->permissive_certificates->remove_ServerCertificateErrorDetected(
                page->server_certificate_error_token);
            page->permissive_certificates.Reset();
        }
        if (page->external_uri_schemes) {
            page->external_uri_schemes->remove_LaunchingExternalUriScheme(
                page->launching_external_uri_token);
            page->external_uri_schemes.Reset();
        }
        if (page->runtime_console_receiver) {
            page->runtime_console_receiver->remove_DevToolsProtocolEventReceived(
                page->runtime_console_token);
            page->runtime_console_receiver.Reset();
        }
        if (page->runtime_exception_receiver) {
            page->runtime_exception_receiver->remove_DevToolsProtocolEventReceived(
                page->runtime_exception_token);
            page->runtime_exception_receiver.Reset();
        }
        if (page->log_entry_receiver) {
            page->log_entry_receiver->remove_DevToolsProtocolEventReceived(
                page->log_entry_token);
            page->log_entry_receiver.Reset();
        }
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
            directory.set_displayed_page(page_id);
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
        // 页面关闭后仍会有在途导航回调,标记之后一律忽略。
        page->navigation.close();
        teardown_page(page);
        std::vector<QueuedCdpCall> queued;
        queued.swap(page->queued_cdp);
        std::string next_active;
        bool closed_active = false;
        std::vector<AgentBrowserState> changed;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            closed_active = directory.displayed_page_id() == page_id;
            next_active = directory.next_displayed_after_close(page_id);
            pages.erase(page_id);
            directory.remove_page(page_id);
            changed = sync_agent_target_flags_locked();
            page->state.ready = false;
            page->state.loading = false;
            page->state.visible = false;
            page->state.active = false;
            page->state.agent_target = false;
            page->state.closed = true;
        }
        emit_state(page->state);
        for (const auto& snapshot : changed) emit_state(snapshot);
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
                hide_agent_browser_widget(browser_widget);
            }
        }
        if (closed_page_id) *closed_page_id = page_id;
        LOG_INFO("[agent-browser] page closed: " + page_id);
        return true;
    }

    // 切档后页面换存储:关掉旧 controller,用同一个 page id、owner 与共享标记在新
    // 存储里重建,并导航回原地址。期间的 CDP 调用排队,就绪后照常执行。
    void rebuild_page_on_ui(const std::shared_ptr<Page>& page) {
        if (!page || page->closing) return;
        std::string url;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            url = page->state.url;
        }
        teardown_page(page);
        page->creation_started = false;
        page->store_prepared = false;
        page->navigation = AgentBrowserNavigationTracker{};
        page->external_handoff_uri.clear();
        page->current_navigation_uri.clear();
        ++page->element_selection_generation;
        ++page->favicon_generation;
        page->restore_url = (url.empty() || url == "about:blank") ? std::string() : url;
        const bool restoring = !page->restore_url.empty();
        update_page(page, [restoring](AgentBrowserState& state) {
            state.ready = false;
            state.loading = restoring;
            state.visible = false;
            state.can_go_back = false;
            state.can_go_forward = false;
            state.element_selection_active = false;
            state.failure_kind.clear();
            state.error.clear();
            state.diagnostic.clear();
            state.content_state = restoring ? kAgentBrowserContentStateLoading
                                            : kAgentBrowserContentStateEmpty;
        });
        apply_bounds(page);
        begin_create_controller(page);
    }

    bool set_cookie_mode_on_ui(const std::string& name, std::string* error) {
        const auto mode = agent_browser_cookie_mode_from_name(name);
        if (!mode) {
            assign_error(error, "unknown Agent Browser cookie mode: " + name);
            return false;
        }
        const AgentBrowserCookieModeSwitch plan =
            plan_agent_browser_cookie_mode_switch(cookie_mode, *mode);
        // 档位相同也写一次:状态里可能是空(从未选过)或别的版本写入的未知值。
        const bool persisted = update_agent_browser_store_state(
            [&mode, &plan](AgentBrowserStoreState& state) {
                state.cookie_mode = agent_browser_cookie_mode_name(*mode);
                if (plan.mark_session_stores_for_wipe) {
                    state.pending_wipe.insert(state.pending_wipe.end(),
                                              state.session_stores.begin(),
                                              state.session_stores.end());
                }
                if (plan.clear_shared_now) state.shared_wipe_pending = true;
                return true;
            });
        if (!persisted) {
            assign_error(error, "failed to save the Agent Browser cookie setting");
            return false;
        }
        if (!plan.changed) return true;

        const AgentBrowserCookieMode previous = cookie_mode;
        cookie_mode = *mode;
        if (plan.mark_session_stores_for_wipe) prepared_session_stores.clear();
        // 先把页面搬进新存储;清空走临时 controller,与页面互不影响。
        const auto rebuild = agent_browser_pages_to_rebuild(page_store_bindings(), cookie_mode);
        for (const auto& page_id : rebuild) {
            rebuild_page_on_ui(find_page(page_id));
        }
        if (plan.clear_shared_now) clear_shared_store_and_mark_done();
        if (plan.release_until_quit_store) release_until_quit_keepalive();
        LOG_INFO(std::string("[agent-browser] cookie mode ") +
                 agent_browser_cookie_mode_name(previous) + " -> " +
                 agent_browser_cookie_mode_name(cookie_mode) + "; rebuilt " +
                 std::to_string(rebuild.size()) + " page(s)");
        AgentBrowserStorageEvent event;
        event.type = "cookie_mode_changed";
        emit_storage_event(std::move(event));
        return true;
    }

    // 清除该页当前所用存储的全部网站数据,完成后重新加载同一存储里的页面。
    bool clear_browsing_data_on_ui(const std::string& page_id, std::string* error) {
        const auto page = find_page(page_id);
        if (!page || page->closing) {
            assign_error(error, "Agent Browser page was not found");
            return false;
        }
        if (!environment) {
            assign_error(error, "Agent Browser is still starting");
            return false;
        }
        const AgentBrowserCookieStoreKey key = page->creation_started
            ? page->store : agent_browser_cookie_store_key(cookie_mode, page->state.owner);
        const auto weak = weak_from_this();
        clear_store(key, [weak, key, page_id](bool ok, const std::string& clear_error) {
            const auto self = weak.lock();
            if (!self) return;
            if (ok) {
                std::vector<std::shared_ptr<Page>> reload;
                {
                    std::lock_guard<std::mutex> lock(self->state_mutex);
                    for (const auto& [id, candidate] : self->pages) {
                        if (!candidate->closing && candidate->webview &&
                            candidate->store == key) {
                            reload.push_back(candidate);
                        }
                    }
                }
                for (const auto& candidate : reload) candidate->webview->Reload();
                LOG_INFO("[agent-browser] browsing data cleared: " + key.id());
            } else {
                LOG_WARN("[agent-browser] failed to clear browsing data for " + key.id() +
                         ": " + clear_error);
            }
            AgentBrowserStorageEvent event;
            event.type = "browsing_data_cleared";
            event.ok = ok;
            event.store = key.id();
            event.page_id = page_id;
            event.error = clear_error;
            self->emit_storage_event(std::move(event));
        });
        return true;
    }

    // daemon 永久删除会话后:关闭该根会话的页面并删除它的按会话存储。本次运行没
    // 加载过的存储直接删目录;加载过的由 WebView2 删除 Profile(浏览器进程占着
    // 目录),状态里的记录留到下次启动删目录时再撤。
    void process_store_tasks_on_ui() {
        const AgentBrowserStoreState state = read_agent_browser_store_state();
        if (state.pending_deletions.empty()) return;
        const auto user_data_path = agent_browser_user_data_path(acecode_dir);
        std::vector<std::string> finished;
        for (const auto& session_id : state.pending_deletions) {
            for (const auto& page_id :
                 agent_browser_pages_of_root_session(page_store_bindings(), session_id)) {
                close_page_on_ui(page_id, nullptr, nullptr);
            }
            prepared_session_stores.erase(session_id);
            AgentBrowserCookieStoreKey key;
            key.kind = AgentBrowserCookieStoreKind::Session;
            key.session_id = session_id;
            if (loaded_store_ids.erase(key.id()) != 0) {
                delete_loaded_store(key);
                continue;
            }
            if (remove_session_profile_directory(user_data_path, session_id)) {
                finished.push_back(session_id);
            }
        }
        if (!finished.empty()) {
            update_agent_browser_store_state([&finished](AgentBrowserStoreState& current) {
                const auto drop = [&finished](std::vector<std::string>& values) {
                    values.erase(
                        std::remove_if(values.begin(), values.end(),
                                       [&finished](const std::string& value) {
                                           return std::find(finished.begin(), finished.end(),
                                                            value) != finished.end();
                                       }),
                        values.end());
                };
                drop(current.pending_deletions);
                drop(current.session_stores);
                drop(current.pending_wipe);
                return true;
            });
        }
        for (const auto& session_id : finished) {
            AgentBrowserStorageEvent event;
            event.type = "session_store_deleted";
            event.store = "session:" + session_id;
            emit_storage_event(std::move(event));
        }
    }

    void delete_loaded_store(const AgentBrowserCookieStoreKey& key) {
        const auto weak = weak_from_this();
        with_profile(key, [weak, key](const Microsoft::WRL::ComPtr<ICoreWebView2Profile>& profile,
                                      const std::string& error,
                                      const std::shared_ptr<ProfileLease>& lease) {
            const auto report = [weak, key](bool ok, const std::string& message) {
                const auto self = weak.lock();
                if (!self) return;
                AgentBrowserStorageEvent event;
                event.type = "session_store_deleted";
                event.ok = ok;
                event.store = key.id();
                event.error = message;
                self->emit_storage_event(std::move(event));
            };
            if (!profile) {
                LOG_WARN("[agent-browser] cannot open cookie store " + key.id() +
                         " for deletion: " + error);
                report(false, error);
                return;
            }
            Microsoft::WRL::ComPtr<ICoreWebView2Profile8> profile8;
            if (SUCCEEDED(profile.As(&profile8)) && profile8 &&
                SUCCEEDED(profile8->Delete())) {
                LOG_INFO("[agent-browser] deleted cookie store " + key.id());
                report(true, {});
                return;
            }
            // 老运行时没有 Profile 删除接口:先清空数据,目录留给下次启动删除。
            clear_profile(profile, lease, [key, report](bool ok, const std::string& message) {
                LOG_INFO("[agent-browser] cleared deleted cookie store " + key.id() +
                         (ok ? std::string() : ": " + message));
                report(ok, message);
            });
        });
    }

    void apply_bounds(const std::shared_ptr<Page>& page) {
        if (!page) return;
        const int width = (std::max)(0, page->requested_bounds.width);
        const int height = (std::max)(0, page->requested_bounds.height);
        const auto parent = static_cast<HWND>(parent_window);
        const bool parent_visible = parent && ::IsWindow(parent) &&
                                    ::IsWindowVisible(parent) &&
                                    !::IsIconic(parent);
        const bool requested_show = page->controller && page->state.active &&
                                    page->state.content_state ==
                                        kAgentBrowserContentStateLive &&
                                    page->requested_bounds.visible &&
                                    parent_surface_visible &&
                                    parent_visible &&
                                    width > 0 && height > 0;
        bool show = false;
        if (page->controller) {
            const POINT origin =
                requested_show ? main_web_content_origin(parent) : POINT{0, 0};
            const bool positioned = requested_show && browser_widget &&
                ::SetWindowPos(browser_widget,
                               HWND_TOP,
                               page->requested_bounds.x + origin.x,
                               page->requested_bounds.y + origin.y,
                               width,
                               height,
                               SWP_NOACTIVATE | SWP_SHOWWINDOW) != FALSE;
            const bool region_applied = positioned &&
                apply_agent_browser_widget_region(
                    browser_widget,
                    width,
                    height,
                    page->requested_bounds.occlusion_rects);
            if (region_applied) {
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
                if (positioned) {
                    LOG_WARN("[agent-browser] failed to apply native occlusion "
                             "region for page " + page->id);
                }
            }
        }
        if (page->state.active && !show) {
            hide_agent_browser_widget(browser_widget);
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
        if (!page || !page->webview || !page->store_prepared) {
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

    bool set_shared_with_agent_on_ui(const std::string& page_id,
                                     bool shared,
                                     std::string* error) {
        auto page = find_page(page_id);
        if (!page || page->closing) {
            assign_error(error, "Agent Browser page was not found");
            return false;
        }
        update_page(page, [shared](AgentBrowserState& state) {
            state.shared_with_agent = shared;
        });
        return true;
    }

    void finish_element_selection(const std::string& page_id,
                                  std::uint64_t generation,
                                  HRESULT result,
                                  LPCWSTR raw) {
        const auto page = find_page(page_id);
        if (!page) return;
        nlohmann::json selected;
        if (SUCCEEDED(result) && raw) {
            auto response = nlohmann::json::parse(
                acecode::wide_to_utf8(raw), nullptr, false);
            if (!response.is_discarded() && response.is_object()) {
                const auto remote = response.value(
                    "result", nlohmann::json::object());
                if (remote.is_object() && remote.contains("value") &&
                    remote["value"].is_object()) {
                    const auto& value = remote["value"];
                    if (!value.value("cancelled", true) &&
                        value.contains("element") &&
                        value["element"].is_object()) {
                        selected = value["element"];
                    }
                }
            }
        }

        AgentBrowserState snapshot;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (page->closing ||
                page->element_selection_generation != generation) {
                return;
            }
            page->state.element_selection_active = false;
            if (selected.is_object()) {
                ++page->state.element_selection_serial;
            }
            snapshot = page->state;
        }
        if (selected.is_object()) {
            snapshot.selected_element_json = selected.dump();
        }
        emit_state(snapshot);
    }

    bool toggle_element_selection_on_ui(const std::string& page_id,
                                        std::string* error) {
        auto page = find_page(page_id);
        if (!page || !page->webview || page->closing) {
            assign_error(error, "Agent Browser page is still starting");
            return false;
        }
        bool active = false;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            active = page->state.element_selection_active;
        }
        if (active) {
            const std::string cancel_expression =
                "(() => { const value = globalThis['" +
                std::string(kAgentBrowserElementPickerKey) +
                "']; if (value && typeof value.cancel === 'function') "
                "value.cancel(); return true; })()";
            const nlohmann::json params{
                {"expression", cancel_expression},
                {"returnByValue", true},
                {"userGesture", true},
            };
            const std::wstring wide_params = acecode::utf8_to_wide(params.dump());
            auto ignored = Microsoft::WRL::Callback<
                ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>(
                [](HRESULT, LPCWSTR) -> HRESULT { return S_OK; });
            const HRESULT dispatched = page->webview->CallDevToolsProtocolMethod(
                L"Runtime.evaluate", wide_params.c_str(), ignored.Get());
            if (FAILED(dispatched)) {
                assign_error(error, "failed to cancel element selection (" +
                                        hresult_text(dispatched) + ")");
                return false;
            }
            return true;
        }

        std::uint64_t generation = 0;
        AgentBrowserState snapshot;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            generation = ++page->element_selection_generation;
            page->state.element_selection_active = true;
            snapshot = page->state;
        }
        emit_state(snapshot);

        const nlohmann::json params{
            {"expression", agent_browser_element_picker_expression()},
            {"awaitPromise", true},
            {"returnByValue", true},
            {"userGesture", true},
        };
        const std::wstring wide_params = acecode::utf8_to_wide(params.dump());
        const auto weak = weak_from_this();
        auto completed = Microsoft::WRL::Callback<
            ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>(
            [weak, page_id, generation](HRESULT result, LPCWSTR raw) -> HRESULT {
                if (const auto self = weak.lock()) {
                    self->finish_element_selection(
                        page_id, generation, result, raw);
                }
                return S_OK;
            });
        const HRESULT dispatched = page->webview->CallDevToolsProtocolMethod(
            L"Runtime.evaluate", wide_params.c_str(), completed.Get());
        if (FAILED(dispatched)) {
            finish_element_selection(page_id, generation, dispatched, nullptr);
            assign_error(error, "failed to start element selection (" +
                                    hresult_text(dispatched) + ")");
            return false;
        }
        return true;
    }

    bool open_developer_tools_on_ui(const std::string& page_id,
                                    std::string* error) {
        auto page = find_page(page_id);
        if (!page || !page->webview || page->closing) {
            assign_error(error, "Agent Browser page is still starting");
            return false;
        }
        const HRESULT result = page->webview->OpenDevToolsWindow();
        if (FAILED(result)) {
            assign_error(error, "failed to open WebView2 developer tools (" +
                                    hresult_text(result) + ")");
            return false;
        }
        return true;
    }

    void call_cdp_on_ui(
        const std::string& requested_page,
        const AgentBrowserPageOwner& owner,
        const std::string& method,
        const nlohmann::json& params,
        const std::shared_ptr<PendingProxyCall>& pending) {
        const std::string page_id =
            resolve_agent_page_id(requested_page, owner, true);
        auto page = find_page(page_id);
        if (!page || page->closing) {
            finish_proxy_call(pending,
                              {{"ok", false},
                               {"page_id", page_id},
                               {"error", "Agent Browser page was not found"}});
            return;
        }
        if (!require_agent_shared_page(page, page_id, pending)) return;
        if (!page->webview || !page->store_prepared) {
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
        const AgentBrowserPageOwner owner = parse_agent_browser_page_owner(
            request.contains("owner") ? request["owner"] : nlohmann::json());
        if (operation == "drain_store_tasks") {
            // 提醒本身不带数据,要做的事都在状态文件里(daemon 写入的待删除会话)。
            process_store_tasks_on_ui();
            finish_proxy_call(pending, {{"ok", true}, {"result", nlohmann::json::object()}});
            return;
        }
        if (operation == "create_page") {
            const std::string page_id = create_page_on_ui(true, owner, true);
            finish_proxy_call(pending,
                              {{"ok", true},
                               {"page_id", page_id},
                               {"result", {{"page_id", page_id}}}});
            return;
        }
        if (operation == "claim_page") {
            const std::string page_id =
                resolve_agent_page_id(requested_page, owner, true);
            const auto page = find_page(page_id);
            if (!page) {
                finish_proxy_call(pending,
                                  {{"ok", false},
                                   {"page_id", page_id},
                                   {"error", "Agent Browser page was not found"}});
            } else if (require_agent_shared_page(page, page_id, pending)) {
                set_agent_target_on_ui(owner, page_id);
                finish_proxy_call(pending,
                                  {{"ok", true},
                                   {"page_id", page_id},
                                   {"result", {{"page_id", page_id}}}});
            }
            return;
        }
        if (operation == "close_page") {
            const std::string page_id =
                resolve_agent_page_id(requested_page, owner, false);
            const auto page = page_id.empty() ? nullptr : find_page(page_id);
            if (!page) {
                finish_proxy_call(pending,
                                  {{"ok", false},
                                   {"page_id", page_id},
                                   {"error", "Agent Browser page was not found"}});
                return;
            }
            if (!require_agent_shared_page(page, page_id, pending)) return;
            std::string closed_page;
            std::string error;
            if (!close_page_on_ui(page_id, &closed_page, &error)) {
                finish_proxy_call(pending,
                                  {{"ok", false},
                                   {"page_id", page_id},
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
            const auto page = find_page(requested_page);
            if (!page) {
                finish_proxy_call(pending,
                                  {{"ok", false},
                                   {"page_id", requested_page},
                                   {"error", "Agent Browser page was not found"}});
                return;
            }
            if (!require_agent_shared_page(page, requested_page, pending)) return;
            std::string error;
            // 有归属的显式选页只钉该会话的默认目标,不改变显示页;旧协议沿用
            // 「选页即显示」。
            const bool selected = owner.empty()
                ? select_page_on_ui(requested_page, &error)
                : (set_agent_target_on_ui(owner, requested_page), true);
            if (!selected) {
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
            owner,
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
            operation != "close_page" && operation != "select_page" &&
            operation != "drain_store_tasks") {
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
                                   DispatchHandler dispatch_handler,
                                   std::string acecode_dir)
    : impl_(std::make_shared<Impl>(parent_window,
                                  desktop_pid,
                                  std::move(desktop_instance_id),
                                  std::move(state_handler),
                                  std::move(dispatch_handler),
                                  std::move(acecode_dir))) {
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

std::vector<AgentBrowserState> AgentBrowserHost::states(
    const std::string& owner_session_id) const {
    return impl_ ? impl_->states(owner_session_id)
                 : std::vector<AgentBrowserState>{};
}

std::string AgentBrowserHost::active_page_id() const {
    return impl_ ? impl_->active_page_id() : std::string{};
}

std::string AgentBrowserHost::create_page(std::string* error,
                                          const AgentBrowserPageOwner& owner) {
#ifdef _WIN32
    if (impl_ && impl_->state().supported) {
        return impl_->create_page_on_ui(false, owner, false);
    }
#else
    (void)owner;
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
    if (bounds.occlusion_rects.size() > kAgentBrowserMaxOcclusionRects) {
        assign_error(error, "Agent Browser has too many occlusion rectangles");
        return false;
    }
    for (const auto& rect : bounds.occlusion_rects) {
        if (rect.x < 0 || rect.y < 0 || rect.width <= 0 || rect.height <= 0 ||
            rect.x > 32768 || rect.y > 32768 ||
            rect.width > 32768 || rect.height > 32768) {
            assign_error(error, "Agent Browser occlusion rectangle is invalid");
            return false;
        }
    }
#ifdef _WIN32
    auto page = impl_->find_page(page_id);
    if (!page) {
        assign_error(error, "Agent Browser page was not found");
        return false;
    }
    if (bounds.layout_revision != 0 &&
        page->requested_bounds.layout_revision != 0 &&
        bounds.layout_revision < page->requested_bounds.layout_revision) {
        return true;
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

bool AgentBrowserHost::set_shared_with_agent(const std::string& page_id,
                                              bool shared,
                                              std::string* error) {
#ifdef _WIN32
    return impl_ && impl_->set_shared_with_agent_on_ui(page_id, shared, error);
#else
    (void)page_id;
    (void)shared;
    assign_error(error, "Agent Browser is unavailable on this platform");
    return false;
#endif
}

bool AgentBrowserHost::toggle_element_selection(const std::string& page_id,
                                                 std::string* error) {
#ifdef _WIN32
    return impl_ && impl_->toggle_element_selection_on_ui(page_id, error);
#else
    (void)page_id;
    assign_error(error, "Agent Browser is unavailable on this platform");
    return false;
#endif
}

std::string AgentBrowserHost::console_logs(const std::string& page_id,
                                            std::string* error) const {
#ifdef _WIN32
    const auto page = impl_ ? impl_->find_page(page_id) : nullptr;
    if (page && !page->closing) return impl_->console_logs_for_page(page);
    assign_error(error, "Agent Browser page was not found");
#else
    (void)page_id;
    assign_error(error, "Agent Browser is unavailable on this platform");
#endif
    return {};
}

bool AgentBrowserHost::open_developer_tools(const std::string& page_id,
                                             std::string* error) {
#ifdef _WIN32
    return impl_ && impl_->open_developer_tools_on_ui(page_id, error);
#else
    (void)page_id;
    assign_error(error, "Agent Browser is unavailable on this platform");
    return false;
#endif
}

void AgentBrowserHost::refresh_layout() {
    if (!impl_) return;
#ifdef _WIN32
    for (const auto& state : impl_->states()) {
        if (auto page = impl_->find_page(state.page_id)) {
            impl_->apply_bounds(page);
        }
    }
#endif
}

void AgentBrowserHost::set_parent_visible(bool visible) {
    if (!impl_) return;
    impl_->parent_surface_visible = visible;
    refresh_layout();
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

std::string AgentBrowserHost::cookie_mode() const {
    return agent_browser_cookie_mode_name(
        impl_ ? impl_->cookie_mode : AgentBrowserCookieMode::Shared);
}

bool AgentBrowserHost::set_cookie_mode(const std::string& mode, std::string* error) {
#ifdef _WIN32
    return impl_ && impl_->set_cookie_mode_on_ui(mode, error);
#else
    (void)mode;
    assign_error(error, "Agent Browser is unavailable on this platform");
    return false;
#endif
}

bool AgentBrowserHost::clear_browsing_data(const std::string& page_id,
                                           std::string* error) {
#ifdef _WIN32
    return impl_ && impl_->clear_browsing_data_on_ui(page_id, error);
#else
    (void)page_id;
    assign_error(error, "Agent Browser is unavailable on this platform");
    return false;
#endif
}

void AgentBrowserHost::process_store_tasks() {
#ifdef _WIN32
    if (impl_) impl_->process_store_tasks_on_ui();
#endif
}

void AgentBrowserHost::set_storage_event_handler(StorageEventHandler handler) {
    if (impl_) impl_->storage_handler = std::move(handler);
}

} // namespace acecode::desktop
