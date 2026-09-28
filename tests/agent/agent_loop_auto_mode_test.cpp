#include "test_support/agent/agent_loop_fixture.hpp"
#include <gtest/gtest.h>
#include "agent/agent_loop.hpp"
#include "test_support/agent/stub_provider.hpp"
#include "tool/bash_tool.hpp"
#include "test_support/sandbox/test_support.hpp"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <condition_variable>
#include <chrono>
#include <memory>
#include <mutex>

using namespace acecode;
using namespace acecode::sandbox;
using namespace std::chrono_literals;

namespace {
class AutoHarness {
public:
    test::TempTree tree;
    PermissionManager permissions;
    ToolExecutor tools;
    AgentCallbacks callbacks;
    std::shared_ptr<acecode_test::StubLlmProvider> provider = std::make_shared<acecode_test::StubLlmProvider>();
    std::unique_ptr<AgentLoop> loop;
    std::vector<nlohmann::json> prompts;
    std::vector<SandboxMode> executions;
    std::vector<std::optional<SandboxPolicy>> policies;   // 每次执行时注入的策略(不沙盒 = nullopt)
    std::optional<ToolResult> next_result;                 // 下一次 bash 执行返回的结果(模拟沙盒拒绝)
    std::vector<security::AuditEntry> audits;              // 审批门写出的审计记录(openspec add-security-center)
    PermissionResult answer = PermissionResult::Allow;
    std::mutex mutex;
    std::condition_variable cv;
    bool busy = false;

    explicit AutoHarness(bool available = true, bool has_prompter = true) {
        permissions.set_mode(PermissionMode::Auto);
        auto bash = create_bash_tool();
        bash.execute = [this](const std::string&, const ToolContext& ctx) {
            executions.push_back(ctx.exec_sandbox ? ctx.exec_sandbox->policy.mode : SandboxMode::FullAccess);
            policies.push_back(ctx.exec_sandbox ? std::optional<SandboxPolicy>(ctx.exec_sandbox->policy) : std::nullopt);
            if (next_result) {
                auto result = *next_result;
                next_result.reset();
                return result;
            }
            return ToolResult{"ok", true};
        };
        tools.register_tool(bash);
        callbacks.on_busy_changed = [this](bool value) {
            std::lock_guard<std::mutex> lock(mutex);
            busy = value;
            if (!busy) cv.notify_all();
        };
        if (has_prompter) callbacks.on_tool_confirm = [this](const std::string&, const std::string& args) {
            prompts.push_back(nlohmann::json::parse(args));
            return answer;
        };
        loop = std::make_unique<AgentLoop>(
        acecode_test::AgentLoopFixture::dependencies([this]() -> std::shared_ptr<LlmProvider> { return provider; }, tools, callbacks, permissions),
        acecode_test::AgentLoopFixture::configuration(path_to_utf8(tree.root)));
        loop->start();
        loop->set_exec_rules({});
        loop->set_sandbox_availability_for_tests(available);
        loop->set_audit_sink([this](const security::AuditEntry& entry) { audits.push_back(entry); });
    }
    ~AutoHarness() { loop.reset(); }
    bool run(nlohmann::json args) {
        provider->push_tool_call("bash", args.dump(), "call-" + std::to_string(provider->turn_count()));
        provider->push_text("done");
        {
            std::lock_guard<std::mutex> lock(mutex);
            busy = true;
        }
        loop->submit("run");
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, 10s, [this] { return !busy; });
    }
};
}

// 场景:auto 模式 + 沙盒可用,模型先后执行 `git status`(已知安全)与
// `pnpm test`(未知)。期望:两次都不弹确认,且都带着 workspace-write 沙盒
// 请求进工具 —— 这就是 Codex Auto 预设的日常体验。
TEST(AgentLoopAutoMode, SafeAndUnknownCommandsRunInsideAvailableSandbox) {
    AutoHarness h;
    ASSERT_TRUE(h.run({{"command", "git status"}}));
    ASSERT_TRUE(h.run({{"command", "pnpm test"}}));
    EXPECT_TRUE(h.prompts.empty());
    ASSERT_EQ(h.executions.size(), 2u);
    EXPECT_EQ(h.executions[0], SandboxMode::WorkspaceWrite);
    EXPECT_EQ(h.executions[1], SandboxMode::WorkspaceWrite);
}

