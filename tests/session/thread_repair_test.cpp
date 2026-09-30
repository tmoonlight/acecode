#include <gtest/gtest.h>

#include "llm/message_predicates.hpp"
#include "llm/token_estimate.hpp"
#include "session/compact_checkpoint.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "session/thread_repair.hpp"

#include <filesystem>
#include <random>
#include <string>
#include <vector>

namespace {

acecode::ChatMessage message(std::string role, std::string content) {
    acecode::ChatMessage item;
    item.role = std::move(role);
    item.content = std::move(content);
    return item;
}

nlohmann::json tool_call(const std::string& id) {
    return nlohmann::json{
        {"id", id},
        {"type", "function"},
        {"function", {
            {"name", "file_write"},
            {"arguments", R"({"path":"a.txt"})"},
        }},
    };
}

std::filesystem::path unique_cwd(const std::string& label) {
    auto cwd = std::filesystem::temp_directory_path() /
        ("acecode_thread_repair_" + label + "_" +
         std::to_string(std::random_device{}()));
    std::filesystem::create_directories(cwd);
    return cwd;
}

bool contains(const std::vector<acecode::ChatMessage>& messages,
              const std::string& text) {
    for (const auto& item : messages) {
        if (item.content.find(text) != std::string::npos) return true;
    }
    return false;
}

} // namespace

TEST(ThreadRepair, PrunesWholeOldTurnAndPreservesCurrentInput) {
    const std::vector<acecode::ChatMessage> history{
        message("user", "old request"),
        message("assistant", "old answer"),
        message("user", "middle request"),
        message("assistant", "middle answer"),
        message("user", "current request"),
    };
    acecode::ThreadRepairOptions options;
    options.force_prune_one_group = true;
    options.target_tokens = 100000;

    const auto result = acecode::plan_thread_repair(history, options);

    ASSERT_EQ(result.status, acecode::ThreadRepairStatus::Repaired);
    EXPECT_EQ(result.pruned_groups, 1);
    EXPECT_EQ(result.pruned_messages, 2);
    EXPECT_FALSE(contains(result.replacement_history, "old request"));
    EXPECT_TRUE(contains(result.replacement_history, "middle request"));
    EXPECT_TRUE(contains(result.replacement_history, "current request"));
}

TEST(ThreadRepair, RecoversMissingToolResultWithoutReplayingTool) {
    auto assistant = message("assistant", "working");
    assistant.tool_calls = nlohmann::json::array({tool_call("call-1")});
    const std::vector<acecode::ChatMessage> history{
        message("user", "write it"),
        assistant,
        message("user", "continue"),
    };

    const auto result = acecode::plan_thread_repair(history, {});

    ASSERT_EQ(result.status, acecode::ThreadRepairStatus::Repaired);
    EXPECT_EQ(result.history_issues.synthesized_tool_results, 1u);
    ASSERT_EQ(result.replacement_history.size(), 4u);
    EXPECT_EQ(result.replacement_history[2].role, "tool");
    EXPECT_EQ(result.replacement_history[2].tool_call_id, "call-1");
    EXPECT_NE(result.replacement_history[2].content.find("outcome is unknown"),
              std::string::npos);
}

TEST(ThreadRepair, ReportsExhaustedInsteadOfTruncatingOnlyCurrentTurn) {
    acecode::ThreadRepairOptions options;
    options.force_prune_one_group = true;
    options.target_tokens = 1;

    const auto result = acecode::plan_thread_repair(
        {message("user", "current input must remain")}, options);

    EXPECT_EQ(result.status, acecode::ThreadRepairStatus::HistoryExhausted);
    EXPECT_TRUE(result.checkpoint.id.empty());
    ASSERT_EQ(result.replacement_history.size(), 1u);
    EXPECT_EQ(result.replacement_history[0].content,
              "current input must remain");
}

