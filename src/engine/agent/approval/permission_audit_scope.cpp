#include "permission_audit_scope.hpp"
#include "session_exec_security.hpp"
#include "permissions/permissions.hpp"

namespace acecode::agent {
PermissionAuditScope::PermissionAuditScope(
    SessionExecSecurity& security, PermissionManager& permissions,
    SessionManager* session, std::string tool, const std::string& command,
    const std::string& path, std::vector<std::string> targets, bool file_mutation,
    const std::optional<sandbox::ExecPermission>& exec)
    : security_(security), permissions_(permissions), session_(session),
      tool_(std::move(tool)), targets_(std::move(targets)), exec_(exec),
      category_(tool_ == "bash" ? security::kAuditCategoryCommand
          : file_mutation ? security::kAuditCategoryFile : security::kAuditCategoryTool),
      target_(tool_ == "bash" ? command : (!targets_.empty() ? targets_.front() : path)),
      sandbox_(exec ? std::string(sandbox::sandbox_mode_name(exec->decision.sandbox))
                    : std::string{}) {}

void PermissionAuditScope::record(
    const std::string& decision, const std::string& source, const std::string& reason) {
    nlohmann::json detail = nlohmann::json::object();
    detail["mode"] = PermissionManager::mode_name(permissions_.mode());
    if (targets_.size() > 1) detail["paths"] = targets_;
    if (exec_) {
        detail["command_kind"] = sandbox::command_kind_name(exec_->classification.kind);
        detail["decision_reason"] = exec_->decision.reason;
        if (exec_->input.escalation_requested) detail["escalation_requested"] = true;
        if (exec_->input.additional_requested) detail["additional_requested"] = true;
    }
    security_.record_audit(session_, category_, tool_, target_, decision, source,
        reason, sandbox_, std::move(detail));
}
} // namespace acecode::agent
