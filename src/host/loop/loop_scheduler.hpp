#pragma once

#include "loop_store.hpp"

#include "session/session_client.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include "utils/joining_thread.hpp"
#include <unordered_map>
#include <vector>

namespace acecode {
class SessionRegistry;
struct AppConfig;
}

namespace acecode::loop {

std::string build_loop_system_context(const LoopDefinition& loop,
                                      const LoopRun& run,
                                      const std::string& execution_root,
                                      const std::string& worktree_branch);
bool should_create_loop_worktree(const LoopDefinition& loop, bool git_workspace);

// 写边界的事后检测:与 launch 时记下的主 checkout `git status` 基线比对,
// 返回运行期间新出现的改动路径。工具层守卫只认参数里的路径和命令里看得见
// 的写目标,`cd 主仓 && node fix.js` 这类动态写入只能靠它事后发现。git 不
// 可用 / 非仓库返回空(当作不可知,不当作没改)。
std::vector<std::string> detect_workspace_touched(const std::string& workspace_cwd,
                                                  const std::vector<std::string>& baseline);

struct LoopEventState {
    bool waiting_user = false;
    std::string error;
};

// Applies one session event to a LOOP run. The returned status is persisted by
// LoopScheduler; nullopt means the event does not change the durable run state.
std::optional<RunStatus> apply_loop_session_event(const SessionEvent& event,
                                                  LoopEventState& state,
                                                  std::string& reason);

class LoopScheduler {
public:
    using Clock = std::function<std::int64_t()>;

    LoopScheduler(LoopStore& store,
                  SessionRegistry& registry,
                  SessionClient& client,
                  const AppConfig& config,
                  Clock clock = {});
    ~LoopScheduler();

    LoopScheduler(const LoopScheduler&) = delete;
    LoopScheduler& operator=(const LoopScheduler&) = delete;

    bool start(StoreError* error = nullptr);
    void stop();
    void notify_changed();
    bool running() const { return running_.load(); }

private:
    struct CallbackState;

    void run();
    void launch(const LoopDefinition& loop, const LoopRun& run);
    bool model_exists(const std::string& name) const;
    void fail_run(const LoopDefinition& loop,
                  const LoopRun& run,
                  const std::string& reason,
                  bool disable_loop = false,
                  const std::string& session_id = {});

    LoopStore& store_;
    SessionRegistry& registry_;
    SessionClient& client_;
    const AppConfig& config_;
    Clock clock_;
    std::string owner_id_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::mutex wait_mu_;
    std::condition_variable wait_cv_;
    acecode::JoiningThread worker_;
    std::shared_ptr<CallbackState> callbacks_;
};

} // namespace acecode::loop