namespace {

acecode::ChatMessage assistant_call(const std::string& id,
                                    std::string arguments = R"({"path":"a.txt"})") {
    acecode::ChatMessage item = message("assistant", "");
    item.tool_calls = nlohmann::json::array({nlohmann::json{
        {"id", id},
        {"type", "function"},
        {"function", {
            {"name", "file_write"},
            {"arguments", std::move(arguments)},
        }},
    }});
    return item;
}

acecode::ChatMessage tool_result(const std::string& id, std::string content) {
    acecode::ChatMessage item = message("tool", std::move(content));
    item.tool_call_id = id;
    return item;
}

// 只有当前回合、三次工具往返、每个输出 4000 字节的历史。
std::vector<acecode::ChatMessage> single_turn_with_tool_outputs() {
    return {
        message("user", "current request"),
        assistant_call("c1"),
        tool_result("c1", std::string(4000, 'A')),
        assistant_call("c2"),
        tool_result("c2", std::string(4000, 'B')),
        assistant_call("c3"),
        tool_result("c3", std::string(4000, 'C')),
    };
}

} // namespace

// 触发场景:只剩当前回合(没有整组可丢),目标要求腾空间,调用方允许清工具
// 输出(PA 兜底 / 摘要失败后的机械兜底)。
// 期望行为:从最旧的工具输出开始换成占位符,最近一条保留给模型继续干活;
// 调用与结果的配对不变,用户输入不动;状态 Repaired 并写 checkpoint。
// 回归背景:旧实现在这种历史上直接报 HistoryExhausted,回合里读了一堆大文件
// 之后一旦撞墙就只剩紧急档一条路,截图里那次致命 400 就是这么来的。
TEST(ThreadRepair, ClearsOldestToolOutputsWhenOnlyCurrentTurnRemains) {
    acecode::ThreadRepairOptions options;
    options.force_prune_one_group = true;
    options.target_tokens = 1;
    options.clear_tool_outputs = true;
    options.keep_recent_tool_outputs = 1;

    const auto result = acecode::plan_thread_repair(
        single_turn_with_tool_outputs(), options);

    EXPECT_EQ(result.status, acecode::ThreadRepairStatus::Repaired);
    EXPECT_EQ(result.pruned_groups, 0);
    EXPECT_EQ(result.cleared_tool_outputs, 2);
    EXPECT_EQ(result.reason, "old tool outputs were cleared");
    EXPECT_FALSE(result.checkpoint.id.empty());
    EXPECT_LT(result.post_tokens, result.pre_tokens);
    ASSERT_EQ(result.replacement_history.size(), 7u);
    EXPECT_EQ(result.replacement_history[0].content, "current request");
    EXPECT_EQ(result.replacement_history[2].content,
              acecode::kClearedToolOutputPlaceholder);
    EXPECT_EQ(result.replacement_history[2].tool_call_id, "c1");
    EXPECT_EQ(result.replacement_history[4].content,
              acecode::kClearedToolOutputPlaceholder);
    EXPECT_EQ(result.replacement_history[6].content, std::string(4000, 'C'))
        << "最近一条工具输出必须保留";
}

// 触发场景:同样的历史,但调用方没有打开 clear_tool_outputs(默认)。
// 期望行为:与旧行为完全一致 —— HistoryExhausted、历史原样、不写 checkpoint。
TEST(ThreadRepair, DoesNotClearToolOutputsUnlessAllowed) {
    acecode::ThreadRepairOptions options;
    options.force_prune_one_group = true;
    options.target_tokens = 1;

    const auto result = acecode::plan_thread_repair(
        single_turn_with_tool_outputs(), options);

    EXPECT_EQ(result.status, acecode::ThreadRepairStatus::HistoryExhausted);
    EXPECT_EQ(result.cleared_tool_outputs, 0);
    EXPECT_TRUE(result.checkpoint.id.empty());
    ASSERT_EQ(result.replacement_history.size(), 7u);
    EXPECT_EQ(result.replacement_history[2].content, std::string(4000, 'A'));
}

