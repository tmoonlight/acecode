#include <gtest/gtest.h>
#include "sandbox/exec_permission.hpp"

using namespace acecode;
using namespace acecode::sandbox;

// 场景:auto 模式的决策表主干。期望:未知命令沙盒不可用时确认、可用时进
// workspace-write 免确认;危险命令一律确认;已知安全放行;显式申请越权即便
// 是安全命令也要确认,且批准后是 full-access。
TEST(ExecDecision, AutoDecisionTable) {
    ExecDecisionInput input;
    input.mode = PermissionMode::Auto;
    input.kind = CommandKind::Unknown;
    EXPECT_EQ(decide_exec(input).verdict, ExecVerdict::Prompt);
    input.sandbox_available = true;
    EXPECT_EQ(decide_exec(input).verdict, ExecVerdict::Allow);
    EXPECT_EQ(decide_exec(input).sandbox, SandboxMode::WorkspaceWrite);
    input.kind = CommandKind::Dangerous;
    EXPECT_EQ(decide_exec(input).verdict, ExecVerdict::Prompt);
    input.kind = CommandKind::KnownSafe;
    EXPECT_EQ(decide_exec(input).verdict, ExecVerdict::Allow);
    input.escalation_requested = true;
    EXPECT_EQ(decide_exec(input).verdict, ExecVerdict::Prompt);
    EXPECT_EQ(decide_exec(input).sandbox, SandboxMode::FullAccess);
}

// 场景:命中 forbidden 规则,同时是 yolo 模式 + --dangerous 启动。期望:仍是
// Forbidden —— forbidden 是用户写下的硬禁令,优先级高于任何放行模式。
TEST(ExecDecision, ForbiddenSurvivesYoloAndDangerousMode) {
    ExecDecisionInput input;
    input.rule = RuleDecision::Forbidden;
    input.mode = PermissionMode::Yolo;
    input.dangerous_mode = true;
    EXPECT_EQ(decide_exec(input).verdict, ExecVerdict::Forbidden);
}

// 场景:default / plan 模式下已知安全命令,以及全局 allow 规则命中。期望:
// 沙盒可用时都进 read-only 沙盒 —— 这两个模式不该让 shell 写任何东西,
// 全局 allow 在 plan 下也不例外(只读约束优先)。
TEST(ExecDecision, DefaultsAndPlanPreferReadOnlyForSafeCommands) {
    ExecDecisionInput input;
    input.kind = CommandKind::KnownSafe;
    input.sandbox_available = true;
    for (const auto mode : {PermissionMode::Default, PermissionMode::Plan}) {
        input.mode = mode;
        EXPECT_EQ(decide_exec(input).sandbox, SandboxMode::ReadOnly);
    }
    input.rule = RuleDecision::Allow;
    EXPECT_EQ(decide_exec(input).sandbox, SandboxMode::ReadOnly);
}

// 场景:会话里记住的是"沙盒内批准"(不是越权批准),之后沙盒变得不可用;
// 以及项目 allow 规则碰上危险参数。期望:两者都回到确认 —— 沙盒内的批准
// 不能在沙盒失效后静默升级成完整访问,项目规则也压不过危险命令确认。
TEST(ExecDecision, SandboxedApprovalDoesNotEscalateWhenBackendFails) {
    ExecDecisionInput input;
    input.mode = PermissionMode::Auto;
    input.session_allow = SessionAllowKind::Sandboxed;
    EXPECT_EQ(decide_exec(input).verdict, ExecVerdict::Prompt);
    input.sandbox_available = true;
    input.kind = CommandKind::Dangerous;
    input.rule = RuleDecision::AllowSandboxed;
    EXPECT_EQ(decide_exec(input).verdict, ExecVerdict::Prompt);
}

// 场景:prompt 规则命中且模型同时显式申请越权。期望:确认一次,批准后按
// 申请给 full-access,而不是把模型的越权申请悄悄降级回沙盒内执行。
TEST(ExecDecision, RulePromptCanApproveExplicitEscalation) {
    ExecDecisionInput input;
    input.mode = PermissionMode::Auto;
    input.sandbox_available = true;
    input.rule = RuleDecision::Prompt;
    input.escalation_requested = true;
    EXPECT_EQ(decide_exec(input).verdict, ExecVerdict::Prompt);
    EXPECT_EQ(decide_exec(input).sandbox, SandboxMode::FullAccess);
}

