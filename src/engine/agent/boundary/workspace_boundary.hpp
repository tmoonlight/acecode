#pragma once

#include "permissions/path_validator.hpp"

#include <mutex>
#include <string>
#include <vector>

namespace acecode {
class PermissionManager;
class SessionManager;
struct SystemPromptWorkspaceFolders;
}

namespace acecode::agent {

// Snapshots cross tool threads by value. The leaf state lock is released before
// asking SessionManager or loading workspace files; no collaborator is called
// while it is held.
class WorkspaceBoundary {
public:
    WorkspaceBoundary(std::string cwd, PermissionManager& permissions);
    std::string cwd() const;
    void set_cwd(const std::string& cwd);
    void set_loop_active(bool active);
    void set_inherited_write_root(std::string root);
    void refresh_workspace_folders(SessionManager* session);
    std::vector<std::string> workspace_extra_folders() const;
    std::vector<std::string> writable_workspace_folders(SessionManager* session) const;
    bool path_in_workspace_folders(const std::string& path, SessionManager* session) const;
    SystemPromptWorkspaceFolders system_prompt_workspace_folders(SessionManager* session) const;
    std::string write_root(SessionManager* session) const;
    std::string validate(const std::string& path) const;
    bool is_dangerous_path(const std::string& path) const;
private:
    PathValidator validator_snapshot() const;
    PermissionManager& permissions_;
    mutable std::mutex state_mu_;
    std::string cwd_;
    PathValidator validator_;
    bool loop_active_ = false;
    std::string inherited_write_root_;
    std::string main_folder_;
    std::vector<std::string> extra_folders_;
};

} // namespace acecode::agent