// 触发场景:工具输出清完仍超目标,历史里有一条 file_write 调用带着整篇文件
// 内容(参数超过 4KB),还有一条小参数调用。
// 期望行为:大参数换成占位 JSON,小参数原样保留,最近一条调用不动。
TEST(ThreadRepair, ClearsOversizedToolCallArgumentsAfterOutputs) {
    const std::string big_arguments =
        R"({"path":"big.txt","content":")" + std::string(6000, 'W') + R"("})";
    std::vector<acecode::ChatMessage> history{
        message("user", "current request"),
        assistant_call("c1", big_arguments),
        tool_result("c1", std::string(4000, 'x')),
        assistant_call("c2"),
        tool_result("c2", std::string(4000, 'y')),
        assistant_call("c3", big_arguments),
        tool_result("c3", std::string(4000, 'z')),
    };
    acecode::ThreadRepairOptions options;
    options.force_prune_one_group = true;
    options.target_tokens = 1;
    options.clear_tool_outputs = true;
    options.keep_recent_tool_outputs = 1;

    const auto result = acecode::plan_thread_repair(history, options);

    EXPECT_EQ(result.status, acecode::ThreadRepairStatus::Repaired);
    // 两条工具输出(c1、c2)+ 一条大参数调用(c1);c3 是最近一条调用,保留。
    EXPECT_EQ(result.cleared_tool_outputs, 3);
    ASSERT_EQ(result.replacement_history.size(), 7u);
    EXPECT_EQ(result.replacement_history[1].tool_calls[0]["function"]["arguments"],
              acecode::kClearedToolArgumentsJson);
    EXPECT_EQ(result.replacement_history[3].tool_calls[0]["function"]["arguments"],
              R"({"path":"a.txt"})")
        << "小参数不值得清";
    EXPECT_EQ(result.replacement_history[5].tool_calls[0]["function"]["arguments"],
              big_arguments)
        << "最近一条调用必须保留";
}

namespace {

acecode::ThreadRepairOptions shrink_options(int target_tokens) {
    // 与 PA 兜底 / 摘要失败后的机械兜底同一组开关。
    acecode::ThreadRepairOptions options;
    options.trigger = "repair-test";
    options.target_tokens = target_tokens;
    options.force_prune_one_group = true;
    options.clear_tool_outputs = true;
    options.keep_recent_tool_outputs = 1;
    options.thin_old_turns_first = true;
    return options;
}

acecode::ChatMessage compact_summary(std::string content) {
    acecode::ChatMessage item = message("user", std::move(content));
    item.is_compact_summary = true;
    item.metadata = nlohmann::json{{"compact_summary", true}};
    return item;
}

} // namespace

// 触发场景:一个老回合(用户的任务说明 + 一次大工具输出 + 结论)加当前回合,
// 目标只比现规模低约 500 token —— 清掉那条旧工具输出就够。
// 期望行为:只清旧工具输出,老回合的任务说明与结论、当前回合的输出都保留;
// 一组都不丢、也不精简。
// 回归背景(反馈 LINDANDAN069):旧顺序先整组丢老回合、后清工具输出,PA 服务端
// 随机拒收时一次把最初的任务说明、用户的多次纠正连同压缩摘要全部删掉,而真正
// 省出空间的是随后清掉的工具输出 —— 模型此后「忘了」任务要求与刚学到的做法。
TEST(ThreadRepair, ClearsToolOutputsBeforeDroppingOldTurns) {
    const std::vector<acecode::ChatMessage> history{
        message("user", "original task: always verify every expect"),
        assistant_call("old-call"),
        tool_result("old-call", std::string(4000, 'O')),
        message("assistant", "old conclusion: compose window found via tabs"),
        message("user", "current request"),
        assistant_call("new-call"),
        tool_result("new-call", std::string(400, 'N')),
    };
    const int pre = acecode::estimate_message_tokens(history);

    const auto result = acecode::plan_thread_repair(
        history, shrink_options(pre - 500));

    ASSERT_EQ(result.status, acecode::ThreadRepairStatus::Repaired);
    EXPECT_EQ(result.pruned_groups, 0);
    EXPECT_EQ(result.thinned_groups, 0);
    EXPECT_EQ(result.cleared_tool_outputs, 1);
    EXPECT_EQ(result.reason, "old tool outputs were cleared");
    EXPECT_TRUE(contains(result.replacement_history, "original task"))
        << "任务说明是老回合里最不该丢的部分";
    EXPECT_TRUE(contains(result.replacement_history, "old conclusion"));
    EXPECT_FALSE(contains(result.replacement_history, std::string(4000, 'O')));
    EXPECT_TRUE(contains(result.replacement_history, std::string(400, 'N')))
        << "最近一条工具输出必须保留";
}