// 场景:越权申请缺 justification / 类型错误;正常申请后用户"总是允许"。期望:
// 参数错误直接报错不弹确认;记住的是 `git commit` 这一个前缀(bypass),
// 同前缀免确认、`git push --force` 仍确认;切换模式清空记忆;bash 永远不会
// 被整个工具级"总是允许"。
TEST(ExecPermission, ValidatesEscalationBeforePromptAndRemembersOnlySpecificPrefixes) {
    PermissionManager permissions;
    permissions.set_mode(PermissionMode::Auto);
    EXPECT_FALSE(evaluate_exec_permission(R"({"command":"git status","with_escalated_permissions":true})",
        permissions, {}, true).error.empty());
    EXPECT_FALSE(evaluate_exec_permission(R"({"command":"git status","with_escalated_permissions":"yes"})",
        permissions, {}, true).error.empty());
    auto first = evaluate_exec_permission(R"({"command":"git commit -m x","with_escalated_permissions":true,"justification":"write repository metadata"})",
        permissions, {}, true);
    ASSERT_EQ(first.prefixes, std::vector<std::string>{"git commit"});
    EXPECT_EQ(first.arguments["permission"]["reason"], "escalation_requested");
    permissions.add_session_command_allow(first.prefixes[0], true);
    EXPECT_EQ(evaluate_exec_permission(first.arguments.dump(), permissions, {}, true).decision.verdict, ExecVerdict::Allow);
    EXPECT_EQ(evaluate_exec_permission(R"({"command":"git push --force"})", permissions, {}, true).decision.verdict, ExecVerdict::Prompt);
    permissions.set_mode(PermissionMode::Default);
    EXPECT_EQ(permissions.session_command_allow({"git commit"}), SessionCommandAllow::None);
    permissions.add_session_allow("bash");
    EXPECT_FALSE(permissions.has_session_allow("bash"));
}

// 场景:无人值守(active goal)下模型显式申请越权 / 额外权限(align-codex-sandboxing
// D1)。期望:一律 Forbidden(reason escalation_unattended),沙盒仍是模式沙盒;
// 会话授权已覆盖的额外权限申请不算申请,照常放行;forbidden 规则与 yolo 仍优先;
// 有人值守时同样的输入照旧 Prompt。
// 回归:曾经落到第 4 条 Prompt + FullAccess,再被 goal 自动放行,模型只要声明
// 越权就能出沙盒,已发布 0.9.15 里就有。
TEST(ExecDecision, UnattendedEscalationIsForbiddenNotAutoApproved) {
    ExecDecisionInput input;
    input.mode = PermissionMode::Auto;
    input.sandbox_available = true;
    input.unattended = true;
    input.escalation_requested = true;
    auto decision = decide_exec(input);
    EXPECT_EQ(decision.verdict, ExecVerdict::Forbidden);
    EXPECT_EQ(decision.reason, "escalation_unattended");
    EXPECT_EQ(decision.sandbox, SandboxMode::WorkspaceWrite);
    input.escalation_requested = false;
    input.additional_requested = true;
    EXPECT_EQ(decide_exec(input).verdict, ExecVerdict::Forbidden);
    input.additional_covered = true;
    EXPECT_EQ(decide_exec(input).verdict, ExecVerdict::Allow);
    input.additional_covered = false;
    input.mode = PermissionMode::Yolo;
    EXPECT_EQ(decide_exec(input).verdict, ExecVerdict::Allow) << "yolo 仍先于无人值守判定";
    input.mode = PermissionMode::Auto;
    input.rule = RuleDecision::Forbidden;
    EXPECT_EQ(decide_exec(input).reason, "rule_forbidden");
    input.rule = RuleDecision::NoMatch;
    input.unattended = false;
    input.escalation_requested = true;
    EXPECT_EQ(decide_exec(input).verdict, ExecVerdict::Prompt);
}

// 场景:模型申请额外权限(with_additional_permissions,D3)。期望:会话授权没覆盖
// 时 Prompt(reason additional_permissions_requested),批准后的沙盒是 workspace-write
// (留在沙盒里加宽),plan 模式仍只读,沙盒不可用时 full-access;已覆盖时按普通
// 命令走;prompt 规则叠加额外申请同样落在 workspace-write。
TEST(ExecDecision, AdditionalPermissionsPromptStaysSandboxed) {
    ExecDecisionInput input;
    input.mode = PermissionMode::Default;
    input.sandbox_available = true;
    input.additional_requested = true;
    auto decision = decide_exec(input);
    EXPECT_EQ(decision.verdict, ExecVerdict::Prompt);
    EXPECT_EQ(decision.reason, "additional_permissions_requested");
    EXPECT_EQ(decision.sandbox, SandboxMode::WorkspaceWrite);
    input.mode = PermissionMode::Plan;
    EXPECT_EQ(decide_exec(input).sandbox, SandboxMode::ReadOnly);
    input.mode = PermissionMode::Auto;
    input.sandbox_available = false;
    EXPECT_EQ(decide_exec(input).sandbox, SandboxMode::FullAccess);
    input.sandbox_available = true;
    input.additional_covered = true;
    input.kind = CommandKind::Unknown;
    EXPECT_EQ(decide_exec(input).reason, "unknown_command_sandboxed");
    input.additional_covered = false;
    input.rule = RuleDecision::Prompt;
    decision = decide_exec(input);
    EXPECT_EQ(decision.reason, "rule_prompt");
    EXPECT_EQ(decision.sandbox, SandboxMode::WorkspaceWrite);
}

