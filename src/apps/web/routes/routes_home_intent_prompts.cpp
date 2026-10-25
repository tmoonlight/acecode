#include "web/server_impl.hpp"

#include "platform/locale.hpp"
#include "provider/provider_factory.hpp"
#include "provider/copilot_provider.hpp"
#include "session/session_recent_activity.hpp"
#include "utils/abandonable_call.hpp"
#include "utils/joining_thread.hpp"
#include "utils/logger.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <unordered_map>

namespace acecode::web {
using nlohmann::json;

namespace {
const char* provider_error_kind_name(ProviderErrorKind kind) {
    switch (kind) {
    case ProviderErrorKind::None: return "none";
    case ProviderErrorKind::UserCancelled: return "cancelled";
    case ProviderErrorKind::Timeout: return "timeout";
    case ProviderErrorKind::Network: return "network";
    case ProviderErrorKind::Http: return "http";
    case ProviderErrorKind::MalformedSse: return "malformed_sse";
    case ProviderErrorKind::MalformedJson: return "malformed_json";
    case ProviderErrorKind::Unknown: break;
    }
    return "unknown";
}

void attach_live_session(GlobalSessionCatalogEntry& row,
                         const std::unordered_map<std::string, SessionInfo>& live_sessions) {
    const auto live = live_sessions.find(global_session_catalog_identity(
        row.meta.no_workspace, row.workspace_hash, row.meta.id));
    if (live != live_sessions.end()) row.active = live->second;
}
} // namespace

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
        const bool force = body.value("force", false);
        auto cancellation = home_intent_prompts.begin(force, now_unix_ms());
        if (!cancellation) return respond(req, 200, home_intent_prompts.snapshot());
        // Shutdown and the model deadline set `cancellation`; preparation stops on
        // its own deadline and keeps what it found. No async closure borrows Impl,
        // session services, config locks or a live conversation provider.
        const auto prepare_expired = std::make_shared<std::atomic<bool>>(false);
        JoiningThread prepare_timeout([prepare_expired](StopToken stop) {
            if (!stop.wait_for(std::chrono::milliseconds(kHomeIntentPrepareBudgetMs))) prepare_expired->store(true);
        });
        const auto stop_preparing = [cancellation, prepare_expired] {
            return cancellation->load() || prepare_expired->load();
        };
        const auto started = std::chrono::steady_clock::now();
        const auto since = [](std::chrono::steady_clock::time_point from) {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - from).count();
        };
        // Diagnostics only: counts, timings and codes, never transcript or provider text.
        json report = {{"force", force}};
        std::string phase = "discover";
        json suggestions = json::array();
        std::string model_name;
        std::string error;
        try {
            AppConfig config;
            {
                std::shared_lock<std::shared_mutex> lock(app_config_mu);
                config = *deps.app_config;
            }
            const auto projects = discover_global_session_projects(projects_dir(), stop_preparing);
            report["projects"] = projects.size();
            report["discover_ms"] = since(started);
            phase = "load";
            std::unordered_map<std::string, SessionInfo> live_sessions;
            for (auto& live : deps.session_client->list_sessions()) {
                auto identity = global_session_catalog_identity(live.no_workspace, live.workspace_hash, live.id);
                live_sessions.emplace(std::move(identity), std::move(live));
            }
            // Reuse the prewarmed index where it still matches the directory; a
            // directory written seconds ago may hide a change inside one timestamp tick.
            const auto index = global_session_search;
            const auto listed_at_ms = now_unix_ms();
            const auto cached_projects = std::make_shared<std::atomic<std::size_t>>(0);
            std::vector<HomeIntentProject> candidates_by_project;
            candidates_by_project.reserve(projects.size());
            for (const auto& project : projects) {
                HomeIntentProject item;
                item.modified_ms = project.modified_ms;
                item.open = [index, project, listed_at_ms, cached_projects, stop_preparing, &live_sessions] {
                    HomeIntentProjectContents contents;
                    if (index && listed_at_ms - project.modified_ms > 10'000) {
                        if (auto rows = index->cached_project_entries(project.project_dir, project.signature)) {
                            cached_projects->fetch_add(1);
                            for (auto& row : *rows) attach_live_session(row, live_sessions);
                            contents.rows = std::move(*rows);
                            return contents;
                        }
                    }
                    // A large project is listed, not read: only its newest files can matter.
                    const auto reader = std::make_shared<const GlobalSessionProjectReader>(project.project_dir);
                    for (auto& stamp : reader->list(stop_preparing)) {
                        HomeIntentSessionFile file;
                        file.modified_ms = stamp.modified_ms;
                        file.read = [reader, path = std::move(stamp.meta_path), &live_sessions] {
                            auto row = reader->read(path);
                            if (row) attach_live_session(*row, live_sessions);
                            return row;
                        };
                        contents.files.push_back(std::move(file));
                    }
                    return contents;
                };
                candidates_by_project.push_back(std::move(item));
            }
            // Old metadata did not persist human recency. Reconstruct it from the
            // transcript without treating a rename or title write as user activity,
            // and only for sessions that can still rank among the needed ones.
            std::unordered_map<std::string, std::string> transcript_activity;
            const HomeIntentActivityReader read_activity =
                [&transcript_activity, stop_preparing](const GlobalSessionCatalogEntry& entry) -> std::optional<std::string> {
                if (stop_preparing()) return std::nullopt;
                if (entry.active && entry.active->busy) return std::string{}; // Transcript is being written.
                const auto path = SessionStorage::session_path(entry.project_dir, entry.meta.id);
                auto [cached, inserted] = transcript_activity.try_emplace(path);
                if (inserted) cached->second = read_session_recent_activity(path).last_user_message_at;
                return cached->second;
            };
            // Sessions come only from the projects that can still matter, newest
            // directory first; archived sessions are not candidates (as in the index).
            auto candidates = load_home_intent_candidates(
                config, std::move(candidates_by_project), read_activity, stop_preparing);
            auto& catalog = candidates.catalog;
            auto profile = resolve_home_intent_model(config, catalog, read_activity);
            const auto current = deps.session_client->list_sessions();
            // Refresh busy state after loading, before reading source transcripts.
            for (auto& entry : catalog.entries) {
                for (const auto& live : current) {
                    if (global_session_catalog_identity(live.no_workspace, live.workspace_hash, live.id) ==
                        global_session_catalog_identity(entry.meta.no_workspace, entry.workspace_hash, entry.meta.id)) {
                        entry.active = live;
                        break;
                    }
                }
            }
            const auto selected = select_home_intent_sessions(catalog, read_activity);
            report["loaded_projects"] = candidates.loaded_projects;
            report["cached_projects"] = cached_projects->load();
            report["read_files"] = candidates.read_files;
            report["sessions"] = catalog.entries.size();
            report["transcript_reads"] = transcript_activity.size();
            report["load_ms"] = since(started);
            if (!candidates.settled) report["prepare_expired"] = prepare_expired->load();
            prepare_timeout.request_stop();
            phase = "sources";
            json sources = json::array();
            for (const auto& entry : selected) {
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
            report["sources"] = sources.size();
            if (cancellation->load()) error = "TIMEOUT";
            else if (sources.empty() && !candidates.settled) error = "TIMEOUT"; // Nothing found in the budget.
            else if (!sources.empty() && !profile) error = "MODEL_UNAVAILABLE";
            else if (!sources.empty()) {
                phase = "model";
                const auto model_started = std::chrono::steady_clock::now();
                // The model budget starts here, whatever preparation cost.
                JoiningThread model_timeout([cancellation](StopToken stop) {
                    if (!stop.wait_for(std::chrono::milliseconds(kHomeIntentModelBudgetMs))) cancellation->store(true);
                });
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
                model_timeout.request_stop();
                report["model_ms"] = since(model_started);
                if (!response || cancellation->load()) error = "TIMEOUT";
                else if (response->finish_reason == "error" || response->has_tool_calls()) {
                    error = "GENERATION_FAILED";
                    report["tool_calls"] = response->has_tool_calls();
                    report["provider_error"] = provider_error_kind_name(response->provider_error.kind);
                    report["http_status"] = response->provider_error.status_code;
                } else if (auto parsed = parse_home_intent_prompts(response->content, sources)) {
                    suggestions = std::move(*parsed);
                } else {
                    error = "INVALID_RESPONSE";
                    report["response_bytes"] = response->content.size();
                }
            }
        } catch (...) {
            // Provider diagnostics may contain credentials or transcript text.
            error = "GENERATION_FAILED";
            report["exception"] = true;
        }
        prepare_timeout.request_stop();
        home_intent_prompts.finish(cancellation, suggestions, model_name, error, now_unix_ms());
        report["result"] = error.empty() ? "ok" : error;
        if (!error.empty()) report["phase"] = phase;
        report["model"] = model_name;
        report["suggestions"] = suggestions.size();
        report["total_ms"] = since(started);
        const auto line = "[home-intent] " + report.dump(-1, ' ', false, json::error_handler_t::replace);
        if (error.empty()) LOG_INFO(line);
        else LOG_WARN(line);
        return respond(req, 200, home_intent_prompts.snapshot());
    });
}
} // namespace acecode::web