// 触发场景:老回合里有 12 次工具往返,每次调用参数约 3KB(不到 4KB,清不掉)、
// 返回只有 "ok"(比占位符还短,也清不掉);目标要求腾出这些往返的大头。
// 期望行为:清工具输出腾不出空间,就把老回合精简成「任务说明 + 结论」:
// 工具调用与结果全部删掉,用户消息与助手的纯文本结论保留,整组不丢。
// 回归背景:旧实现只能整组丢弃,模型连任务说明带结论一起丢;只留用户消息不留
// 结论又会让模型以为老任务还没做、从头再做一遍,所以结论必须一起保留。
TEST(ThreadRepair, ThinsOldTurnsBeforeDroppingThem) {
    std::vector<acecode::ChatMessage> history{
        message("user", "original task: reuse netdisk_helpers.py"),
    };
    for (int i = 0; i < 12; ++i) {
        const std::string id = "old-" + std::to_string(i);
        history.push_back(assistant_call(
            id, R"({"path":"x.txt","content":")" + std::string(3000, 'P') +
                    R"("})"));
        history.push_back(tool_result(id, "ok"));
    }
    history.push_back(message("assistant", "old conclusion: all cases rerun"));
    history.push_back(message("user", "current request"));
    const int pre = acecode::estimate_message_tokens(history);

    const auto result = acecode::plan_thread_repair(
        history, shrink_options(pre / 2));

    ASSERT_EQ(result.status, acecode::ThreadRepairStatus::Repaired);
    EXPECT_EQ(result.cleared_tool_outputs, 0);
    EXPECT_EQ(result.thinned_groups, 1);
    EXPECT_EQ(result.pruned_groups, 0);
    EXPECT_EQ(result.pruned_messages, 24);
    ASSERT_EQ(result.replacement_history.size(), 3u);
    EXPECT_EQ(result.replacement_history[0].content,
              "original task: reuse netdisk_helpers.py");
    EXPECT_EQ(result.replacement_history[1].content,
              "old conclusion: all cases rerun");
    EXPECT_EQ(result.replacement_history[2].content, "current request");
    EXPECT_LE(result.post_tokens, pre / 2);
}

// 触发场景:历史开头是上一次压缩留下的「保留用户消息 + 摘要」,后面接着两个
// 纯文本老回合和当前输入;目标极小(1 token),精简也腾不出空间,只能整组丢。
// 期望行为:老回合整组丢掉,但最新那份压缩摘要保留在最前面,当前输入不动。
// 回归背景:旧实现整组丢弃时摘要跟着所在的组一起消失 —— 那份摘要是此前所有
// 回合的唯一记忆,丢了它模型对之前做过的事一无所知。
TEST(ThreadRepair, KeepsLatestCompactSummaryWhenDroppingOldTurns) {
    const std::vector<acecode::ChatMessage> history{
        message("user", "retained request from before compaction"),
        compact_summary("handoff summary: helpers live in reports/"),
        message("assistant", "reply after compaction"),
        message("user", "second request"),
        message("assistant", "second reply"),
        message("user", "current request"),
    };

    const auto result = acecode::plan_thread_repair(history, shrink_options(1));

    ASSERT_EQ(result.status, acecode::ThreadRepairStatus::Repaired);
    EXPECT_EQ(result.pruned_groups, 2);
    ASSERT_EQ(result.replacement_history.size(), 2u);
    EXPECT_TRUE(acecode::is_compact_summary_message(
        result.replacement_history[0]));
    EXPECT_NE(result.replacement_history[0].content.find("helpers live in"),
              std::string::npos);
    EXPECT_EQ(result.replacement_history[1].content, "current request");
}

// 触发场景:同样的历史,但调用方没打开 thin_old_turns_first(手动修复、通用
// 恢复链的默认值)。
// 期望行为:与旧行为一致 —— 整组丢弃,摘要不额外保留。
TEST(ThreadRepair, DefaultOptionsKeepWholeGroupPruning) {
    const std::vector<acecode::ChatMessage> history{
        message("user", "retained request from before compaction"),
        compact_summary("handoff summary: helpers live in reports/"),
        message("assistant", "reply after compaction"),
        message("user", "current request"),
    };
    acecode::ThreadRepairOptions options;
    options.force_prune_one_group = true;

    const auto result = acecode::plan_thread_repair(history, options);

    ASSERT_EQ(result.status, acecode::ThreadRepairStatus::Repaired);
    EXPECT_EQ(result.pruned_groups, 1);
    EXPECT_EQ(result.thinned_groups, 0);
    ASSERT_EQ(result.replacement_history.size(), 1u);
    EXPECT_EQ(result.replacement_history[0].content, "current request");
}

