#include "test_support/agent_loop/characterization_fixture.hpp"
#include "tool/apply_patch_tool.hpp"

namespace {
using namespace acecode;
using namespace acecode_test::characterization;

void install_patch(Harness& h) {
    h.provider->set_model("gpt-5");
    auto tool = create_apply_patch_tool();
    tool.execute = h.probe("apply_patch", false).execute;
    h.tools.register_tool(std::move(tool));
    h.install_hooks({"PermissionRequest", "PermissionResolved"});
}

std::string patch_arguments(const std::string& body) {
    return Json{{"patch", "*** Begin Patch\n" + body + "*** End Patch\n"}}.dump();
}

class AgentLoopPatchGolden : public testing::TestWithParam<std::string> {};

// 场景:安全路径之后的 Add/Update/Delete/Move 目标落进 exec rules。
// 期望:整份补丁硬拒绝,不执行、不确认,只记一次审计;漏扫后续路径会改坏用户规则。
TEST_P(AgentLoopPatchGolden, EveryTargetProtectsExecRulesBeforeAnyExecution) {
    Isolation isolation;
    Harness h(isolation);
    install_patch(h);
    h.permissions.set_mode(PermissionMode::Yolo);
    h.permissions.set_dangerous(true);
    const std::string protected_path = ".acecode/rules/project.rules";
    std::string body = "*** Add File: ordinary.txt\n+safe\n";
    if (GetParam() == "Add") body += "*** Add File: " + protected_path + "\n+rule\n";
    if (GetParam() == "Update") body += "*** Update File: " + protected_path + "\n@@\n-old\n+rule\n";
    if (GetParam() == "Delete") body += "*** Delete File: " + protected_path + "\n";
    if (GetParam() == "Move") body += "*** Update File: source.txt\n*** Move to: " + protected_path + "\n@@\n-old\n+rule\n";
    ASSERT_TRUE(h.run_calls({{"patch-call", "apply_patch", patch_arguments(body)}}));
    std::lock_guard<std::mutex> lock(h.observed->mutex);
    EXPECT_TRUE(h.observed->executions.empty());
    EXPECT_EQ(h.observed->confirmations, 0);
    EXPECT_TRUE(h.observed->hooks.empty());
    ASSERT_EQ(h.observed->results.size(), 1u);
    EXPECT_FALSE(h.observed->results[0].success);
    EXPECT_EQ(h.observed->results[0].output,
        "[Permission denied] Exec rules must be edited by the user.");
    ASSERT_EQ(h.observed->audits.size(), 1u);
    const auto& audit = h.observed->audits[0];
    EXPECT_EQ(audit.reason, "exec_rules_protected");
    EXPECT_EQ(audit.decision, "forbidden");
    EXPECT_EQ(audit.source, "rule");
    EXPECT_EQ(audit.target, acecode::path_to_utf8(h.cwd / "ordinary.txt"));
    ASSERT_TRUE(audit.detail["paths"].is_array());
    EXPECT_EQ(audit.detail["paths"].back(), acecode::path_to_utf8((h.cwd / protected_path).lexically_normal()));
}

INSTANTIATE_TEST_SUITE_P(P0_11, AgentLoopPatchGolden,
    testing::Values("Add", "Update", "Delete", "Move"),
    [](const testing::TestParamInfo<std::string>& info) { return info.param; });

// 场景:补丁含三个安全路径。期望:一个整体审批、一个执行和一条文件审计;
// 逐文件弹窗或审批前执行部分补丁都会破坏原有原子授权语义。
TEST(AgentLoopPatchGolden, MultipleSafePathsAskExactlyOnce) {
    Isolation isolation;
    Harness h(isolation);
    install_patch(h);
    ASSERT_TRUE(h.run_calls({{"patch-call", "apply_patch", patch_arguments(
        "*** Add File: a.txt\n+a\n*** Add File: b.txt\n+b\n*** Add File: c.txt\n+c\n")}}));
    std::lock_guard<std::mutex> lock(h.observed->mutex);
    EXPECT_EQ(h.observed->executions.size(), 1u);
    EXPECT_EQ(h.observed->confirmations, 1);
    EXPECT_EQ(h.observed->hooks.size(), 2u);
    ASSERT_EQ(h.observed->audits.size(), 1u);
    EXPECT_EQ(h.observed->audits[0].detail["paths"], Json::array({
        acecode::path_to_utf8(h.cwd / "a.txt"), acecode::path_to_utf8(h.cwd / "b.txt"),
        acecode::path_to_utf8(h.cwd / "c.txt")}));
    EXPECT_EQ(h.observed->decisions, (std::vector<std::string>{
        "PermissionRequest", "confirm", "PermissionResolved:allow:interactive", "audit:allow:user", "execute"}));
}

// 场景:Plan 模式补丁只触及活动计划文件。期望:免确认并标 plan_file;
// 回归会让计划编辑也弹权限框,或丢掉专用授权来源。
TEST(AgentLoopPatchGolden, PlanOnlyPatchHasAutomaticPlanFileAudit) {
    Isolation isolation;
    Harness h(isolation);
    install_patch(h);
    h.permissions.set_mode(PermissionMode::Plan);
    h.observed->answer = PermissionResult::Deny;
    const auto plan = h.session->ensure_plan_file_path();
    ASSERT_FALSE(plan.empty());
    ASSERT_TRUE(h.run_calls({{"patch-call", "apply_patch", patch_arguments(
        "*** Add File: " + plan + "\n+# plan\n")}}));
    std::lock_guard<std::mutex> lock(h.observed->mutex);
    EXPECT_EQ(h.observed->executions.size(), 1u);
    EXPECT_EQ(h.observed->confirmations, 0);
    EXPECT_TRUE(h.observed->hooks.empty());
    ASSERT_EQ(h.observed->audits.size(), 1u);
    EXPECT_EQ(h.observed->audits[0].reason, "plan_file");
    EXPECT_EQ(h.observed->audits[0].source, "auto");
}

// 场景:计划文件和普通文件混在同一补丁。期望:必须整体审批,拒绝后一个都不执行;
// 只检查首条路径会误把普通写入当作计划文件放行。
TEST(AgentLoopPatchGolden, MixedPlanAndOrdinaryPathsRequireWholePatchConfirmation) {
    Isolation isolation;
    Harness h(isolation);
    install_patch(h);
    h.permissions.set_mode(PermissionMode::Plan);
    h.observed->answer = PermissionResult::Deny;
    const auto plan = h.session->ensure_plan_file_path();
    ASSERT_TRUE(h.run_calls({{"patch-call", "apply_patch", patch_arguments(
        "*** Add File: " + plan + "\n+# plan\n*** Add File: ordinary.txt\n+side effect\n")}}));
    std::lock_guard<std::mutex> lock(h.observed->mutex);
    EXPECT_TRUE(h.observed->executions.empty());
    EXPECT_EQ(h.observed->confirmations, 1);
    EXPECT_EQ(h.observed->hooks.size(), 2u);
    ASSERT_EQ(h.observed->results.size(), 1u);
    EXPECT_EQ(h.observed->results[0].output, "[User denied tool execution]");
    ASSERT_EQ(h.observed->audits.size(), 1u);
    EXPECT_EQ(h.observed->audits[0].reason, "confirmation");
    EXPECT_EQ(h.observed->audits[0].decision, "deny");
    EXPECT_EQ(h.observed->audits[0].detail["paths"].size(), 2u);
}
} // namespace
