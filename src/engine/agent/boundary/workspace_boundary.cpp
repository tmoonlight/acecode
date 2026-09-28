#include "workspace_boundary.hpp"

#include "permissions/permissions.hpp"
#include "prompt/system_prompt.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "workspace/workspace_registry.hpp"

#include <algorithm>
#include <utility>

namespace acecode::agent {

WorkspaceBoundary::WorkspaceBoundary(std::string cwd, PermissionManager& permissions)
    : permissions_(permissions), cwd_(std::move(cwd)), validator_(cwd_, permissions.is_dangerous()) {}

std::string WorkspaceBoundary::cwd() const {
    std::lock_guard<std::mutex> lock(state_mu_);
    return cwd_;
}

void WorkspaceBoundary::set_cwd(const std::string& cwd) {
    PathValidator validator(cwd, permissions_.is_dangerous());
    std::lock_guard<std::mutex> lock(state_mu_);
    cwd_ = cwd;
    validator_ = std::move(validator);
}

void WorkspaceBoundary::set_loop_active(bool active) {
    std::lock_guard<std::mutex> lock(state_mu_);
    loop_active_ = active;
}

void WorkspaceBoundary::set_inherited_write_root(std::string root) {
    std::lock_guard<std::mutex> lock(state_mu_);
    inherited_write_root_ = std::move(root);
}

PathValidator WorkspaceBoundary::validator_snapshot() const {
    std::lock_guard<std::mutex> lock(state_mu_);
    return validator_;
}

std::string WorkspaceBoundary::validate(const std::string& path) const {
    return validator_snapshot().validate(path);
}

bool WorkspaceBoundary::is_dangerous_path(const std::string& path) const {
    return validator_snapshot().is_dangerous_path(path);
}

void WorkspaceBoundary::refresh_workspace_folders(SessionManager* session) {
    // workspace.json 与会话文件同在 <projects>/<hash>/ 下。worktree 会话的 cwd()
    // 是 worktree 路径,但会话存储目录不动,所以优先用 SessionManager 的 project dir。
    std::string project_dir =
        session ? session->current_project_dir() : std::string{};
    if (project_dir.empty()) project_dir = SessionStorage::get_project_dir(cwd());
    auto folders = desktop::load_workspace_folders(project_dir);
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        main_folder_ = std::move(folders.main_folder);
        extra_folders_ = std::move(folders.extra_folders);
    }
}

std::vector<std::string> WorkspaceBoundary::workspace_extra_folders() const {
    std::lock_guard<std::mutex> lk(state_mu_);
    return extra_folders_;
}

std::vector<std::string> WorkspaceBoundary::writable_workspace_folders(SessionManager* session) const {
    std::string main_folder;
    std::vector<std::string> extras;
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        main_folder = main_folder_;
        extras = extra_folders_;
    }
    if (extras.empty() || main_folder.empty() || write_root(session).empty()) return extras;
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

bool WorkspaceBoundary::path_in_workspace_folders(const std::string& path, SessionManager* session) const {
    if (path.empty()) return false;
    const auto folders = writable_workspace_folders(session);
    if (folders.empty()) return false;
    std::filesystem::path target = path_from_utf8(path);
    // 相对路径永远按会话 cwd 解析,不能拿附加文件夹当基准去"凑"出一个放行。
    if (target.is_relative()) target = path_from_utf8(cwd()) / target;
    const std::string absolute = path_to_utf8(target);
    for (const auto& folder : folders) {
        if (PathValidator(folder, false).validate(absolute).empty()) return true;
    }
    return false;
}

SystemPromptWorkspaceFolders WorkspaceBoundary::system_prompt_workspace_folders(SessionManager* session) const {
    SystemPromptWorkspaceFolders result;
    result.additional = writable_workspace_folders(session);
    for (const auto& folder : workspace_extra_folders()) {
        if (std::find(result.additional.begin(), result.additional.end(), folder) ==
            result.additional.end()) {
            result.read_only.push_back(folder);
        }
    }
    return result;
}

std::string WorkspaceBoundary::write_root(SessionManager* session) const {
    // worktree 优先:进了 worktree(自己进的,或 spawn_subagent 从父会话继承
    // 的)边界就是 worktree;其次 LOOP 执行策略(边界 = cwd);最后父会话
    // 透传的 write_root。三者都空 = 无边界,Yolo 维持旧的全放行语义。
    if (session) {
        const WorktreeSessionInfo worktree = session->active_worktree();
        if (worktree.active()) return worktree.worktree_path;
    }
    std::lock_guard<std::mutex> lock(state_mu_);
    if (loop_active_) return cwd_;
    return inherited_write_root_;
}

} // namespace acecode::agent
