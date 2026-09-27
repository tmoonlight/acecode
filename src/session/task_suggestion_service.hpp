#pragma once

#include "provider/llm_provider.hpp"

#include <memory>
#include <functional>
#include <shared_mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace acecode {

class SessionClient;
class SessionRegistry;
struct AppConfig;
struct SessionEntry;

struct TaskSuggestionServiceResult {
    bool ok = false;
    int http_status = 500;
    nlohmann::json value = nlohmann::json::object();
    std::string error;
};

// Suggestions are offers, not agent jobs. Only accept() can provision and
// dispatch a target. All worker callbacks own shared state, never this object.
class TaskSuggestionService {
public:
    struct Deps {
        SessionRegistry* registry = nullptr;
        SessionClient* client = nullptr;
        const AppConfig* config = nullptr;
        std::shared_mutex* config_mutex = nullptr;
        // Optional embedding/test seam. Called with the target already acquired;
        // must not acquire SessionRegistry or call back into the source loop.
        std::function<bool(const std::shared_ptr<SessionEntry>&, const UserInput&)>
            enqueue_input;
    };

    explicit TaskSuggestionService(Deps deps);
    ~TaskSuggestionService();
    TaskSuggestionService(const TaskSuggestionService&) = delete;
    TaskSuggestionService& operator=(const TaskSuggestionService&) = delete;

    // The host calls this before destroying SessionRegistry. Tool closures may
    // keep the service alive longer than the registry they were registered for.
    void shutdown();

    TaskSuggestionServiceResult propose(const std::string& source_session_id,
                                        nlohmann::json draft);
    TaskSuggestionServiceResult list(const std::string& source_session_id) const;
    TaskSuggestionServiceResult accept(const std::string& source_session_id,
                                       const std::string& suggestion_id,
                                       const std::string& location);
    TaskSuggestionServiceResult dismiss(const std::string& source_session_id,
                                        const std::string& suggestion_id);

    // Called by the host after restoring a source. Reads never implicitly
    // accept work. Recovery only schedules records already accepted by a user.
    TaskSuggestionServiceResult recover(const std::string& source_session_id);

private:
    struct State;
    std::shared_ptr<State> state_;
};

// The same bounded handoff builder is used for fresh starts and retry receipts.
// raw_messages are the durable visible history, not a clone for the new model.
nlohmann::json build_task_handoff_snapshot(
    const nlohmann::json& source_context,
    const std::vector<ChatMessage>& raw_messages,
    const nlohmann::json& goal,
    const nlohmann::json& todos);

UserInput build_task_suggestion_input(const nlohmann::json& suggestion);

} // namespace acecode
