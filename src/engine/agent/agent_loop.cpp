#include "agent/agent_loop.hpp"
#include "agent/model_step/active_provider_slot.hpp"
#include "agent/detail/agent_payloads.hpp"
#include "agent/guards/doom_guard.hpp"
#include "computer_use/runtime.hpp"
#include "hooks/hook_manager.hpp"
#include "pa/pa_context_budget.hpp"
#include "provider/text_tool_call_recovery.hpp"
#include "session/permission_prompter.hpp"
#include "session/session_manager.hpp"
#include "session/token_tracker.hpp"
#include "utils/encoding.hpp"
#include "utils/logger.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <sstream>
#include <utility>
#include <thread>

namespace acecode {

using agent::detail::kDefaultNoModelConfiguredPrompt;

AgentLoop::AgentLoop(ProviderAccessor provider_accessor, ToolExecutor& tools,
                     AgentCallbacks callbacks, const std::string& cwd,
                     PermissionManager& permissions)
    : provider_accessor_(std::move(provider_accessor))
    , tools_(tools)
    , callbacks_(std::move(callbacks))
    , active_provider_slot_(std::make_unique<agent::ActiveProviderSlot>())
    , cwd_(cwd)
    , permissions_(permissions)
    , path_validator_(cwd, permissions.is_dangerous())
    , no_model_config_prompt_(kDefaultNoModelConfiguredPrompt)
{
    reload_exec_rules();
    worker_thread_ = JoiningThread(&AgentLoop::worker_main, this);
}

AgentLoop::~AgentLoop() {
    shutdown();
}

void AgentLoop::set_cwd(const std::string& new_cwd) {
    cwd_ = new_cwd;
    path_validator_ = PathValidator(new_cwd, permissions_.is_dangerous());
    // cwd 变了(EnterWorktree/ExitWorktree),旧 gitStatus 快照作废,
    // 下一次模型请求按新 cwd 重采(openspec add-git-context)。
    git_snapshot_cache_.reset();
    permissions_.clear_session_allows();
    sandbox_runtime_.clear_session_grants();
    last_sandbox_violation_.reset();
    reload_exec_rules();
    // 进出 worktree 会改变写边界,可写附加文件夹随之重算。
    sandbox_runtime_.set_workspace_writable_roots(writable_workspace_folders());
}

ResolvedQuestionPolicy AgentLoop::resolved_question_policy() const {
    return resolve_question_policy(loop_cfg_);
}

void AgentLoop::abort() {
    abort_signal_.request();
    if (session_manager_) release_computer_use_session(session_manager_->current_session_id());
    wake_active_provider_retry();
}

// computer-use 会话租约的唯一释放出口(abort / 回合收尾 / DesktopTurnLease 析构都走
// 这里)。默认原样调用 computer_use::release_session;P0-11 的表征测试注入 fake
// 记录 owner 与释放次数,不启动真实桌面 helper。
void AgentLoop::release_computer_use_session(const std::string& session_id) const {
    if (computer_use_release_) {
        computer_use_release_(session_id);
        return;
    }
    computer_use::release_session(session_id);
}

void AgentLoop::wake_active_provider_retry() {
    active_provider_slot_->wake();
}

void AgentLoop::clear_stale_abort_request() {
    if (!busy_.load()) {
        abort_signal_.clear();
    }
}

void AgentLoop::shutdown() {
    side_question_shutdown_.store(true);
    {
        std::lock_guard<std::mutex> lk(queue_mu_);
        shutdown_requested_ = true;
    }
    abort_signal_.request();
    wake_active_provider_retry();
    queue_cv_.notify_one();
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
    join_side_question_threads();
}

void AgentLoop::set_permission_prompter(std::unique_ptr<PermissionPrompter> p) {
    prompter_ = std::move(p);
}

void AgentLoop::set_callbacks(AgentCallbacks cb) {
    callbacks_ = std::move(cb);
}

} // namespace acecode