// 场景:新参数面的校验与解析。期望:sandbox_permissions 非法值 / 类型错误报错;
// with_additional_permissions 缺 additional_permissions 或缺 justification 报错;
// 相对路径报错;`~` 路径展开成家目录;use_default 却带 additional_permissions 报错;
// 旧 with_escalated_permissions=true 等价 require_escalated;prefix_rule 空串报错。
TEST(ExecPermission, ValidatesAndParsesAdditionalPermissionArguments) {
    using nlohmann::json;
    EXPECT_FALSE(validate_escalation_arguments(json::parse(R"({"command":"x","sandbox_permissions":"maybe"})")).empty());
    EXPECT_FALSE(validate_escalation_arguments(json::parse(R"({"command":"x","sandbox_permissions":1})")).empty());
    EXPECT_FALSE(validate_escalation_arguments(json::parse(
        R"({"command":"x","sandbox_permissions":"with_additional_permissions","justification":"y"})")).empty());
    EXPECT_FALSE(validate_escalation_arguments(json::parse(
        R"({"command":"x","sandbox_permissions":"with_additional_permissions","additional_permissions":{"file_system":{"write":["/a"]}}})")).empty());
    EXPECT_FALSE(validate_escalation_arguments(json::parse(
        R"({"command":"x","sandbox_permissions":"with_additional_permissions","justification":"y","additional_permissions":{"file_system":{"write":["relative"]}}})")).empty());
    EXPECT_FALSE(validate_escalation_arguments(json::parse(
        R"({"command":"x","additional_permissions":{"network":{"enabled":true}}})")).empty());
    EXPECT_FALSE(validate_escalation_arguments(json::parse(R"({"command":"x","prefix_rule":["git",""]})")).empty());
    const auto ok = json::parse(
        R"({"command":"pnpm install","sandbox_permissions":"with_additional_permissions","justification":"cache",
            "additional_permissions":{"file_system":{"write":["~/.cache/pnpm","/opt/x"],"read":["~/.cache/pnpm"]},"network":{"enabled":true}},
            "prefix_rule":["pnpm","install"]})");
    EXPECT_TRUE(validate_escalation_arguments(ok).empty()) << validate_escalation_arguments(ok);
    EXPECT_EQ(requested_sandbox_permissions(ok), SandboxPermissionsRequest::WithAdditional);
    const auto parsed = parse_additional_permissions(ok, "/home/u");
    ASSERT_EQ(parsed.write.size(), 2u);
    EXPECT_NE(parsed.write[0].find(".cache"), std::string::npos);
    EXPECT_EQ(parsed.write[0].find('~'), std::string::npos) << "~ 已展开";
    EXPECT_EQ(parsed.read.size(), 1u);
    EXPECT_TRUE(parsed.network);
    EXPECT_EQ(requested_prefix_rule(ok), (std::vector<std::string>{"pnpm", "install"}));
    EXPECT_EQ(requested_sandbox_permissions(json::parse(R"({"with_escalated_permissions":true})")),
              SandboxPermissionsRequest::RequireEscalated);
    EXPECT_EQ(requested_sandbox_permissions(json::parse(R"({"sandbox_permissions":"use_default"})")),
              SandboxPermissionsRequest::UseDefault);
}