// 场景:auto 模式下模型执行 `rm -rf output`,用户拒绝。期望:弹一次确认,
// 确认 args 的 permission.reason 是 dangerous_command,拒绝后工具不执行。
TEST(AgentLoopAutoMode, DangerousCommandRequiresConfirmationAndCanBeDenied) {
    AutoHarness h;
    h.answer = PermissionResult::Deny;
    ASSERT_TRUE(h.run({{"command", "rm -rf output"}}));
    EXPECT_TRUE(h.executions.empty());
    ASSERT_EQ(h.prompts.size(), 1u);
    EXPECT_EQ(h.prompts[0]["permission"]["reason"], "dangerous_command");
}

// 场景:auto 模式但沙盒不可用(Codex 的 Windows-disabled 分支)。期望:已知
// 安全命令仍免确认;未知命令弹确认(reason=unknown_command_without_sandbox),
// 批准后以 full-access 执行。
TEST(AgentLoopAutoMode, UnavailableSandboxPromptsForUnknownButAllowsKnownSafe) {
    AutoHarness h(false);
    ASSERT_TRUE(h.run({{"command", "git status"}}));
    ASSERT_TRUE(h.run({{"command", "pnpm test"}}));
    ASSERT_EQ(h.prompts.size(), 1u);
    EXPECT_EQ(h.prompts[0]["permission"]["reason"], "unknown_command_without_sandbox");
    ASSERT_EQ(h.executions.size(), 2u);
    EXPECT_EQ(h.executions.back(), SandboxMode::FullAccess);
}

// 场景:模型带 with_escalated_permissions + justification 执行 `pnpm install`,
// 用户选"总是允许";随后 `pnpm install lodash` 与 `pnpm test`。期望:确认框
// 带 reason=escalation_requested、sandbox=full-access、前缀 `pnpm install` 与
// 理由原文;同前缀第二次免确认且沙盒外执行;`pnpm test` 不沾光,照常进沙盒。
TEST(AgentLoopAutoMode, EscalationShowsJustificationAndRemembersOnlyApprovedPrefix) {
    AutoHarness h;
    h.answer = PermissionResult::AlwaysAllow;
    ASSERT_TRUE(h.run({{"command", "pnpm install"}, {"with_escalated_permissions", true},
                      {"justification", "Install dependencies using the shared cache."}}));
    ASSERT_EQ(h.prompts.size(), 1u);
    EXPECT_EQ(h.prompts[0]["permission"]["reason"], "escalation_requested");
    EXPECT_EQ(h.prompts[0]["permission"]["sandbox"], "full-access");
    EXPECT_EQ(h.prompts[0]["permission"]["always_allow_prefix"], "pnpm install");
    EXPECT_EQ(h.prompts[0]["justification"], "Install dependencies using the shared cache.");
    ASSERT_TRUE(h.run({{"command", "pnpm install lodash"}}));
    ASSERT_TRUE(h.run({{"command", "pnpm test"}}));
    EXPECT_EQ(h.prompts.size(), 1u);
    ASSERT_EQ(h.executions.size(), 3u);
    EXPECT_EQ(h.executions[0], SandboxMode::FullAccess);
    EXPECT_EQ(h.executions[1], SandboxMode::FullAccess);
    EXPECT_EQ(h.executions[2], SandboxMode::WorkspaceWrite);
}

// 场景:yolo 模式 + 项目规则 forbidden `git push`。期望:不弹确认、不执行 ——
// forbidden 是硬禁令,yolo 只能跳过确认,不能跳过禁令。
TEST(AgentLoopAutoMode, ForbiddenRuleWinsEvenInYoloWithoutPrompt) {
    AutoHarness h;
    h.permissions.set_mode(PermissionMode::Yolo);
    ExecRules rules;
    auto parsed = parse_rules_text("prefix_rule(pattern=[\"git\", \"push\"], decision=\"forbidden\")",
                                   RuleScope::Project, "project.rules");
    ASSERT_TRUE(parsed.error.empty());
    rules.add_rule(parsed.rules.front());
    h.loop->set_exec_rules(std::move(rules));
    ASSERT_TRUE(h.run({{"command", "git push"}}));
    EXPECT_TRUE(h.prompts.empty());
    EXPECT_TRUE(h.executions.empty());
}

