#include "agent/agent_loop.hpp"
#include "agent/approval/permission_payloads.hpp"
#include "agent/request/request_context.hpp"
#include "agent/transcript/transcript_queries.hpp"
#include "permissions/interaction_mode.hpp"
#include "permissions/shell_write_guard.hpp"
#include "prompt/system_prompt.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "session/thread_repair.hpp"
#include "skills/skill_registry.hpp"
#include "utils/logger.hpp"
#include "utils/stream_processing.hpp"
#include "workspace/workspace_registry.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <sstream>
#include <utility>

namespace acecode {

void AgentLoop::refresh_workspace_folders() {
    // workspace.json 与会话文件同在 <projects>/<hash>/ 下。worktree 会话的 cwd_
    // 是 worktree 路径,但会话存储目录不动,所以优先用 SessionManager 的 project dir。
    std::string project_dir =
        session_manager_ ? session_manager_->current_project_dir() : std::string{};
    if (project_dir.empty()) project_dir = SessionStorage::get_project_dir(cwd_);
    auto folders = desktop::load_workspace_folders(project_dir);
    {
        std::lock_guard<std::mutex> lk(workspace_folders_mu_);
        workspace_main_folder_ = std::move(folders.main_folder);
        workspace_extra_folders_ = std::move(folders.extra_folders);
    }
    sandbox_runtime_.set_workspace_writable_roots(writable_workspace_folders());
}

std::vector<std::string> AgentLoop::workspace_extra_folders() const {
    std::lock_guard<std::mutex> lk(workspace_folders_mu_);
    return workspace_extra_folders_;
}

std::vector<std::string> AgentLoop::writable_workspace_folders() const {
    std::string main_folder;
    std::vector<std::string> extras;
    {
        std::lock_guard<std::mutex> lk(workspace_folders_mu_);
        main_folder = workspace_main_folder_;
        extras = workspace_extra_folders_;
    }
    if (extras.empty() || main_folder.empty() || write_root().empty()) return extras;
    // 写边界存在的意义是护住主 checkout。与主文件夹互为包含的附加文件夹
    // (主仓的上级目录,或主仓里的子目录)一旦放行,worktree 隔离就被绕开了。
    const auto contains = [](const std::string& root, const std::string& path) {
        return PathValidator(root, false).validate(path).empty();
    };
    std::vector<std::string> out;
    for (const auto& folder : extras) {
        if (contains(folder, main_folder) || contains(main_folder, folder)) continue;
        out.push_back(folder);
    }
    return out;
}

bool AgentLoop::path_in_workspace_folders(const std::string& path) const {
    if (path.empty()) return false;
    const auto folders = writable_workspace_folders();
    if (folders.empty()) return false;
    std::filesystem::path target = path_from_utf8(path);
    // 相对路径永远按会话 cwd 解析,不能拿附加文件夹当基准去"凑"出一个放行。
    if (target.is_relative()) target = path_from_utf8(cwd_) / target;
    const std::string absolute = path_to_utf8(target);
    for (const auto& folder : folders) {
        if (PathValidator(folder, false).validate(absolute).empty()) return true;
    }
    return false;
}

SystemPromptWorkspaceFolders AgentLoop::system_prompt_workspace_folders() const {
    SystemPromptWorkspaceFolders result;
    result.additional = writable_workspace_folders();
    for (const auto& folder : workspace_extra_folders()) {
        if (std::find(result.additional.begin(), result.additional.end(), folder) ==
            result.additional.end()) {
            result.read_only.push_back(folder);
        }
    }
    return result;
}

std::string AgentLoop::write_root() const {
    // worktree 优先:进了 worktree(自己进的,或 spawn_subagent 从父会话继承
    // 的)边界就是 worktree;其次 LOOP 执行策略(边界 = cwd);最后父会话
    // 透传的 write_root。三者都空 = 无边界,Yolo 维持旧的全放行语义。
    if (session_manager_) {
        const WorktreeSessionInfo worktree = session_manager_->active_worktree();
        if (worktree.active()) return worktree.worktree_path;
    }
    if (loop_execution_policy_.active) return cwd_;
    return inherited_write_root_;
}

} // namespace acecode
