#pragma once

// MeshAgentService: the mesh swarm (Codex Multi-Agent V2) tree controller.
//
// - Tree directory per root session: canonical path -> session id, persisted
//   in the root's data dir (session/mesh_tree_index.hpp). Every mesh child has
//   parent_session_id = root id; the real parent is the path's parent.
// - Residency (Codex V2Residency): at most max_concurrent_agents - 1 loaded
//   children per tree (the root takes the remaining slot). A full tree evicts
//   the least recently active unloadable child (finished, no active work, empty
//   mailbox); addressing an evicted child restores it from its transcript.
// - Delivery goes through AgentLoop::deliver_inter_agent_message; mail for an
//   unloaded agent waits here and is flushed when the agent is restored.
// - Completion routing: a child's finished turn sends FINAL_ANSWER (completed or
//   errored; interrupted turns send nothing) to its parent without waking it.
//
// Lifetime: owned by a composition root as shared_ptr, constructed after the
// SessionRegistry and shut down before it. Tools and event listeners capture
// weak_ptr only. The service mutex is never held across registry create /
// resume / destroy, which may join agent workers.

#include "session/agent_fork_history.hpp"
#include "session/agent_path.hpp"
#include "session/inter_agent_message.hpp"
#include "session/scoped_subscription.hpp"
#include "session_host/local_session_client.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

namespace acecode {

struct ToolContext;
class AgentLoop;
class SessionManager;
class ExpertRegistry;
struct AppConfig;
struct MeshSwarmConfig;

namespace mesh {

// Codex AgentStatus, serialized like Codex: "pending_init" | "running" |
// "interrupted" | "shutdown" | "not_found" | {"completed": text|null} |
// {"errored": text}.
struct AgentStatus {
    enum class Kind { PendingInit, Running, Interrupted, Completed, Errored, Shutdown, NotFound };
    Kind kind = Kind::NotFound;
    std::optional<std::string> completed;
    std::string errored;
};
nlohmann::json agent_status_to_json(const AgentStatus& status);
// Short UI label: pending_init | running | completed | errored | interrupted | not_found.
std::string agent_status_label(const AgentStatus& status);

struct SpawnArgs {
    std::string task_name;
    std::string message;
    std::string agent_type;
    std::string model;
    std::optional<std::string> reasoning_effort;
    ForkTurns fork;
};

struct SpawnResult {
    std::string error;       // non-empty = failure, model-facing text
    std::string session_id;
    std::string path;
};

struct ListedAgent {
    std::string path;
    std::string session_id;
    AgentStatus status;
};

enum class WaitResult { Mailbox, Steered, TimedOut, Aborted };

// Snapshot for UI surfaces (background-task panel, /tasks).
struct TreeAgentInfo {
    std::string session_id;
    std::string path;
    bool loaded = false;
    AgentStatus status;
    std::size_t held_mail = 0;
};

class MeshAgentService : public std::enable_shared_from_this<MeshAgentService> {
public:
    struct Deps {
        SessionRegistry* registry = nullptr;        // Borrowed; outlives the service.
        const ExpertRegistry* experts = nullptr;    // Nullable borrowed.
        const AppConfig* config = nullptr;          // Nullable borrowed; outlives the service.
        std::shared_mutex* config_mutex = nullptr;  // Nullable; guards *config writers.
        // TUI: the root session lives outside the registry. Both return the
        // current main session; the loop pointer is borrowed for one call.
        std::function<std::string()> external_root_id;
        std::function<AgentLoop*()> external_root_loop;
    };

    explicit MeshAgentService(Deps deps);
    ~MeshAgentService();
    MeshAgentService(const MeshAgentService&) = delete;
    MeshAgentService& operator=(const MeshAgentService&) = delete;

    // Installs the registry swarm-mode guard; call once after make_shared.
    void attach();
    // After a mesh child is created or restored (UI event tracking). Install
    // before sessions run; the callback is invoked on tool threads.
    using AgentLoadedCallback =
        std::function<void(const std::string& child_id, const std::string& root_id)>;
    void set_on_agent_loaded(AgentLoadedCallback callback);
    MeshSwarmConfig config_snapshot() const;
    std::vector<std::string> saved_model_names() const;
    // Drops subscriptions and the guard. Idempotent; call before the registry
    // shuts down.
    void shutdown();

    SpawnResult spawn(const ToolContext& ctx, const SpawnArgs& args);
    // send_message (trigger_turn=false) / followup_task (true). Empty = ok.
    std::string deliver(const ToolContext& ctx, const std::string& target,
                        const std::string& message, bool trigger_turn);
    std::string list(const ToolContext& ctx, const std::string& path_prefix,
                     std::vector<ListedAgent>& out);
    std::string interrupt(const ToolContext& ctx, const std::string& target,
                          AgentStatus& previous);
    std::string wait(const ToolContext& ctx, std::chrono::milliseconds timeout,
                     WaitResult& result);

    // Mesh exit guard: non-empty when the root's tree still runs agents or
    // holds undelivered follow-up tasks.
    std::string tree_busy_reason(const std::string& root_id);
    std::vector<TreeAgentInfo> tree_agents(const std::string& root_id);

    struct Caller;
    struct Tree;
    struct Record;

private:
    std::optional<Caller> caller(const ToolContext& ctx, std::string* error) const;
    Tree& tree_locked(const Caller& caller);
    std::string resolve_locked(Tree& tree, const Caller& caller, const std::string& target,
                               Record** out) const;
    AgentLoop* loop_for(const std::string& session_id,
                        std::shared_ptr<SessionEntry>& keep_alive) const;
    AgentStatus status_locked(const Tree& tree, const Record& record) const;
    std::string reserve_slot(const std::string& root_id, const std::string& protected_id);
    void release_slot(const std::string& root_id);
    void commit_resident(const std::string& root_id, const std::string& session_id);
    void touch_locked(Tree& tree, const std::string& session_id);
    std::string ensure_loaded(const Caller& caller, const std::string& path);
    void subscribe_child(const std::string& root_id, const std::string& session_id);
    void persist_index_locked(const Tree& tree) const;
    void on_child_event(const std::string& root_id, const std::string& session_id,
                        const SessionEvent& event);
    void route_completion(const std::string& root_id, const std::string& session_id,
                          const std::string& outcome);
    bool deliver_envelope_locked(Tree& tree, Record& recipient, UserInput envelope,
                                 bool trigger_turn);
    UserInput make_envelope(InterAgentMessageType type, const std::string& recipient,
                            const std::string& sender, const std::string& sender_session,
                            const std::string& payload, const std::string& status = {}) const;

    Deps deps_;
    LocalSessionClient client_;
    void notify_agent_loaded(const std::string& child_id, const std::string& root_id);

    mutable std::mutex mu_;
    std::condition_variable cv_; // Signals restore / eviction completion.
    mutable std::mutex callback_mu_;
    AgentLoadedCallback on_agent_loaded_;
    std::map<std::string, std::unique_ptr<Tree>> trees_;
    std::atomic<bool> stopped_{false};
    std::uint64_t activity_clock_ = 0; // Guarded by mu_.
};

} // namespace mesh
} // namespace acecode