// 场景:沙盒不可用且宿主没有装确认回调(非交互嵌入)。期望:缺 justification
// 的越权申请直接报参数错误;需要确认的未知命令因为没有确认通道也不执行 ——
// 两条路径都不能退化成"没人问就直接跑"。
TEST(AgentLoopAutoMode, InvalidEscalationAndMissingPrompterNeverExecute) {
    AutoHarness h(false, false);
    ASSERT_TRUE(h.run({{"command", "pnpm test"}, {"with_escalated_permissions", true}}));
    ASSERT_TRUE(h.run({{"command", "pnpm test"}}));
    EXPECT_TRUE(h.executions.empty());
}

// 场景:会话里已记住 `pnpm test`(bypass),用户 `/sandbox off` 再 `/sandbox on`。
// 期望:开关翻转即清空前缀记忆;off 期间未知命令按沙盒不可用弹确认;on 之后
// 恢复进 workspace-write 沙盒且不再确认。
TEST(AgentLoopAutoMode, SessionSandboxSwitchClearsRememberedPrefixes) {
    AutoHarness h;
    h.permissions.add_session_command_allow("pnpm test", true);
    EXPECT_NE(h.loop->sandbox_command("off").find("disabled for this session"), std::string::npos);
    EXPECT_TRUE(h.permissions.session_command_allows().empty());
    ASSERT_TRUE(h.run({{"command", "pnpm test"}}));
    EXPECT_EQ(h.prompts.size(), 1u);
    h.loop->sandbox_command("on");
    ASSERT_TRUE(h.run({{"command", "pnpm test"}}));
    EXPECT_EQ(h.prompts.size(), 1u);
    EXPECT_EQ(h.executions.back(), SandboxMode::WorkspaceWrite);
}

namespace {
bool has_writable_root(const std::optional<SandboxPolicy>& policy, const std::filesystem::path& dir) {
    if (!policy) return false;
    const auto wanted = path_to_utf8(std::filesystem::weakly_canonical(dir));
    for (const auto& root : policy->writable_roots) {
        if (root.root == wanted) return true;
    }
    return false;
}
}

// 场景:模型带 sandbox_permissions=with_additional_permissions 申请给 `<tree>/extra`
// 写权限(align-codex-sandboxing D3),用户选「本次会话保留这些权限」。期望:
// 弹一次确认(reason=additional_permissions_requested,sandbox=workspace-write,
// payload 列出申请的路径);批准后命令留在 workspace-write 里、可写根含 extra;
// 同样的申请第二次不再确认(会话授权已覆盖);普通命令也带着授权跑。
TEST(AgentLoopAutoMode, AdditionalPermissionsStaySandboxedAndCanBeKeptForSession) {
    AutoHarness h;
    const auto extra = h.tree.dir("extra");
    h.answer = PermissionResult::AlwaysAllow;
    const nlohmann::json request = {{"command", "pnpm install"},
        {"sandbox_permissions", "with_additional_permissions"},
        {"justification", "Needs the shared store."},
        {"additional_permissions", {{"file_system", {{"write", {path_to_utf8(extra)}}}}}}};
    ASSERT_TRUE(h.run(request));
    ASSERT_EQ(h.prompts.size(), 1u);
    EXPECT_EQ(h.prompts[0]["permission"]["reason"], "additional_permissions_requested");
    EXPECT_EQ(h.prompts[0]["permission"]["sandbox"], "workspace-write");
    EXPECT_EQ(h.prompts[0]["permission"]["request"], "with_additional_permissions");
    EXPECT_EQ(h.prompts[0]["permission"]["additional_permissions"]["write"].size(), 1u);
    ASSERT_EQ(h.executions.size(), 1u);
    EXPECT_EQ(h.executions[0], SandboxMode::WorkspaceWrite);
    EXPECT_TRUE(has_writable_root(h.policies[0], extra));
    ASSERT_TRUE(h.run(request));
    ASSERT_TRUE(h.run({{"command", "pnpm test"}}));
    EXPECT_EQ(h.prompts.size(), 1u) << "会话授权已覆盖,不再确认";
    ASSERT_EQ(h.executions.size(), 3u);
    EXPECT_TRUE(has_writable_root(h.policies[1], extra));
    EXPECT_TRUE(has_writable_root(h.policies[2], extra)) << "普通命令也带着会话授权";
}

