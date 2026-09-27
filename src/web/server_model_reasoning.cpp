#include "server_impl.hpp"

#include "config/config_mutation.hpp"
#include "config/saved_models_revision.hpp"

namespace acecode::web {

void WebServer::Impl::initialize_model_reasoning_sync() {
    if (!deps.app_config || !deps.sync_model_reasoning) return;
    model_reasoning_sync = std::make_unique<SavedModelReasoningSync>(
        [this] {
            std::shared_lock<std::shared_mutex> lock(app_config_mu);
            return deps.app_config->saved_models;
        },
        [this](const ModelReasoningSyncBatch& batch, const ParsedOpenAiModels& discovered) {
            if (shutdown_requested.load()) return;
            std::vector<std::string> changed_names;
            {
                std::lock_guard<std::shared_mutex> lock(app_config_mu);
                if (shutdown_requested.load()) return;
                const auto result = mutate_config(
                    [&](AppConfig& candidate, std::string& error) {
                        const auto before_merge = candidate.saved_models;
                        if (!merge_model_reasoning_sync(
                                candidate.saved_models, batch.profiles, discovered)) return false;
                        for (std::size_t i = 0; i < before_merge.size(); ++i) {
                            if (!model_profiles_equal(before_merge[i], candidate.saved_models[i])) {
                                changed_names.push_back(candidate.saved_models[i].name);
                            }
                        }
                        return validate_saved_models(candidate.saved_models,
                                                     candidate.default_model_name, error);
                    }, deps.config_path, deps.app_config);
                if (!result.ok || !result.changed) return;
                publish_live_saved_models(*deps.app_config, result.config.saved_models);
            }
            // Reuse the binding's idle/busy transition gate; never interrupt a turn.
            if (deps.session_registry && deps.session_client) {
                for (const auto& session : deps.session_client->list_sessions()) {
                    const auto state = deps.session_registry->current_model_state(session.id);
                    if (state && std::find(changed_names.begin(), changed_names.end(), state->name) !=
                                     changed_names.end()) {
                        deps.session_registry->reload_model_profile(session.id, false);
                    }
                }
            }
            const auto message = nlohmann::json{
                {"type", "model_profiles_updated"},
                {"payload", {{"names", changed_names}}}
            }.dump();
            std::lock_guard<std::mutex> lock(ws_mu);
            for (const auto& [connection, state] : ws_connections) {
                if (!state) continue;
                try { connection->send_text(message); } catch (...) {}
            }
        });
}

void WebServer::Impl::request_model_reasoning_sync(const std::string& name) {
    if (model_reasoning_sync && !shutdown_requested.load()) model_reasoning_sync->request(name);
}

} // namespace acecode::web
