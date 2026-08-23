#pragma once

#include "session_client.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace acecode {

class SessionManager;
class SessionRegistry;

struct ThreadScope {
    std::string cwd;
    std::string caller_thread_id;
    SessionManager* caller_manager = nullptr;
};

struct ThreadWaitTarget {
    std::string thread_id;
    std::uint64_t after_cursor = 0;
};

struct ThreadServiceResult {
    bool success = false;
    nlohmann::json value = nlohmann::json::object();
    std::string error;
    // Runtime-only control used by delete_thread self-deletion. The tool layer
    // forwards these fields to ToolResult; they are never written as JSON.
    bool terminate_caller_after_turn = false;
    std::function<void()> post_turn_action;

    static ThreadServiceResult ok(
        nlohmann::json value = nlohmann::json::object());
    static ThreadServiceResult fail(std::string error);
};

// In-process thread domain service shared by model tools and host surfaces.
// It resolves persistent and active sessions in the calling workspace and
// never loops back through ACECode's HTTP API.
class ThreadService {
public:
    struct Deps {
        SessionRegistry* registry = nullptr;
        SessionClient* client = nullptr;
    };

    explicit ThreadService(Deps deps);

    ThreadServiceResult list(const ThreadScope& scope,
                             std::size_t limit = 20) const;
    ThreadServiceResult read(const ThreadScope& scope,
                             const std::string& thread_id,
                             const std::string& cursor = {},
                             std::size_t turn_limit = 8,
                             bool include_outputs = false,
                             std::size_t max_chars_per_item = 2000) const;
    ThreadServiceResult wait(const ThreadScope& scope,
                             const std::vector<ThreadWaitTarget>& targets,
                             int timeout_ms,
                             const std::atomic<bool>* abort_flag = nullptr) const;

    ThreadServiceResult create(const ThreadScope& scope,
                               const std::string& prompt,
                               const std::string& title = {},
                               const std::string& model_name = {}) const;
    ThreadServiceResult fork(const ThreadScope& scope,
                             const std::string& thread_id = {}) const;
    ThreadServiceResult send(const ThreadScope& scope,
                             const std::string& thread_id,
                             const std::string& prompt) const;
    ThreadServiceResult set_title(const ThreadScope& scope,
                                  const std::string& thread_id,
                                  const std::string& title) const;
    ThreadServiceResult set_pinned(const ThreadScope& scope,
                                   const std::string& thread_id,
                                   bool pinned) const;
    ThreadServiceResult set_archived(const ThreadScope& scope,
                                     const std::string& thread_id,
                                     bool archived) const;
    ThreadServiceResult delete_thread(const ThreadScope& scope,
                                      const std::string& thread_id) const;
    ThreadServiceResult repair(const ThreadScope& scope,
                               const std::string& thread_id) const;

private:
    Deps deps_;
};

} // namespace acecode