// 场景:命令在沙盒里因为写 `<tree>/outside/x.txt` 被拒(工具结果带
// sandbox_violation),模型随后带越权申请重试,用户选「只放行该目录」(D4)。
// 期望:越权确认的 payload 带 denied_path 与 scoped_write_root=<tree>/outside;
// 批准后命令仍在 workspace-write 里跑、可写根多了 outside;不记 bypass 前缀,
// 之后同前缀命令照常进沙盒;成功执行后陈旧的被拒路径作废,再次越权申请不再
// 提供 scoped 选项。
TEST(AgentLoopAutoMode, ScopedApprovalGrantsOnlyTheDeniedDirectory) {
    AutoHarness h;
    // Both fixtures live in the system temp directory, which is writable by
    // default on POSIX. Exclude it so the simulated denial is valid everywhere.
    SandboxConfig config;
    config.exclude_tmpdir = true;
    h.loop->set_sandbox_config(config);
    // 被拒目录必须在工作区(harness cwd = tree.root)之外。
    test::TempTree elsewhere;
    const auto outside = elsewhere.dir("outside");
    ToolResult denied{"sh: " + path_to_utf8(outside / "x.txt") + ": Permission denied", false};
    denied.metadata["sandbox_denied"] = true;
    denied.metadata["sandbox_violation"] = {{"reason", "permission_denied"},
        {"path", path_to_utf8(outside / "x.txt")}, {"snippet", "Permission denied"}};
    h.next_result = denied;
    ASSERT_TRUE(h.run({{"command", "pnpm install"}}));
    EXPECT_TRUE(h.prompts.empty());
    ASSERT_EQ(h.policies.size(), 1u);
    ASSERT_TRUE(h.policies[0].has_value());
    ASSERT_FALSE(h.policies[0]->can_write(path_to_utf8(outside)));
    h.answer = PermissionResult::AllowScoped;
    ASSERT_TRUE(h.run({{"command", "pnpm install"}, {"sandbox_permissions", "require_escalated"},
                      {"justification", "write the report"}}));
    ASSERT_EQ(h.prompts.size(), 1u);
    EXPECT_EQ(h.prompts[0]["permission"]["reason"], "escalation_requested");
    EXPECT_EQ(h.prompts[0]["permission"]["denied_path"], path_to_utf8(outside / "x.txt"));
    EXPECT_EQ(h.prompts[0]["permission"]["scoped_write_root"], path_to_utf8(std::filesystem::weakly_canonical(outside)));
    ASSERT_EQ(h.executions.size(), 2u);
    EXPECT_EQ(h.executions[1], SandboxMode::WorkspaceWrite) << "只放行目录 = 留在沙盒里";
    EXPECT_TRUE(has_writable_root(h.policies[1], outside));
    ASSERT_TRUE(h.run({{"command", "pnpm install lodash"}}));
    EXPECT_EQ(h.executions.back(), SandboxMode::WorkspaceWrite);
    EXPECT_EQ(h.prompts.size(), 1u);
    h.answer = PermissionResult::Deny;
    ASSERT_TRUE(h.run({{"command", "pnpm publish"}, {"with_escalated_permissions", true},
                      {"justification", "publish"}}));
    ASSERT_EQ(h.prompts.size(), 2u);
    EXPECT_FALSE(h.prompts[1]["permission"].contains("scoped_write_root")) << "成功执行后陈旧路径作废";
}

