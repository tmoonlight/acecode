#include <gtest/gtest.h>
#include "permissions/default_rules.hpp"
#include "permissions/permissions.hpp"

TEST(TuiDefaultRules, MainSessionRulesDoNotChangeOtherHosts) {
    // TUI 的七条规则不能泄漏给 daemon 或子会话;共享规则保护仍为 1000。
    acecode::PermissionManager tui, other;
    acecode::configure_tui_default_permissions(tui, false, "unknown");
    EXPECT_EQ(tui.mode(), acecode::PermissionMode::Default);
    for (const auto* tool : {"file_write", "file_edit", "apply_patch"}) {
        for (const auto* path : {".env", ".git/config"}) {
            const auto rule = tui.matched_rule_detail(tool, path);
            ASSERT_TRUE(rule.has_value());
            EXPECT_EQ(rule->action, acecode::RuleAction::Deny);
            EXPECT_EQ(rule->priority, 100);
            EXPECT_FALSE(other.matched_rule_detail(tool, path).has_value());
        }
        const auto protected_rule = tui.matched_rule_detail(tool, ".acecode/rules/local.rules");
        ASSERT_TRUE(protected_rule.has_value());
        EXPECT_EQ(protected_rule->priority, 1000);
    }
    const auto shell = tui.matched_rule_detail("bash", "", "rm -rf /");
    ASSERT_TRUE(shell.has_value());
    EXPECT_EQ(shell->priority, 100);
}
TEST(TuiDefaultRules, DangerousFlagOverridesConfiguredMode) {
    // CLI 危险模式优先,但规则本身仍在,供恢复普通模式后继续使用。
    acecode::PermissionManager permissions;
    acecode::configure_tui_default_permissions(permissions, true, "plan");
    EXPECT_EQ(permissions.mode(), acecode::PermissionMode::Yolo);
    EXPECT_TRUE(permissions.is_dangerous());
    EXPECT_TRUE(permissions.matched_rule_detail("file_write", ".env").has_value());
}
