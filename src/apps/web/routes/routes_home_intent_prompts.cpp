#include "web/server_impl.hpp"

#include "platform/locale.hpp"
#include "provider/provider_factory.hpp"
#include "provider/copilot_provider.hpp"
#include "session/session_recent_activity.hpp"
#include "utils/abandonable_call.hpp"
#include "utils/joining_thread.hpp"

namespace acecode::web {
using nlohmann::json;

void WebServer::Impl::register_home_intent_prompts() {
    const auto respond = [this](const crow::request& req, int status, const json& value) {
        crow::response response(status);
        response.add_header("Content-Type", "application/json");
        response.add_header("Cache-Control", "no-store");
        response.body = value.dump();
        return with_cors(req, std::move(response));
    };
    CROW_ROUTE(app, "/api/home/intent-prompts").methods(crow::HTTPMethod::Options)
    ([this](const crow::request& req) { return cors_preflight(req); });
    CROW_ROUTE(app, "/api/home/intent-prompts").methods(crow::HTTPMethod::GET)
    ([this, respond](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        return respond(req, 200, home_intent_prompts.snapshot());
    });
    CROW_ROUTE(app, "/api/home/intent-prompts").methods(crow::HTTPMethod::POST)
    ([this, respond](const crow::request& req) {
        if (auto rejected = require_auth(req)) return std::move(*rejected);
        if (!deps.app_config || !deps.session_client || shutdown_requested.load())
            return respond(req, 503, {{"error", "UNAVAILABLE"}});
        const auto body = json::parse(req.body, nullptr, false);
        if (!body.is_object() || (body.contains("force") && !body["force"].is_boolean()))
            return respond(req, 400, {{"error", "BAD_REQUEST"}});
        auto cancellation = home_intent_prompts.begin(body.value("force", false), now_unix_ms());
        if (!cancellation) return respond(req, 200, home_intent_prompts.snapshot());
        // The timeout and provider worker own this flag. No async closure borrows
        // Impl, session services, config locks or a live conversation provider.
        JoiningThread timeout([cancellation](StopToken stop) {
            if (!stop.wait_for(std::chrono::seconds(60))) cancellation->store(true);
        });
        json suggestions = json::array();
        std::string model_name;
        std::string error;
        try {
            AppConfig config;
            {
                std::shared_lock<std::shared_mutex> lock(app_config_mu);
                config = *deps.app_config;
            }
            GlobalSessionCatalogOptions options;
            options.include_archived = true; // Model fallback can come from an archived session.
            options.should_cancel = [cancellation] { return cancellation->load(); };
            auto catalog = build_global_session_catalog(projects_dir(), deps.session_client->list_sessions(), options);
            // Old metadata did not persist human recency. Reconstruct it without
            // treating a rename or a background title write as user activity.
            for (auto& entry : catalog.entries) {
                if (cancellation->load()) break;
                if (entry.meta.last_user_message_at.empty() && !(entry.active && entry.active->busy)) {
                    entry.meta.last_user_message_at = read_session_recent_activity(
                        SessionStorage::session_path(entry.project_dir, entry.meta.id)).last_user_message_at;
                }
            }
            auto profile = resolve_home_intent_model(config, catalog);
            const auto current = deps.session_client->list_sessions();
            // Refresh busy state after disk discovery, before reading transcripts.
            for (auto& entry : catalog.entries) {
                for (const auto& live : current) {
                    if (global_session_catalog_identity(live.no_workspace, live.workspace_hash, live.id) ==
                        global_session_catalog_identity(entry.meta.no_workspace, entry.workspace_hash, entry.meta.id)) {
                        entry.active = live;
                        break;
                    }
                }
            }
            json sources = json::array();
            for (const auto& entry : select_home_intent_sessions(catalog)) {
                if (cancellation->load()) break;
                sources.push_back(home_intent_source(entry));
            }
            // Exclude a source that became busy while its snapshot was being read.
            const auto latest = deps.session_client->list_sessions();
            sources.erase(std::remove_if(sources.begin(), sources.end(), [&](const json& source) {
                return std::any_of(latest.begin(), latest.end(), [&](const SessionInfo& live) {
                    return live.busy && live.id == source["session_id"] &&
                        (live.no_workspace ? source["workspace"]["noWorkspace"].get<bool>()
                                           : live.workspace_hash == source["workspace"]["hash"]);
                });
            }), sources.end());
            if (cancellation->load()) error = "TIMEOUT";
            else if (!sources.empty() && !profile) error = "MODEL_UNAVAILABLE";
            else if (!sources.empty()) {
                model_name = profile->name;
                const auto locale = desktop::resolve_ui_locale(config.ui.locale, desktop::detect_system_locale_tag());
                auto messages = home_intent_messages(sources, locale);
                const auto response = run_abandonable<ChatResponse>(
                    [profile = *profile, config = std::move(config), messages = std::move(messages), cancellation]() mutable {
                        auto provider = create_provider_from_entry(profile, &config);
                        if (!provider) throw std::runtime_error("provider unavailable");
                        if (auto copilot = std::dynamic_pointer_cast<CopilotProvider>(provider)) copilot->try_silent_auth();
                        // Compaction's tool-free adapter also supports providers
                        // which need a special path for requests without tools.
                        return provider->chat_for_compaction(messages, {}, cancellation.get());
                    }, cancellation.get());
                if (!response || cancellation->load()) error = "TIMEOUT";
                else if (response->finish_reason == "error" || response->has_tool_calls()) error = "GENERATION_FAILED";
                else if (auto parsed = parse_home_intent_prompts(response->content, sources)) suggestions = std::move(*parsed);
                else error = "INVALID_RESPONSE";
            }
        } catch (...) {
            // Provider diagnostics may contain credentials or transcript text.
            error = "GENERATION_FAILED";
        }
        timeout.request_stop();
        home_intent_prompts.finish(cancellation, suggestions, model_name, error, now_unix_ms());
        return respond(req, 200, home_intent_prompts.snapshot());
    });
}
} // namespace acecode::web