// 场景:模型越权申请 `pnpm install`,用户选「以后都允许」(D6)。期望:确认框 payload
// 带 proposed_prefix_rule=`pnpm install`;批准后本次沙盒外执行;规则写进临时规则
// 目录的 default.rules(沙盒外批准);随后不带越权的 `pnpm install lodash` 不再确认
// 且按全局 allow 规则沙盒外执行。对照:auto 下危险命令 `rm -rf output` 的确认
// 不提供记住选项(`rm` 在禁用名单),用户即便选「以后都允许」也只降级为会话允许。
TEST(AgentLoopAutoMode, RememberWritesRuleFileAndSkipsFuturePrompts) {
    AutoHarness h;
    const auto rules_dir = h.tree.root / "rules";
    h.loop->set_exec_rules_dir_for_tests(path_to_utf8(rules_dir));
    h.answer = PermissionResult::AllowRemember;
    ASSERT_TRUE(h.run({{"command", "pnpm install"}, {"sandbox_permissions", "require_escalated"},
                      {"justification", "Install into the shared store."}}));
    ASSERT_EQ(h.prompts.size(), 1u);
    EXPECT_EQ(h.prompts[0]["permission"]["proposed_prefix_rule"], "pnpm install");
    ASSERT_EQ(h.executions.size(), 1u);
    EXPECT_EQ(h.executions[0], SandboxMode::FullAccess);
    std::ifstream in(rules_dir / kRememberedRulesFile);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("prefix_rule(pattern=[\"pnpm\", \"install\"], decision=\"allow\")"), std::string::npos) << content;
    ASSERT_TRUE(h.run({{"command", "pnpm install lodash"}}));
    EXPECT_EQ(h.prompts.size(), 1u);
    EXPECT_EQ(h.executions.back(), SandboxMode::FullAccess);
    ASSERT_TRUE(h.run({{"command", "rm -rf output"}}));
    ASSERT_EQ(h.prompts.size(), 2u);
    EXPECT_EQ(h.prompts[1]["permission"]["reason"], "dangerous_command");
    EXPECT_FALSE(h.prompts[1]["permission"].contains("proposed_prefix_rule"));
    EXPECT_FALSE(std::filesystem::exists(rules_dir / kRememberedSandboxedRulesFile));
    EXPECT_EQ(h.executions.back(), SandboxMode::WorkspaceWrite);
}

// 场景:auto 模式下危险命令 `git push --force` 的确认,用户选「以后都允许」。期望:
// 沙盒内批准写到 default.sandboxed.rules,加载后 allow 降级为 AllowSandboxed;
// 但新出现的危险参数仍要确认(危险命令判定先于项目 / 沙盒 allow),所以第二次
// `git push --force` 仍弹确认 —— 记住的是免确认的普通 `git push` 用法。
TEST(AgentLoopAutoMode, SandboxedApprovalIsRememberedInSandboxedRulesFile) {
    AutoHarness h;
    const auto rules_dir = h.tree.root / "rules";
    h.loop->set_exec_rules_dir_for_tests(path_to_utf8(rules_dir));
    h.answer = PermissionResult::AllowRemember;
    ASSERT_TRUE(h.run({{"command", "git push --force"}}));
    ASSERT_EQ(h.prompts.size(), 1u);
    EXPECT_EQ(h.prompts[0]["permission"]["reason"], "dangerous_command");
    EXPECT_EQ(h.prompts[0]["permission"]["proposed_prefix_rule"], "git push");
    EXPECT_EQ(h.executions.back(), SandboxMode::WorkspaceWrite);
    std::ifstream in(rules_dir / kRememberedSandboxedRulesFile);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("prefix_rule(pattern=[\"git\", \"push\"], decision=\"allow\")"), std::string::npos) << content;
    EXPECT_FALSE(std::filesystem::exists(rules_dir / kRememberedRulesFile));
    ASSERT_TRUE(h.run({{"command", "git push --force"}}));
    EXPECT_EQ(h.prompts.size(), 2u) << "危险参数不能被记住的 allow 悄悄放行";
}

