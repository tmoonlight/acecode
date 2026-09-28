#include "test_support/agent_loop/characterization_fixture.hpp"

#include "sandbox/exec_rules.hpp"
#include "session/thread_goal_store.hpp"
#include "tool/bash_tool.hpp"

#include <algorithm>
#include <tuple>

namespace {
using namespace acecode;
using namespace acecode_test::characterization;

struct PermissionCase {
    const char* name;
    PermissionMode mode = PermissionMode::Default;
    const char* tool = "file_write";
    const char* rule = "none";
    bool goal = false;
    bool headless = false;
    const char* hook = "none";
    std::optional<PermissionResult> answer = PermissionResult::Allow;
    const char* verdict = "interactive_allow";
    const char* command = "pnpm test";
    bool escalation = false;
};

struct GoldenDecision {
    bool executed;
    std::string output;
    std::string decision;
    std::string source;
    std::string reason;
    std::string resolved;
    std::string resolved_source;
    bool confirmation = false;
    bool session_allow = false;
};

GoldenDecision golden(const std::string& name) {
    if (name == "read") return {true, "probe ok", "", "", "", "", ""};
    if (name == "auto_file") return {true, "probe ok", "allow", "auto", "mode_auto", "", ""};
    if (name == "yolo_file") return {true, "probe ok", "allow", "auto", "mode_yolo", "", ""};
    if (name == "rule_allow") return {true, "probe ok", "allow", "auto", "mode_default", "", ""};
    if (name == "session_file") return {true, "probe ok", "allow", "session", "session_allow", "", ""};
    if (name == "goal") return {true, "probe ok", "allow", "goal", "unattended_goal", "", ""};
    if (name == "hook_allow") return {true, "probe ok", "allow", "hook", "hook_allowed", "allow", "hook"};
    if (name == "hook_deny") return {false, "[Hook denied permission] golden hook denial",
        "deny", "hook", "hook_denied", "deny", "hook"};
    if (name == "headless") return {false,
        "[Headless mode] This tool call requires interactive user confirmation, which is unavailable in print (-p) "
        "mode; it was denied automatically. Prefer a read-only alternative and continue. Rerun in an interactive "
        "session to approve this operation.", "deny", "headless", "headless_no_channel", "deny", "headless"};
    if (name == "interactive_deny") return {false, "[User denied tool execution]",
        "deny", "user", "confirmation", "deny", "interactive", true};
    if (name == "interactive_session") return {true, "probe ok",
        "allow_session", "user", "confirmation", "always_allow", "interactive", true, true};
    if (name == "implicit") return {true, "probe ok", "allow", "none", "implicit", "allow", "implicit"};
    if (name == "protected") return {false,
        "[Permission denied] Exec rules must be edited by the user.", "forbidden", "rule", "exec_rules_protected", "", ""};
    if (name == "yolo_deny") return {false, "[Permission denied by configured rule in yolo mode]",
        "forbidden", "rule", "deny_rule_yolo", "", ""};
    if (name == "exec_forbidden") return {false, "[Permission denied by configured exec rule]",
        "forbidden", "rule", "rule_forbidden", "", ""};
    if (name == "exec_escalation") return {false,
        "[Sandbox] Escalated or additional permissions cannot be approved while running unattended (active goal), "
        "so this call was not executed. Retry the same command without sandbox_permissions / "
        "with_escalated_permissions / additional_permissions; it will run inside the sandbox.",
        "forbidden", "goal", "escalation_unattended", "", ""};
    if (name == "exec_safe") return {true, "probe ok", "allow", "auto", "known_safe", "", ""};
    if (name == "exec_no_channel") return {false,
        "[Permission denied] This command requires approval, but no confirmation channel is available.",
        "deny", "none", "no_confirmation_channel", "", ""};
    if (name == "exec_danger_deny") return {false, "[User denied tool execution]",
        "deny", "user", "dangerous_command", "deny", "interactive", true};
    if (name == "interactive_allow") return {true, "probe ok", "allow", "user", "confirmation", "allow", "interactive", true};
    throw std::logic_error("unknown permission golden: " + name);
}

std::vector<PermissionCase> permission_cases() {
    using M = PermissionMode;
    using A = PermissionResult;
    return {
        {"DefaultReadSkipsEveryApproval", M::Default, "read_probe", "none", false, true, "deny", A::Deny, "read"},
        {"PlanReadSkipsEveryApproval", M::Plan, "read_probe", "none", true, true, "deny", A::Deny, "read"},
        {"DefaultWriteAllow", M::Default, "file_write", "none", false, false, "none", A::Allow},
        {"DefaultWriteDeny", M::Default, "file_write", "none", false, false, "none", A::Deny, "interactive_deny"},
        {"DefaultWriteSession", M::Default, "file_write", "none", false, false, "none", A::AlwaysAllow, "interactive_session"},
        {"AutoWriteOverridesHookDeny", M::Auto, "file_write", "none", false, true, "deny", A::Deny, "auto_file"},
        {"YoloWriteOverridesHookDeny", M::Yolo, "file_write", "none", false, true, "deny", A::Deny, "yolo_file"},
        {"PlanWriteStillConfirms", M::Plan, "file_write", "none", false, false, "none", A::Deny, "interactive_deny"},
        {"PlanDoesNotRememberAlwaysAllow", M::Plan, "file_write", "none", false, false, "none", A::AlwaysAllow, "interactive_session"},
        {"RuleAllowBeforeHook", M::Default, "file_write", "allow", false, true, "deny", A::Deny, "rule_allow"},
        {"ConfiguredDenyStillConfirms", M::Default, "file_write", "deny", false, false, "none", A::Allow},
        {"YoloRuleDenyBeforeGoalAndHook", M::Yolo, "file_write", "deny", true, true, "allow", A::Allow, "yolo_deny"},
        {"ProtectedRuleBeforeGoalAndHook", M::Yolo, "file_write", "protected", true, true, "allow", A::Allow, "protected"},
        {"SessionAllowBeforeHook", M::Default, "file_write", "session", false, true, "deny", A::Deny, "session_file"},
        {"GoalBeforeHookAndHeadless", M::Default, "file_write", "none", true, true, "deny", A::Deny, "goal"},
        {"PlanGoalCannotBypassHeadless", M::Plan, "file_write", "none", true, true, "none", A::Allow, "headless"},
        {"HookAllowBeforeHeadless", M::Default, "file_write", "none", false, true, "allow", A::Deny, "hook_allow"},
        {"HookDenyBeforeUserAllow", M::Default, "file_write", "none", false, false, "deny", A::Allow, "hook_deny"},
        {"HeadlessBeforeInteractive", M::Default, "file_write", "none", false, true, "none", A::Allow, "headless"},
        {"NonExecImplicitEmbedding", M::Default, "write_probe", "none", false, false, "none", std::nullopt, "implicit"},
        {"ExecKnownSafeBeforeHook", M::Auto, "bash", "none", false, true, "deny", A::Deny, "exec_safe", "git status"},
        {"ExecDangerousInteractiveDeny", M::Auto, "bash", "none", false, false, "none", A::Deny, "exec_danger_deny", "rm -rf output"},
        {"ExecForbiddenBeforeYoloGoalHook", M::Yolo, "bash", "exec_forbidden", true, true, "allow", A::Allow, "exec_forbidden", "git push"},
        {"ExecGoalDoesNotEscalate", M::Auto, "bash", "none", true, false, "allow", A::Allow, "exec_escalation", "pnpm test", true},
        {"ExecGoalKeepsDangerousInSandbox", M::Auto, "bash", "none", true, true, "deny", A::Deny, "goal", "rm -rf output"},
    };
}

class AgentLoopPermissionGolden : public testing::TestWithParam<PermissionCase> {};

// 场景:各模式、工具类别、规则、goal、headless、hook 与用户选择发生竞争。
// 期望:黄金决策链、文案、审计及授权一起稳定;回归会表现为绕过前置拒绝或重复审批。
TEST_P(AgentLoopPermissionGolden, PreservesDecisionAuditHookAndGrantSequence) {
    const auto test = GetParam();
    const auto expected = golden(test.verdict);
    Isolation isolation;
    Harness h(isolation, test.name, {}, test.answer.has_value());
    ScopedHeadless headless(test.headless);
    h.permissions.set_mode(test.mode);
    if (test.answer) h.observed->answer = *test.answer;
    const bool shell = std::string(test.tool) == "bash";
    const bool read_only = std::string(test.tool) == "read_probe";
    std::string path = acecode::path_to_utf8(h.cwd / "result.txt");
    if (std::string(test.rule) == "protected") path = acecode::path_to_utf8(h.cwd / ".acecode/rules/project.rules");
    auto tool = h.probe(test.tool, read_only);
    if (shell) {
        auto real_shell = create_bash_tool();
        real_shell.execute = tool.execute;
        tool = std::move(real_shell);
    }
    h.tools.register_tool(std::move(tool));
    const std::string rule = test.rule;
    if (rule == "allow" || rule == "deny") {
        // 规则 pattern 使用跨平台斜线,调用参数仍保留平台原生路径。
        auto rule_path = path;
        std::replace(rule_path.begin(), rule_path.end(), '\\', '/');
        h.permissions.add_rule({test.tool, rule_path, "", rule == "allow" ? RuleAction::Allow : RuleAction::Deny, 10});
    }
    if (rule == "session") h.permissions.add_session_allow(test.tool);
    if (rule == "exec_forbidden") {
        const auto parsed = sandbox::parse_rules_text(
            "prefix_rule(pattern=[\"git\", \"push\"], decision=\"forbidden\")",
            sandbox::RuleScope::Project, "golden.rules");
        ASSERT_TRUE(parsed.error.empty());
        sandbox::ExecRules rules;
        for (const auto& entry : parsed.rules) rules.add_rule(entry);
        h.loop->set_exec_rules(std::move(rules));
    }
    h.install_hooks({"PermissionRequest", "PermissionResolved"},
        [choice = std::string(test.hook)](const Json& payload) {
            if (payload["hook_event_name"] != "PermissionRequest" || choice == "none") return Json::object();
            return Json{{"hookSpecificOutput", {{"permissionDecision", choice},
                {"permissionDecisionReason", "golden hook denial"}}}};
        });
    if (test.goal) {
        ASSERT_TRUE(h.session->goal_store()->replace_thread_goal(
            h.session->current_session_id(), "golden unattended goal", std::nullopt, ThreadGoalStatus::Active));
        h.loop->restore_goal_runtime();
        h.observed->after_turn = [session = std::weak_ptr<SessionManager>(h.session)] {
            if (auto owned = session.lock()) owned->goal_store()->pause_active_thread_goal(owned->current_session_id());
        };
    }
    Json args = shell ? Json{{"command", test.command}} : Json{{"path", path}};
    if (test.escalation) {
        args["sandbox_permissions"] = "require_escalated";
        args["justification"] = "golden escalation request";
    }
    ASSERT_TRUE(h.run_calls({{"golden-call", test.tool, args.dump()}}));
    const auto state = h.observed;
    std::lock_guard<std::mutex> lock(state->mutex);
    ASSERT_EQ(state->results.size(), 1u);
    EXPECT_EQ(state->results[0].success, expected.executed);
    EXPECT_EQ(state->results[0].output, expected.output);
    EXPECT_EQ(state->executions.size(), expected.executed ? 1u : 0u);
    EXPECT_EQ(state->confirmations, expected.confirmation ? 1 : 0);
    EXPECT_EQ(h.permissions.has_session_allow(test.tool),
        rule == "session" || (expected.session_allow && test.mode != PermissionMode::Plan));

    std::vector<std::string> sequence;
    if (!expected.resolved.empty()) {
        sequence.push_back("PermissionRequest");
        if (expected.confirmation) sequence.push_back("confirm");
        sequence.push_back("PermissionResolved:" + expected.resolved + ":" + expected.resolved_source);
        ASSERT_EQ(state->hooks.size(), 2u);
        EXPECT_EQ(state->hooks[0]["tool_name"], test.tool);
        EXPECT_EQ(state->hooks[1]["tool_name"], test.tool);
        EXPECT_EQ(state->hooks[0]["tool_input"], state->hooks[1]["tool_input"]);
    } else {
        EXPECT_TRUE(state->hooks.empty());
    }
    if (!expected.decision.empty()) {
        sequence.push_back("audit:" + expected.decision + ":" + expected.source);
        ASSERT_EQ(state->audits.size(), 1u);
        const auto& audit = state->audits[0];
        EXPECT_EQ(audit.decision, expected.decision);
        EXPECT_EQ(audit.source, expected.source);
        EXPECT_EQ(audit.reason, expected.reason);
        EXPECT_EQ(audit.tool, test.tool);
        EXPECT_EQ(audit.category, shell ? "command" : std::string(test.tool) == "file_write" ? "file" : "tool");
        EXPECT_EQ(audit.target, shell ? std::string(test.command) : path);
        EXPECT_EQ(audit.cwd, acecode::path_to_utf8(h.cwd));
        EXPECT_EQ(audit.session_id, h.session->current_session_id());
        EXPECT_EQ(audit.detail["mode"], PermissionManager::mode_name(test.mode));
        if (!shell) EXPECT_EQ(audit.detail, (Json{{"mode", PermissionManager::mode_name(test.mode)}}));
        if (shell && expected.executed) EXPECT_EQ(state->executions[0]["sandbox"], "workspace-write");
    } else {
        EXPECT_TRUE(state->audits.empty());
    }
    if (expected.executed) sequence.push_back("execute");
    EXPECT_EQ(state->decisions, sequence);
}

INSTANTIATE_TEST_SUITE_P(P0_11, AgentLoopPermissionGolden,
    testing::ValuesIn(permission_cases()),
    [](const testing::TestParamInfo<PermissionCase>& info) { return info.param.name; });

// 场景:同一会话先 AlwaysAllow 再写第二个文件。期望:第二次不再投递权限 hook,
// 审计来源由 user 变为 session;只断首次返回值会漏掉未记忆或过宽记忆的回归。
TEST(AgentLoopPermissionGrantGolden, FileGrantAffectsOnlyTheApprovedToolOnNextCall) {
    Isolation isolation;
    Harness h(isolation);
    h.tools.register_tool(h.probe("file_write", false));
    h.tools.register_tool(h.probe("file_edit", false));
    h.observed->answer = PermissionResult::AlwaysAllow;
    h.install_hooks({"PermissionRequest", "PermissionResolved"});
    ASSERT_TRUE(h.run_calls({{"grant-1", "file_write", Json{{"path", path_to_utf8(h.cwd / "one.txt")}}.dump()}}));
    EXPECT_TRUE(h.permissions.has_session_allow("file_write"));
    EXPECT_FALSE(h.permissions.has_session_allow("file_edit"));
    ASSERT_TRUE(h.run_calls({{"grant-2", "file_write", Json{{"path", path_to_utf8(h.cwd / "two.txt")}}.dump()}}));
    std::lock_guard<std::mutex> lock(h.observed->mutex);
    EXPECT_EQ(h.observed->confirmations, 1);
    EXPECT_EQ(h.observed->hooks.size(), 2u);
    EXPECT_EQ(h.observed->executions.size(), 2u);
    EXPECT_EQ(h.observed->decisions, (std::vector<std::string>{"PermissionRequest", "confirm",
        "PermissionResolved:always_allow:interactive", "audit:allow_session:user", "execute",
        "audit:allow:session", "execute"}));
    ASSERT_EQ(h.observed->audits.size(), 2u);
    EXPECT_EQ(h.observed->audits[1].reason, "session_allow");
}

// 场景:用户批准 bash 越权并选择本会话允许。期望:只记命令前缀,第二次沿用 bypass;
// 不能退化成整个 bash 工具永久免审,也不能把已批准的越权悄悄变回沙盒执行。
TEST(AgentLoopPermissionGrantGolden, EscalatedCommandGrantKeepsPrefixAndSandboxSideEffects) {
    Isolation isolation;
    Harness h(isolation);
    h.permissions.set_mode(PermissionMode::Auto);
    h.observed->answer = PermissionResult::AlwaysAllow;
    auto shell = create_bash_tool();
    shell.execute = h.probe("bash", false).execute;
    h.tools.register_tool(std::move(shell));
    h.install_hooks({"PermissionRequest", "PermissionResolved"});
    ASSERT_TRUE(h.run_calls({{"grant-shell-1", "bash", Json{{"command", "pnpm test"},
        {"sandbox_permissions", "require_escalated"}, {"justification", "golden approved escalation"}}.dump()}}));
    EXPECT_EQ(h.permissions.session_command_allow({"pnpm test"}), SessionCommandAllow::Bypass);
    EXPECT_EQ(h.permissions.session_command_allow({"pnpm install"}), SessionCommandAllow::None);
    EXPECT_FALSE(h.permissions.has_session_allow("bash"));
    ASSERT_TRUE(h.run_calls({{"grant-shell-2", "bash", R"({"command":"pnpm test --filter golden"})"}}));
    std::lock_guard<std::mutex> lock(h.observed->mutex);
    EXPECT_EQ(h.observed->confirmations, 1);
    EXPECT_EQ(h.observed->hooks.size(), 2u);
    ASSERT_EQ(h.observed->executions.size(), 2u);
    EXPECT_EQ(h.observed->executions[0]["sandbox"], "full-access");
    EXPECT_EQ(h.observed->executions[1]["sandbox"], "full-access");
    ASSERT_EQ(h.observed->audits.size(), 2u);
    EXPECT_EQ(h.observed->audits[0].decision, "allow_session");
    EXPECT_EQ(h.observed->audits[0].reason, "escalation_requested");
    EXPECT_EQ(h.observed->audits[0].detail["escalation_requested"], true);
    EXPECT_EQ(h.observed->audits[1].reason, "session_allow");
    EXPECT_EQ(h.observed->decisions, (std::vector<std::string>{"PermissionRequest", "confirm",
        "PermissionResolved:always_allow:interactive", "audit:allow_session:user", "execute",
        "audit:allow:session", "execute"}));
}

// 场景:无交互通道的 bash 需要审批,hook 没有给出决定。期望:拒绝而不执行;
// 当前该出口只发 PermissionRequest、不发 Resolved,明确锁定这一现状,不静默补事件。
TEST(AgentLoopPermissionGrantGolden, ExecWithoutConfirmationChannelCurrentlyLeavesRequestUnresolved) {
    Isolation isolation;
    Harness h(isolation, "no-exec-channel", {}, false);
    h.permissions.set_mode(PermissionMode::Auto);
    auto shell = create_bash_tool();
    shell.execute = h.probe("bash", false).execute;
    h.tools.register_tool(std::move(shell));
    h.install_hooks({"PermissionRequest", "PermissionResolved"});
    ASSERT_TRUE(h.run_calls({{"no-channel", "bash", R"({"command":"rm -rf output"})"}}));
    std::lock_guard<std::mutex> lock(h.observed->mutex);
    EXPECT_TRUE(h.observed->executions.empty());
    EXPECT_EQ(h.observed->confirmations, 0);
    ASSERT_EQ(h.observed->results.size(), 1u);
    EXPECT_EQ(h.observed->results[0].output,
        "[Permission denied] This command requires approval, but no confirmation channel is available.");
    ASSERT_EQ(h.observed->hooks.size(), 1u);
    EXPECT_EQ(h.observed->hooks[0]["hook_event_name"], "PermissionRequest");
    EXPECT_EQ(h.observed->decisions,
        (std::vector<std::string>{"PermissionRequest", "audit:deny:none"}));
    ASSERT_EQ(h.observed->audits.size(), 1u);
    EXPECT_EQ(h.observed->audits[0].reason, "no_confirmation_channel");
}
} // namespace