// 场景:完整评估一条额外权限申请。期望:permission payload 带 request /
// additional_permissions / proposed_prefix_rule;会话授权覆盖后不再 Prompt 且
// 决策落在 workspace-write;无人值守标记透传。
TEST(ExecPermission, EvaluatesAdditionalPermissionsAgainstSessionGrants) {
    PermissionManager permissions;
    permissions.set_mode(PermissionMode::Auto);
    ExecPermissionOptions options;
    options.home = "/home/u";
    const std::string args = R"({"command":"pnpm install","sandbox_permissions":"with_additional_permissions",
        "justification":"cache","additional_permissions":{"file_system":{"write":["/home/u/.cache/pnpm"]}}})";
    auto first = evaluate_exec_permission(args, permissions, {}, true, CommandPlatform::Posix, options);
    ASSERT_TRUE(first.error.empty()) << first.error;
    EXPECT_EQ(first.decision.verdict, ExecVerdict::Prompt);
    EXPECT_EQ(first.decision.reason, "additional_permissions_requested");
    EXPECT_EQ(first.arguments["permission"]["request"], "with_additional_permissions");
    EXPECT_EQ(first.arguments["permission"]["additional_permissions"]["write"][0], "/home/u/.cache/pnpm");
    EXPECT_EQ(first.arguments["permission"]["proposed_prefix_rule"], "pnpm install");
    EXPECT_FALSE(first.arguments["permission"].contains("unattended"));
    options.session_grants.write = {"/home/u/.cache"};
    auto covered = evaluate_exec_permission(args, permissions, {}, true, CommandPlatform::Posix, options);
    EXPECT_EQ(covered.decision.verdict, ExecVerdict::Allow);
    EXPECT_EQ(covered.decision.sandbox, SandboxMode::WorkspaceWrite);
    options.session_grants = {};
    options.unattended = true;
    auto unattended = evaluate_exec_permission(args, permissions, {}, true, CommandPlatform::Posix, options);
    EXPECT_EQ(unattended.decision.verdict, ExecVerdict::Forbidden);
    EXPECT_EQ(unattended.arguments["permission"]["unattended"], true);
}

// 场景:旧模式名解析与规则目录保护。期望:auto / accept-edits / acceptEdits
// 都解析成 Auto 且归一成 "auto";未知名字返回空;内置 Deny 规则命中
// `.acecode/rules/` 下的文件写入;Windows 反斜杠路径也能识别为规则目录。
TEST(ExecPermission, LegacyModeNamesAndRuleDirectoryProtection) {
    for (const char* name : {"auto", "accept-edits", "acceptEdits"}) {
        EXPECT_EQ(PermissionManager::parse_mode_name(name), PermissionMode::Auto);
        EXPECT_EQ(PermissionManager::canonical_mode_name(name), "auto");
    }
    EXPECT_FALSE(PermissionManager::parse_mode_name("banana"));
    PermissionManager permissions;
    permissions.set_mode(PermissionMode::Auto);
    EXPECT_EQ(permissions.matched_rule("file_write", ".acecode/rules/default.rules"), RuleAction::Deny);
    EXPECT_TRUE(is_exec_rules_path("C:\\project\\.acecode\\rules\\default.rules"));
}

// 场景:自动模式（试验）与 auto 共用整张 exec 决策表(openspec add-auto-review-mode D2)。
// 触发:把同一组输入分别按 Auto 与 AutoReview 求值,覆盖命令分类、规则结论、
// 沙盒是否可用、越权 / 额外权限申请、无人值守、会话记忆的全部组合。
// 期望:verdict、执行沙盒、原因三项逐一相同。
// 回归意义:mode_sandbox 的兜底是完整访问,新模式漏分支时沙盒可用也不进沙盒,
// 这条用例会在 sandbox 一项上立刻失败。
TEST(ExecDecision, AutoReviewMatchesAutoForEveryInput) {
    for (const auto kind : {CommandKind::KnownSafe, CommandKind::Unknown, CommandKind::Dangerous}) {
        for (const auto rule : {RuleDecision::NoMatch, RuleDecision::Allow, RuleDecision::AllowSandboxed,
                                RuleDecision::Prompt, RuleDecision::Forbidden}) {
            for (const auto session : {SessionAllowKind::None, SessionAllowKind::Sandboxed,
                                       SessionAllowKind::Bypass}) {
                for (int bits = 0; bits < 16; ++bits) {
                    ExecDecisionInput input;
                    input.kind = kind;
                    input.rule = rule;
                    input.session_allow = session;
                    input.sandbox_available = (bits & 1) != 0;
                    input.escalation_requested = (bits & 2) != 0;
                    input.additional_requested = (bits & 4) != 0;
                    input.unattended = (bits & 8) != 0;
                    input.mode = PermissionMode::Auto;
                    const auto expected = decide_exec(input);
                    input.mode = PermissionMode::AutoReview;
                    const auto actual = decide_exec(input);
                    EXPECT_EQ(actual.verdict, expected.verdict) << expected.reason;
                    EXPECT_EQ(actual.sandbox, expected.sandbox) << expected.reason;
                    EXPECT_EQ(actual.reason, expected.reason);
                }
            }
        }
    }
    EXPECT_EQ(mode_sandbox(PermissionMode::AutoReview, true), SandboxMode::WorkspaceWrite);
    EXPECT_EQ(mode_sandbox(PermissionMode::AutoReview, false), SandboxMode::FullAccess);
}