// 场景:配置里的普通 Deny 规则(`.env` 写入,priority 10)与内置保护级 Deny
// (priority 1000)分别命中 file_write(align-codex-sandboxing D10)。期望:普通
// Deny 回到弹确认,批准后执行(Desktop 没有 --dangerous 也有逃生口);保护级 Deny
// 仍硬拒绝、不弹确认。
TEST(AgentLoopAutoMode, ConfiguredDenyRulePromptsWhileBuiltinProtectionHardDenies) {
    AutoHarness h;
    h.permissions.add_rule({"file_write", "**/.env", "", RuleAction::Deny, 10});
    h.permissions.add_rule({"file_write", "**/protected/**", "", RuleAction::Deny,
                            PermissionManager::kBuiltinProtectionPriority});
    int writes = 0;
    ToolDef def;
    def.name = "file_write";
    def.description = "test";
    def.parameters = nlohmann::json({{"type", "object"}, {"properties", {{"file_path", {{"type", "string"}}}}}});
    ToolImpl tool{def, [&](const std::string&, const ToolContext&) { ++writes; return ToolResult{"ok", true}; }, false};
    ASSERT_TRUE(h.tools.register_tool(tool));
    auto run_write = [&](const std::string& path, const char* id) {
        h.provider->push_tool_call("file_write", nlohmann::json{{"file_path", path}, {"content", "x"}}.dump(), id);
        h.provider->push_text("done");
        {
            std::lock_guard<std::mutex> lock(h.mutex);
            h.busy = true;
        }
        h.loop->submit("write");
        std::unique_lock<std::mutex> lock(h.mutex);
        return h.cv.wait_for(lock, 10s, [&h] { return !h.busy; });
    };
    h.answer = PermissionResult::Allow;
    ASSERT_TRUE(run_write(path_to_utf8(h.tree.root / ".env"), "call-env"));
    EXPECT_EQ(h.prompts.size(), 1u) << "普通 Deny 规则改为弹确认";
    EXPECT_EQ(writes, 1);
    ASSERT_TRUE(run_write(path_to_utf8(h.tree.root / "protected" / "x"), "call-protected"));
    EXPECT_EQ(h.prompts.size(), 1u) << "保护级 Deny 不弹确认";
    EXPECT_EQ(writes, 1) << "保护级 Deny 硬拒绝";
}

// 场景(openspec add-security-center):auto 模式下模型先执行已知安全的 `git status`
// (自动放行),再执行 `rm -rf build`(危险命令弹确认,用户拒绝),再执行一条命中
// forbidden 规则的 `curl x`。期望:三条各记一条审计 —— command/allow/auto(带模式
// 沙盒)、command/deny/user(reason=dangerous_command)、command/forbidden/rule;
// 每条都带 cwd 与命令原文,detail.mode=auto。
TEST(AgentLoopAutoMode, AuditsAutoAllowUserDenyAndRuleForbidden) {
    AutoHarness h;
    ASSERT_TRUE(h.run({{"command", "git status"}}));
    ASSERT_EQ(h.audits.size(), 1u);
    EXPECT_EQ(h.audits[0].category, "command");
    EXPECT_EQ(h.audits[0].decision, "allow");
    EXPECT_EQ(h.audits[0].source, "auto");
    EXPECT_EQ(h.audits[0].reason, "known_safe");
    EXPECT_EQ(h.audits[0].tool, "bash");
    EXPECT_EQ(h.audits[0].target, "git status");
    EXPECT_EQ(h.audits[0].sandbox, "workspace-write");
    EXPECT_EQ(h.audits[0].cwd, path_to_utf8(h.tree.root));
    EXPECT_EQ(h.audits[0].detail["mode"], "auto");

    h.answer = PermissionResult::Deny;
    ASSERT_TRUE(h.run({{"command", "rm -rf build"}}));
    ASSERT_EQ(h.audits.size(), 2u);
    EXPECT_EQ(h.audits[1].decision, "deny");
    EXPECT_EQ(h.audits[1].source, "user");
    EXPECT_EQ(h.audits[1].reason, "dangerous_command");
    EXPECT_EQ(h.audits[1].target, "rm -rf build");

    ExecRules rules;
    auto parsed = parse_rules_text("prefix_rule(pattern=[\"curl\"], decision=\"forbidden\")",
                                   RuleScope::Global, "global.rules");
    ASSERT_TRUE(parsed.error.empty());
    rules.add_rule(parsed.rules.front());
    h.loop->set_exec_rules(std::move(rules));
    ASSERT_TRUE(h.run({{"command", "curl http://x"}}));
    ASSERT_EQ(h.audits.size(), 3u);
    EXPECT_EQ(h.audits[2].decision, "forbidden");
    EXPECT_EQ(h.audits[2].source, "rule");
    EXPECT_EQ(h.audits[2].reason, "rule_forbidden");
    EXPECT_EQ(h.executions.size(), 1u) << "被拒 / 禁止的命令不执行";
}

