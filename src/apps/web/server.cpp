// server.cpp — WebServer orchestrator.
// All helper implementations are in server_helpers.cpp.
// All register_*() method bodies are in routes/routes_*.cpp.
// This file contains only: Impl state definition, register_routes(),
// WebServer public methods (ctor, dtor, run, stop).

#include "server_impl.hpp"

#include "http_address.hpp"
#include "remote_web_proxy.hpp"

namespace acecode::web {

using nlohmann::json;

namespace {

// Crow 自带的 CerrLogHandler 只写 stderr。Desktop 拉起 daemon 时 stderr 已经
// 重定向到 NUL,Crow 记下的告警(路由里逃逸的异常、socket 错误)就此蒸发 ——
// 用户看到裸 500,daemon-*.log 里却一行线索都没有。桥到 ACECode Logger 后
// 落进同一个日志文件。Info/Debug 丢弃:Crow 每个响应都打一行 Info,侧边栏
// 轮询会把日志刷爆;启动横幅由 `[web] Web UI:` 那行覆盖。
class CrowLogBridge final : public crow::ILogHandler {
public:
    void log(const std::string& message, crow::LogLevel level) override {
        switch (level) {
            case crow::LogLevel::Warning:
                LOG_WARN("[crow] " + message);
                break;
            case crow::LogLevel::Error:
            case crow::LogLevel::Critical:
                LOG_ERROR("[crow] " + message);
                break;
            default:
                break;
        }
    }
};

void install_crow_log_bridge() {
    // Crow 的 handler / level 是进程级静态量,多个 WebServer 实例(单测)共用。
    static CrowLogBridge bridge;
    static std::once_flag once;
    std::call_once(once, [] {
        crow::logger::setHandler(&bridge);
        crow::logger::setLogLevel(crow::LogLevel::Warning);
    });
}

// 路由 handler 逃逸的异常兜底。Crow 在 catch(...) 里回调本函数,所以这里可以
// `throw;` 重新抛出当前异常来分类。默认实现只回一个空 body 的 500 并把原因写
// stderr;这里改成记 ERROR 日志 + JSON body,前端 toast 与日志两边都能看到原因。
void route_exception_handler(crow::response& res) {
    res = crow::response(500);
    try {
        throw;
    } catch (const crow::bad_request& e) {
        // 与 Crow 默认实现一致:请求格式错误回 400。
        res = crow::response(400);
        res.body = e.what();
    } catch (const std::exception& e) {
        const std::string message = ensure_utf8(e.what());
        LOG_ERROR("[web] uncaught exception in route handler: " + message);
        res.body = json{{"error", "INTERNAL_ERROR"}, {"message", message}}.dump();
        res.add_header("Content-Type", "application/json");
    } catch (...) {
        LOG_ERROR("[web] uncaught non-std exception in route handler");
        res.body = R"({"error":"INTERNAL_ERROR","message":"unknown exception"})";
        res.add_header("Content-Type", "application/json");
    }
}

} // namespace

WebServer::Impl::~Impl() {
    const bool already_stopping = shutdown_requested.exchange(true);
    if (model_reasoning_sync) model_reasoning_sync->stop();
    if (global_session_search) global_session_search->stop();
    stop_side_chat_workers();
    if (!already_stopping) {
        std::lock_guard<std::mutex> stop_lock(listener_stop_mu);
        app.stop();
    }
    // 先阻止 tracked-subagent producer 再停 flusher。若先停 flusher，
    // 尚未解除的订阅仍可能标脏，却再也没有线程负责落盘。
    if (subagent_tracker_state) {
        std::lock_guard<std::mutex> lk(subagent_tracker_state->mu);
        subagent_tracker_state->impl = nullptr;
    }

    std::vector<std::pair<std::string, SessionClient::SubscriptionId>> subs;
    {
        std::lock_guard<std::mutex> lk(tracked_subagents_mu);
        subs.reserve(tracked_subagent_subscriptions.size());
        for (const auto& [sid, sub] : tracked_subagent_subscriptions) {
            subs.emplace_back(sid, sub);
        }
        tracked_subagent_subscriptions.clear();
    }
    if (deps.session_client) {
        for (const auto& [sid, sub] : subs) {
            deps.session_client->unsubscribe(sid, sub);
        }
    }

    // 所有已知 attention producer 均已停用，最后一次 flush 不会再漏掉
    // 在 shutdown / unsubscribe 期间到达的事件。
    stop_attention_flusher();
}

// =====================================================================
// register_routes — dispatches to each domain's register_*()
// =====================================================================
void WebServer::Impl::register_routes() {
    register_health();
    register_usage();
    register_workspaces();
    register_pinned_sessions();
    register_sessions();
    register_task_suggestions();
    register_models();
    register_image_generation();
    register_computer_use();
    register_summary_generation();
    register_tool_rewrites();
    register_tool_preamble();
    register_security();
    register_experts();
    register_loops();
    register_ui_preferences();
    register_themes();
    register_history();
    register_files();
    register_fs();
    register_git();
    register_lsp();
    register_skills();
    register_commands();
    register_mcp();
    register_hooks();
    register_feedback();
    register_pty();
    register_environment();
    register_websocket();
    register_static();
}

// =====================================================================
// WebServer public methods
// =====================================================================
WebServer::WebServer(WebServerDeps deps)
    : impl_(std::make_unique<Impl>(std::move(deps))) {
    try {
        std::string dir = impl_->deps.web_cfg ? impl_->deps.web_cfg->static_dir : std::string{};
        impl_->assets = make_asset_source(dir);
    } catch (const std::exception& e) {
        LOG_ERROR(std::string("[web] failed to init asset source: ") + e.what());
    }
    install_crow_log_bridge();
    impl_->app.exception_handler(&route_exception_handler);
    impl_->register_routes();
    impl_->initialize_model_reasoning_sync();
}

WebServer::~WebServer() = default;

int WebServer::run() {
    if (!impl_->deps.web_cfg) {
        LOG_ERROR("[web] missing web_cfg");
        return 1;
    }
    WebConfig cfg;
    {
        std::shared_lock<std::shared_mutex> config_lock(impl_->app_config_mu);
        cfg = *impl_->deps.web_cfg;
    }
    cfg.port = impl_->runtime_port;

    auto preflight = preflight_bind_check(
        cfg.bind,
        impl_->deps.token,
        impl_->deps.dangerous);
    if (!preflight.empty()) {
        LOG_ERROR("[web] " + preflight);
        return 2;
    }

    {
        std::lock_guard<std::mutex> lock(impl_->listener_state_mu);
        impl_->effective_bind = cfg.bind;
        impl_->effective_port = cfg.port;
    }
    LOG_INFO(
        "[web] Web UI: " + format_http_address(cfg.bind, cfg.port));
    impl_->request_model_reasoning_sync();
    try {
        impl_->app
            .bindaddr(cfg.bind)
            .port(static_cast<std::uint16_t>(cfg.port))
            .multithreaded()
            .run();
    } catch (const std::exception& e) {
        LOG_ERROR(std::string("[web] server crashed: ") + e.what());
        LOG_ERROR("[web] port " + std::to_string(cfg.port) +
                  " may be in use — change web.port in config.json or stop "
                  "the conflicting process; daemon will not retry");
        return 3;
    }
    return 0;
}

void WebServer::stop() {
    if (!impl_) return;
    if (impl_->shutdown_requested.exchange(true)) return;
    if (impl_->model_reasoning_sync) impl_->model_reasoning_sync->stop();
    impl_->stop_side_chat_workers();
    std::lock_guard<std::mutex> stop_lock(impl_->listener_stop_mu);
    impl_->app.stop();
}

void WebServer::track_subagent(const std::string& child_session_id) {
    if (impl_) impl_->track_subagent(child_session_id);
}

void WebServer::refresh_saved_models_from_disk() {
    if (impl_) impl_->refresh_saved_models_from_disk();
}

void WebServer::with_app_config_lock(const std::function<void()>& fn) const {
    if (!fn) return;
    if (!impl_) {
        fn();
        return;
    }
    std::lock_guard<std::shared_mutex> lock(impl_->app_config_mu);
    fn();
}

void WebServer::broadcast_remote_control_session_selected(
    const std::string& session_id,
    const std::string& workspace_hash,
    const std::string& cwd,
    bool no_workspace,
    const std::string& title,
    const std::string& updated_at) {
    if (!impl_) return;
    impl_->broadcast_remote_control_session_selected(
        session_id, workspace_hash, cwd, no_workspace, title, updated_at);
}

} // namespace acecode::web