// 触发场景:当前回合里先有一次大输出(4000 字节),最近一次工具只返回了 "ok"
// (比占位符还短,本身不值得清);目标要求腾空间,保留最近 1 条。
// 期望行为:最近那条 "ok" 占掉保护名额,更早的大输出被清掉。
// 回归背景:旧实现只在「值得清的输出」里数最近 N 条,短输出不算,保护名额
// 落到了更早的大输出上 —— 它恰恰是唯一能腾出空间的那条,于是一条都清不掉,
// 修复转而去精简 / 整组丢弃老回合(端到端用例里就是这么绕过去的)。
TEST(ThreadRepair, ShortLatestToolOutputStillTakesTheProtectedSlot) {
    const std::vector<acecode::ChatMessage> history{
        message("user", "current request"),
        assistant_call("c1"),
        tool_result("c1", std::string(4000, 'A')),
        assistant_call("c2"),
        tool_result("c2", "ok"),
    };
    acecode::ThreadRepairOptions options;
    options.force_prune_one_group = true;
    options.target_tokens = 1;
    options.clear_tool_outputs = true;
    options.keep_recent_tool_outputs = 1;

    const auto result = acecode::plan_thread_repair(history, options);

    EXPECT_EQ(result.status, acecode::ThreadRepairStatus::Repaired);
    EXPECT_EQ(result.cleared_tool_outputs, 1);
    ASSERT_EQ(result.replacement_history.size(), 5u);
    EXPECT_EQ(result.replacement_history[2].content,
              acecode::kClearedToolOutputPlaceholder);
    EXPECT_EQ(result.replacement_history[4].content, "ok");
}

TEST(ThreadRepair, HealthyHistoryReportsNoChangeWithoutWritingCheckpoint) {
    const auto result = acecode::plan_thread_repair(
        {message("user", "request"), message("assistant", "answer")}, {});

    EXPECT_EQ(result.status, acecode::ThreadRepairStatus::NoChange);
    EXPECT_TRUE(result.checkpoint.id.empty());
    EXPECT_EQ(result.reason, "provider history is already consistent");
}

TEST(ThreadRepair, ApplyAppendsCheckpointWithoutRewritingTranscript) {
    const auto cwd = unique_cwd("append");
    const std::string cwd_string = cwd.string();
    const std::string project_dir =
        acecode::SessionStorage::get_project_dir(cwd_string);
    std::filesystem::remove_all(project_dir);

    {
        acecode::SessionManager manager;
        manager.start_session(cwd_string, "stub", "model");
        std::vector<acecode::ChatMessage> history{
            message("user", "old request"),
            message("assistant", "old answer"),
            message("user", "current request"),
        };
        for (const auto& item : history) manager.on_message(item);
        const std::string id = manager.current_session_id();

        acecode::ThreadRepairOptions options;
        options.trigger = "repair-test";
        options.force_prune_one_group = true;
        const auto result = acecode::apply_thread_repair(
            &manager, history, options);
        ASSERT_TRUE(result.repaired());

        const auto raw = acecode::SessionStorage::load_messages(
            acecode::SessionStorage::session_path(project_dir, id));
        ASSERT_EQ(raw.size(), 4u);
        EXPECT_EQ(raw[0].content, "old request");
        EXPECT_EQ(raw[1].content, "old answer");
        EXPECT_EQ(raw[2].content, "current request");
        EXPECT_TRUE(acecode::is_compact_checkpoint_message(raw[3]));
        const auto effective =
            acecode::reconstruct_effective_model_history(raw);
        ASSERT_EQ(effective.size(), 1u);
        EXPECT_EQ(effective[0].content, "current request");
        manager.finalize();
    }

    std::filesystem::remove_all(project_dir);
    std::filesystem::remove_all(cwd);
}