// 场景:命令在沙盒里被拒(工具结果带 sandbox_violation 与路径)。期望:除了那条
// command/allow 之外,额外记一条 sandbox/blocked/sandbox,target 是被拒路径、
// detail.command 是命令原文 —— 文件安全页「最近被拦路径」就是按它聚合的。
TEST(AgentLoopAutoMode, AuditsSandboxViolationWithDeniedPath) {
    AutoHarness h;
    ToolResult denied{"sh: D:/data/out/x.txt: Permission denied", false};
    denied.metadata["sandbox_denied"] = true;
    denied.metadata["sandbox_violation"] = {{"reason", "permission_denied"},
        {"path", "D:/data/out/x.txt"}, {"snippet", "Permission denied"}};
    h.next_result = denied;
    ASSERT_TRUE(h.run({{"command", "pnpm install"}}));
    ASSERT_EQ(h.audits.size(), 2u);
    EXPECT_EQ(h.audits[0].category, "command");
    EXPECT_EQ(h.audits[0].decision, "allow");
    EXPECT_EQ(h.audits[1].category, "sandbox");
    EXPECT_EQ(h.audits[1].decision, "blocked");
    EXPECT_EQ(h.audits[1].source, "sandbox");
    EXPECT_EQ(h.audits[1].reason, "permission_denied");
    EXPECT_EQ(h.audits[1].target, "D:/data/out/x.txt");
    EXPECT_EQ(h.audits[1].detail["command"], "pnpm install");
    EXPECT_EQ(h.audits[1].detail["snippet"], "Permission denied");
}

// 场景:用户对越权申请选「以后都允许」(写规则文件)。期望:除 command/allow_remember/user
// 之外,再记一条 rule/allow_remember(target 是前缀展示 `pnpm install`,reason=remember_rule);
// 同一命令随后免确认时记 command/allow —— 本会话里会话前缀记忆先于规则命中,所以来源是
// session(reason=session_allow),规则只对以后的新会话生效;沙盒外(full-access)执行。
TEST(AgentLoopAutoMode, AuditsRememberedRuleAndRuleAllow) {
    AutoHarness h;
    h.loop->set_exec_rules_dir_for_tests(path_to_utf8(h.tree.root / "rules"));
    h.answer = PermissionResult::AllowRemember;
    ASSERT_TRUE(h.run({{"command", "pnpm install"}, {"sandbox_permissions", "require_escalated"},
                      {"justification", "Install into the shared store."}}));
    ASSERT_EQ(h.audits.size(), 2u);
    EXPECT_EQ(h.audits[0].category, "command");
    EXPECT_EQ(h.audits[0].decision, "allow_remember");
    EXPECT_EQ(h.audits[0].source, "user");
    EXPECT_EQ(h.audits[0].detail["escalation_requested"], true);
    EXPECT_EQ(h.audits[1].category, "rule");
    EXPECT_EQ(h.audits[1].decision, "allow_remember");
    EXPECT_EQ(h.audits[1].reason, "remember_rule");
    EXPECT_EQ(h.audits[1].target, "pnpm install");
    ASSERT_TRUE(h.run({{"command", "pnpm install lodash"}}));
    ASSERT_EQ(h.audits.size(), 3u);
    EXPECT_EQ(h.audits[2].decision, "allow");
    EXPECT_EQ(h.audits[2].source, "session");
    EXPECT_EQ(h.audits[2].reason, "session_allow");
    EXPECT_EQ(h.audits[2].sandbox, "full-access");
}
