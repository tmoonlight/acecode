#pragma once

#include "config/config.hpp"
#include "session/global_session_catalog.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace acecode {

constexpr std::int64_t kHomeIntentRefreshMs = 30 * 60 * 1000;
// A transcript's last user message is never meaningfully newer than its metadata
// update; persisted data shows one-second rounding differences, so allow more.
constexpr std::int64_t kHomeIntentActivitySlackMs = 10 * 60 * 1000;

// Reads the last user-message time of a session whose metadata predates
// last_user_message_at ("" when it has none). nullopt stops the ranking (cancelled).
using HomeIntentActivityReader =
    std::function<std::optional<std::string>(const GlobalSessionCatalogEntry&)>;

// Without a reader such sessions count as inactive. With one, a session is read
// only while its metadata update time could still place it in the needed prefix.
std::vector<GlobalSessionCatalogEntry> select_home_intent_sessions(
    const GlobalSessionCatalog& catalog, const HomeIntentActivityReader& read = {});
std::optional<ModelProfile> resolve_home_intent_model(
    const AppConfig& config, const GlobalSessionCatalog& catalog,
    const HomeIntentActivityReader& read = {});

// Finding and reading sessions has its own budget, so a cold directory scan
// cannot spend the time reserved for the model call.
constexpr std::int64_t kHomeIntentPrepareBudgetMs = 30 * 1000;
constexpr std::int64_t kHomeIntentModelBudgetMs = 60 * 1000;

// Last-write times below are Unix ms. Metadata is replaced by rename, so no
// timestamp stored under a directory or in a file is newer; 0 means unknown,
// which is loaded before ranking can stop.
struct HomeIntentSessionFile {
    std::int64_t modified_ms = 0;
    std::function<std::optional<GlobalSessionCatalogEntry>()> read;
};

struct HomeIntentProjectContents {
    std::vector<GlobalSessionCatalogEntry> rows; // Already known, e.g. prewarmed index rows.
    std::vector<HomeIntentSessionFile> files;    // Read only while they can still matter.
};

struct HomeIntentProject {
    std::int64_t modified_ms = 0;
    std::function<HomeIntentProjectContents()> open; // Must not throw across threads.
};

struct HomeIntentCandidates {
    GlobalSessionCatalog catalog; // Rows that were loaded; the rest were ruled out or not reached.
    std::size_t loaded_projects = 0;
    std::size_t read_files = 0;
    bool settled = false;         // False when stopped before everything unloaded was ruled out.
};

// Opens projects and reads metadata files newest first, one parallel batch at a
// time, until nothing unloaded can hold one of the five sources or a newer
// fallback model.
HomeIntentCandidates load_home_intent_candidates(
    const AppConfig& config, std::vector<HomeIntentProject> projects,
    const HomeIntentActivityReader& read, const std::function<bool()>& stopped,
    std::size_t batch_size = 8);
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
