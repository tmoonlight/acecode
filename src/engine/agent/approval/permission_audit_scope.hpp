#pragma once
#include "sandbox/exec_permission.hpp"
#include <optional>
#include <string>
#include <vector>

namespace acecode { class SessionManager; class PermissionManager; }
namespace acecode::agent {
class SessionExecSecurity;

// One synchronous decision scope: sandbox/category/target are early-bound,
// mode and command decision details are read at each record() call.
class PermissionAuditScope {
public:
    PermissionAuditScope(SessionExecSecurity& security, PermissionManager& permissions,
        SessionManager* session, std::string tool, const std::string& command,
        const std::string& path, std::vector<std::string> targets, bool file_mutation,
        const std::optional<sandbox::ExecPermission>& exec);
    PermissionAuditScope(const PermissionAuditScope&) = delete;
    PermissionAuditScope& operator=(const PermissionAuditScope&) = delete;
    const std::string& sandbox() const { return sandbox_; }
    void record(const std::string& decision, const std::string& source,
        const std::string& reason);
private:
    SessionExecSecurity& security_;
    PermissionManager& permissions_;
    SessionManager* session_; // Nullable borrowed constructor dependency.
    std::string tool_;
    std::vector<std::string> targets_;
    const std::optional<sandbox::ExecPermission>& exec_;
    std::string category_;
    std::string target_;
    std::string sandbox_;
};
} // namespace acecode::agent
