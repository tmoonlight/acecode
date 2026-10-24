#pragma once

#include "config/config.hpp"
#include "session/global_session_catalog.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace acecode {

constexpr std::int64_t kHomeIntentRefreshMs = 30 * 60 * 1000;

std::vector<GlobalSessionCatalogEntry> select_home_intent_sessions(
    const GlobalSessionCatalog& catalog);
std::optional<ModelProfile> resolve_home_intent_model(
    const AppConfig& config, const GlobalSessionCatalog& catalog);
nlohmann::json home_intent_source(const GlobalSessionCatalogEntry& entry);
std::vector<ChatMessage> home_intent_messages(const nlohmann::json& sources,
                                            const std::string& locale);
// Returns nullopt for malformed output. Workspace fields in model output are ignored.
std::optional<nlohmann::json> parse_home_intent_prompts(
    const std::string& content, const nlohmann::json& sources);

// Owned by the web host. HTTP workers share only the cancellation flag with
// detached provider calls; those calls never access this cache or host services.
class HomeIntentPromptCache {
public:
    ~HomeIntentPromptCache() { shutdown(); }
    nlohmann::json snapshot() const;
    std::shared_ptr<std::atomic<bool>> begin(bool force, std::int64_t now_ms);
    void finish(const std::shared_ptr<std::atomic<bool>>& request,
                const nlohmann::json& suggestions, const std::string& model,
                const std::string& error, std::int64_t now_ms);
    void shutdown();
private:
    mutable std::mutex mutex_;
    nlohmann::json suggestions_ = nlohmann::json::array();
    std::string model_;
    std::string error_;
    std::int64_t generated_at_ms_ = 0;
    std::int64_t next_refresh_at_ms_ = 0;
    bool stopped_ = false;
    // Shared by the host, synchronous HTTP waiter, timeout and provider worker.
    std::shared_ptr<std::atomic<bool>> request_;
};

} // namespace acecode
