#pragma once
#include <string>

namespace acecode { class ToolExecutor; class PermissionManager; class SessionManager; }
namespace acecode::agent {
class ToolSessionHost;
class WorkspaceBoundary;
class PathAccessPolicy {
public:
    PathAccessPolicy(ToolExecutor& tools, PermissionManager& permissions,
        WorkspaceBoundary& boundary, ToolSessionHost& host, SessionManager* session)
        : tools_(tools), permissions_(permissions), boundary_(boundary),
          host_(host), session_manager_(session) {}
    std::string path_validation_error(const std::string& tool, const std::string& path);
private:
    bool is_cwd_validation_exempt(const std::string& tool, const std::string& path,
        const std::string& write_root);
    ToolExecutor& tools_;
    PermissionManager& permissions_;
    WorkspaceBoundary& boundary_;
    ToolSessionHost& host_;
    SessionManager* session_manager_; // Nullable borrowed constructor dependency.
};
} // namespace acecode::agent
