#include <gtest/gtest.h>
#include "agent/approval/permission_audit_scope.hpp"
#include "agent/approval/session_exec_security.hpp"
#include "agent/boundary/workspace_boundary.hpp"
#include "permissions/permissions.hpp"
#include <atomic>
#include <memory>

TEST(PermissionAuditScope, SandboxIsFixedButModeAndDecisionAreReadAtRecordTime) {
    std::atomic<bool> busy{true};
    acecode::PermissionManager permissions;
    acecode::agent::WorkspaceBoundary boundary(testing::TempDir(), permissions);
    acecode::agent::SessionExecSecurity security(boundary, permissions, busy);
    auto entries = std::make_shared<std::vector<acecode::security::AuditEntry>>();
    security.set_audit_sink([entries](const auto& entry) { entries->push_back(entry); });
    std::optional<acecode::sandbox::ExecPermission> exec{std::in_place};
    exec->decision.sandbox = acecode::sandbox::SandboxMode::WorkspaceWrite;
    exec->decision.reason = "before";
    acecode::agent::PermissionAuditScope audit(
        security, permissions, nullptr, "bash", "command", "", {"a", "b"}, false, exec);
    permissions.set_mode(acecode::PermissionMode::Plan);
    exec->decision.sandbox = acecode::sandbox::SandboxMode::FullAccess;
    exec->decision.reason = "after";
    exec->input.escalation_requested = true;
    audit.record("allow", "user", "confirmation");
    ASSERT_EQ(entries->size(), 1U);
    EXPECT_EQ(entries->front().sandbox, acecode::sandbox::sandbox_mode_name(
        acecode::sandbox::SandboxMode::WorkspaceWrite));
    EXPECT_EQ(entries->front().detail["mode"], "plan");
    EXPECT_EQ(entries->front().detail["decision_reason"], "after");
    EXPECT_EQ(entries->front().detail["paths"], nlohmann::json({"a", "b"}));
    EXPECT_EQ(entries->front().detail["escalation_requested"], true);
}
