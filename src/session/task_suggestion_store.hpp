#pragma once

#include "provider/llm_provider.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace acecode {

struct TaskSuggestionClaimResult {
    std::optional<nlohmann::json> suggestion;
    bool claimed = false;
};

// Successful semantic summaries in the current fork epoch. Repair checkpoints
// and visible compact notices never increment or reset this count.
std::uint64_t count_successful_compactions(
    const std::vector<ChatMessage>& raw_messages);

class TaskSuggestionStore {
public:
    explicit TaskSuggestionStore(std::filesystem::path project_dir);

    static std::filesystem::path database_path_for_project(
        const std::filesystem::path& project_dir);

    std::vector<nlohmann::json> list(const std::string& source_session_id,
                                   std::string* error = nullptr) const;
    std::optional<nlohmann::json> get(const std::string& source_session_id,
                                     const std::string& id,
                                     std::string* error = nullptr) const;
    std::optional<nlohmann::json> propose(const std::string& source_session_id,
                                         nlohmann::json draft,
                                         std::string* error = nullptr) const;
    // The first durable claim fixes both target_session_id and location. A
    // repeated claim returns that same record, with claimed=false.
    TaskSuggestionClaimResult claim(const std::string& source_session_id,
                                    const std::string& id,
                                    const std::string& location,
                                    std::string* error = nullptr) const;
    // Runs the callback inside a cross-process SQLite write transaction. The
    // callback must only edit JSON, never perform external work or call back
    // into this store. false leaves the record unchanged.
    std::optional<nlohmann::json> update(
        const std::string& source_session_id,
        const std::string& id,
        const std::function<bool(nlohmann::json&)>& mutate,
        std::string* error = nullptr) const;
    std::optional<nlohmann::json> dismiss(const std::string& source_session_id,
                                         const std::string& id,
                                         std::string* error = nullptr) const;
    std::vector<nlohmann::json> recoverable(std::string* error = nullptr) const;
    bool erase_source(const std::string& source_session_id,
                       std::string* error = nullptr) const;

    // One context_handoff suggestion per source, including dismissed/started
    // records. threshold=0 disables creation. Existing records are unchanged.
    std::optional<nlohmann::json> propose_continuation(
        const std::string& source_session_id,
        const std::vector<ChatMessage>& raw_messages,
        std::uint64_t threshold,
        nlohmann::json launch_context = nlohmann::json::object(),
        std::string* error = nullptr) const;

private:
    std::filesystem::path db_path_;
};

} // namespace acecode
