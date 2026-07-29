// server.cpp — WebServer orchestrator.
// All helper implementations are in server_helpers.cpp.
// All register_*() method bodies are in routes/routes_*.cpp.
// This file contains only: Impl state definition, register_routes(),
// WebServer public methods (ctor, dtor, run, stop).

#include "server_impl.hpp"

namespace acecode::web {

using nlohmann::json;

WebServer::Impl::~Impl() {
    if (!shutdown_requested.exchange(true)) {
        std::lock_guard<std::mutex> stop_lock(listener_stop_mu);
        app.stop();
    }
    join_rebind_stop_thread();

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

void WebServer::Impl::request_listener_rebind() {
    rebind_requested.store(true);
    std::lock_guard<std::mutex> lock(rebind_thread_mu);
    if (rebind_stop_thread.joinable()) return;
    rebind_stop_thread = std::thread([this] {
        // Let Crow finish writing the mutation response before its acceptor and
        // active sockets are stopped.
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        if (shutdown_requested.load()) return;
        std::lock_guard<std::mutex> stop_lock(listener_stop_mu);
        if (!shutdown_requested.load()) app.stop();
    });
}

void WebServer::Impl::join_rebind_stop_thread() {
    std::thread pending;
    {
        std::lock_guard<std::mutex> lock(rebind_thread_mu);
        if (rebind_stop_thread.joinable()) {
            pending = std::move(rebind_stop_thread);
        }
    }
    if (pending.joinable()) pending.join();
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
    register_models();
    register_experts();
    register_loops();
    register_ui_preferences();
    register_history();
    register_files();
    register_git();
    register_lsp();
    register_skills();
    register_commands();
    register_mcp();
    register_hooks();
    register_feedback();
    register_pty();
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
    impl_->register_routes();
}

WebServer::~WebServer() = default;

int WebServer::run() {
    if (!impl_->deps.web_cfg) {
        LOG_ERROR("[web] missing web_cfg");
        return 1;
    }
    while (!impl_->shutdown_requested.load()) {
        WebConfig cfg;
        {
            std::lock_guard<std::mutex> config_lock(impl_->app_config_mu);
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
            "[web] listening on " + cfg.bind + ":" +
            std::to_string(cfg.port));
        try {
            impl_->app
                .bindaddr(cfg.bind)
                .port(static_cast<std::uint16_t>(cfg.port))
                .multithreaded()
                .run();
        } catch (const std::exception& e) {
            impl_->join_rebind_stop_thread();
            LOG_ERROR(std::string("[web] server crashed: ") + e.what());
            LOG_ERROR("[web] port " + std::to_string(cfg.port) +
                      " may be in use — change web.port in config.json or stop "
                      "the conflicting process; daemon will not retry");
            return 3;
        }

        impl_->join_rebind_stop_thread();
        if (impl_->shutdown_requested.load()) break;
        if (!impl_->rebind_requested.exchange(false)) break;
    }
    return 0;
}

void WebServer::stop() {
    if (!impl_) return;
    if (impl_->shutdown_requested.exchange(true)) return;
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
    std::lock_guard<std::mutex> lock(impl_->app_config_mu);
    fn();
}

} // namespace acecode::web
