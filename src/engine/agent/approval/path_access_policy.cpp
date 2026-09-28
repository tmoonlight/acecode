#include "agent/agent_loop.hpp"
#include "permissions/shell_write_guard.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "utils/logger.hpp"
#include "workspace/workspace_registry.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <sstream>
#include <utility>

namespace acecode {

bool AgentLoop::is_cwd_validation_exempt(const std::string& tool_name, const std::string& path, const std::string& boundary_root) {
    const bool bounded = !boundary_root.empty();
    if (tool_name == "file_read" || tool_name == "create_workspace" ||
        (bounded &&
         permissions_.mode() == PermissionMode::Yolo &&
         tools_.is_read_only(tool_name))) return true;
    if (permissions_.mode() == PermissionMode::Yolo &&
        !permissions_.is_dangerous() && !bounded) {
        return true;
    }
    if (!session_manager_) return false;
    return session_manager_->is_plan_file_path(path);
}

std::string AgentLoop::path_validation_error(const std::string& tool_name, const std::string& path) {
    if (path.empty() || tool_name == "bash") return {};
    const std::string boundary_root = write_root();
    if (!boundary_root.empty() &&
        permissions_.mode() == PermissionMode::Yolo &&
        !permissions_.is_dangerous() &&
        !tools_.is_read_only(tool_name) &&
        tool_name != "create_workspace") {
        const std::string boundary_error =
            PathValidator(boundary_root, false).validate(path);
        if (!boundary_error.empty() && !path_in_workspace_folders(path)) {
            return "Write boundary blocked: " + path +
                   " is outside the session write root " + boundary_root +
                   ". Reads may go anywhere, but every write must stay inside "
                   "the worktree / execution root.";
        }
    }
    if (is_cwd_validation_exempt(tool_name, path, boundary_root)) return {};
    std::string cwd_error = path_validator_.validate(path);
    // 「编辑项目」的附加文件夹与工作目录同等对待。
    if (!cwd_error.empty() && path_in_workspace_folders(path)) return {};
    return cwd_error;
}

} // namespace acecode
