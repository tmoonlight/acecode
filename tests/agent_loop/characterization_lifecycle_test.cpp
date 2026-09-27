#include "test_support/agent_loop/characterization_fixture.hpp"
#include "tool/bash_tool.hpp"

#include <algorithm>
#include <thread>

namespace {
using namespace acecode;
using namespace acecode_test::characterization;

// 场景:PreToolUse 在工具开始前拒绝。期望:实时 start/end 均为零,轨迹恰好各一条;
// 回归会让界面出现从未执行的工具进度,或让回放看不到被拒绝的调用。
TEST(AgentLoopLifecycleGolden, PreToolDenialHasOnlyOneTrajectoryPair) {
    Isolation isolation;
    Harness h(isolation);
    h.tools.register_tool(h.probe("write_probe", false));
    h.install_hooks({"PreToolUse", "PostToolUse", "PermissionRequest", "PermissionResolved"},
        [](const Json& payload) {
            if (payload["hook_event_name"] == "PreToolUse") {
                return Json{{"hookSpecificOutput", {{"permissionDecision", "deny"},
                    {"permissionDecisionReason", "preflight blocked"}}}};
            }
            return Json::object();
        });
    ASSERT_TRUE(h.run_calls({{"pre-denied", "write_probe", "{}"}}));
    const auto starts = h.trajectory("tool_start");
    const auto ends = h.trajectory("tool_end");
    ASSERT_EQ(starts.size(), 1u);
    ASSERT_EQ(ends.size(), 1u);
    EXPECT_LT(starts[0].sequence, ends[0].sequence);
    EXPECT_EQ(starts[0].payload["tool_call_id"], "pre-denied");
    EXPECT_EQ(ends[0].payload["tool_call_id"], "pre-denied");
    EXPECT_EQ(ends[0].payload["failure_stage"], "pre_tool_hook");
    EXPECT_EQ(ends[0].payload["duration_ms"], 0);
    EXPECT_EQ(ends[0].payload["output"], "[Hook denied tool execution] preflight blocked");
    EXPECT_EQ(starts[0].payload["started_at_ms"], ends[0].payload["completed_at_ms"]);
    std::lock_guard<std::mutex> lock(h.observed->mutex);
    EXPECT_TRUE(events_of(*h.observed, SessionEventKind::ToolStart).empty());
    EXPECT_TRUE(events_of(*h.observed, SessionEventKind::ToolEnd).empty());
    EXPECT_TRUE(h.observed->executions.empty());
    EXPECT_TRUE(h.observed->audits.empty());
    EXPECT_EQ(h.observed->confirmations, 0);
    ASSERT_EQ(h.observed->hooks.size(), 1u);
    EXPECT_EQ(h.observed->hooks[0]["hook_event_name"], "PreToolUse");
}

struct ParallelBatch {
    std::mutex mutex;
    std::condition_variable changed;
    bool second_posted = false;
    bool overlap = false;
    bool timed_out = false;
    std::vector<std::string> trace;
};

// 场景:第二个只读工具先结束,每个调用都经 Pre/Post hooks。
// 期望:实际并行但调用/结果展示与模型历史保持提交顺序,每个 hook 配对且输入改写生效;
// 回归会把并行完成顺序误当历史顺序,或绕过生命周期 hook。
TEST(AgentLoopLifecycleGolden, ParallelReadHooksKeepSubmissionOrderAndPerCallPairs) {
    Isolation isolation;
    Harness h(isolation);
    auto batch = std::make_shared<ParallelBatch>(); // 工具线程与 hook 共同持有并发门和轨迹。
    const bool parallel = std::thread::hardware_concurrency() > 1;
    auto tool = h.probe("read_probe", true);
    tool.execute = [batch, parallel](const std::string& arguments, const ToolContext&) {
        const auto input = Json::parse(arguments);
        const int ordinal = input["ordinal"];
        std::unique_lock<std::mutex> lock(batch->mutex);
        batch->trace.push_back("execute:" + std::to_string(ordinal));
        EXPECT_EQ(input["rewritten"], true);
        if (ordinal == 1 && parallel) {
            if (!batch->changed.wait_for(lock, 2s, [batch] { return batch->second_posted; })) {
                batch->timed_out = true;
            } else {
                batch->overlap = true;
            }
        }
        return ToolResult{"output-" + std::to_string(ordinal), true};
    };
    h.tools.register_tool(std::move(tool));
    h.install_hooks({"PreToolUse", "PostToolUse"}, [batch](const Json& payload) {
        const int ordinal = payload["tool_input"]["ordinal"];
        const bool before = payload["hook_event_name"] == "PreToolUse";
        {
            std::lock_guard<std::mutex> lock(batch->mutex);
            batch->trace.push_back(std::string(before ? "pre:" : "post:") + std::to_string(ordinal));
            if (!before && ordinal == 2) {
                batch->second_posted = true;
                batch->changed.notify_all();
            }
        }
        if (before) return Json{{"hookSpecificOutput", {{"updatedInput", {{"ordinal", ordinal}, {"rewritten", true}}}}}};
        return Json::object();
    });
    ASSERT_TRUE(h.run_calls({{"read-1", "read_probe", R"({"ordinal":1})"},
                            {"read-2", "read_probe", R"({"ordinal":2})"}}));
    {
        std::lock_guard<std::mutex> lock(batch->mutex);
        EXPECT_FALSE(batch->timed_out);
        EXPECT_EQ(batch->overlap, parallel);
        for (int ordinal : {1, 2}) {
            std::vector<std::string> per_call;
            for (const auto& item : batch->trace) if (item.back() == '0' + ordinal) per_call.push_back(item);
            EXPECT_EQ(per_call, (std::vector<std::string>{"pre:" + std::to_string(ordinal),
                "execute:" + std::to_string(ordinal), "post:" + std::to_string(ordinal)}));
        }
    }
    std::vector<std::string> history_ids;
    for (const auto& message : h.provider->messages_for_turn(1)) {
        if (message.role == "tool") history_ids.push_back(message.tool_call_id);
    }
    EXPECT_EQ(history_ids, (std::vector<std::string>{"read-1", "read-2"}));
    std::lock_guard<std::mutex> lock(h.observed->mutex);
    std::vector<std::string> display_roles;
    std::vector<std::string> display_results;
    for (const auto& display : h.observed->displays) {
        if (display["role"] == "tool_call" || display["role"] == "tool_result") display_roles.push_back(display["role"]);
        if (display["role"] == "tool_result") display_results.push_back(display["content"]);
    }
    EXPECT_EQ(display_roles, (std::vector<std::string>{"tool_call", "tool_result", "tool_call", "tool_result"}));
    EXPECT_EQ(display_results, (std::vector<std::string>{"output-1", "output-2"}));
    EXPECT_EQ(events_of(*h.observed, SessionEventKind::ToolStart).size(), 2u);
    EXPECT_EQ(events_of(*h.observed, SessionEventKind::ToolEnd).size(), 2u);
    EXPECT_EQ(h.observed->hooks.size(), 4u);
    EXPECT_EQ(h.observed->confirmations, 0);
    EXPECT_TRUE(h.observed->audits.empty());
}

// 场景:用户 !cmd 被 PreToolUse 改写并返回超长输出。期望:实时与 JSONL 均保留全文,
// shell 只发布 BusyChanged(false),落盘是 !改写后的命令 + tool_result;回归会折叠或截断输出。
TEST(AgentLoopLifecycleGolden, UserShellKeepsRewrittenCommandFullOutputAndDistinctBusyContract) {
    Isolation isolation;
    Harness h(isolation);
    std::string output;
    for (int line = 0; line < 2000; ++line) output += "shell-line-" + std::to_string(line) + " content stays complete\n";
    auto shell = create_bash_tool();
    shell.execute = h.probe("bash", false, output).execute;
    h.tools.register_tool(std::move(shell));
    h.install_hooks({"PreToolUse", "PostToolUse"}, [](const Json& payload) {
        if (payload["hook_event_name"] == "PreToolUse") {
            return Json{{"hookSpecificOutput", {{"updatedInput", {{"command", "rewritten command"}}}}}};
        }
        return Json::object();
    });
    ASSERT_TRUE(h.perform([loop = h.loop.get()] { loop->submit_shell("original command"); }));
    const auto path = SessionStorage::session_path(SessionStorage::get_project_dir(path_to_utf8(h.cwd)),
        h.session->current_session_id());
    const auto persisted = SessionStorage::load_messages(path);
    ASSERT_EQ(persisted.size(), 2u);
    EXPECT_EQ(persisted[0].role, "user");
    EXPECT_EQ(persisted[0].content, "!rewritten command");
    EXPECT_EQ(persisted[1].role, "tool_result");
    EXPECT_EQ(persisted[1].content, output);
    EXPECT_FALSE(persisted[1].metadata.contains("tool_summary"));
    ASSERT_EQ(h.loop->messages().size(), 1u);
    EXPECT_NE(h.loop->messages()[0].content.find("rewritten command"), std::string::npos);
    EXPECT_NE(h.loop->messages()[0].content.find(output), std::string::npos);
    std::lock_guard<std::mutex> lock(h.observed->mutex);
    const auto busy = events_of(*h.observed, SessionEventKind::BusyChanged);
    ASSERT_EQ(busy.size(), 1u);
    EXPECT_EQ(busy[0].payload["busy"], false);
    EXPECT_EQ(h.observed->busy_callbacks, (std::vector<bool>{true, false}));
    EXPECT_EQ(h.observed->turns_finished, 0);
    EXPECT_TRUE(h.observed->results.empty());
    ASSERT_EQ(h.observed->displays.size(), 2u);
    EXPECT_EQ(h.observed->displays[1], (Json{{"role", "user_shell_output"}, {"content", output}}));
    ASSERT_EQ(h.observed->executions.size(), 1u);
    EXPECT_EQ(Json::parse(h.observed->executions[0]["arguments"].get<std::string>())["command"], "rewritten command");
    ASSERT_EQ(h.observed->hooks.size(), 2u);
    EXPECT_EQ(h.observed->hooks[0]["tool_input"]["command"], "original command");
    EXPECT_EQ(h.observed->hooks[1]["tool_input"]["command"], "rewritten command");
}

// 场景:Stop 要求续跑时恰好耗尽 max_iterations。期望:下一用户回合仍收到 active=true,
// 再下一回合才复位;这是现有残留行为,结构重构不得顺手修复它。
TEST(AgentLoopLifecycleGolden, StopHookActiveSurvivesCappedTurnUntilNextStopDecision) {
    Isolation isolation;
    Harness h(isolation);
    AgentLoopConfig config;
    config.max_iterations = 1;
    h.loop->set_agent_loop_config(config);
    h.install_hooks({"Stop"}, [](const Json&) { return Json{{"decision", "block"}, {"reason", "continue once"}}; });
    for (const std::string reply : {"first", "second", "third"}) {
        h.provider->push_text(reply);
        ASSERT_TRUE(h.perform([loop = h.loop.get()] { loop->submit("next visible turn"); }));
    }
    EXPECT_EQ(h.provider->turn_count(), 3);
    std::lock_guard<std::mutex> lock(h.observed->mutex);
    ASSERT_EQ(h.observed->hooks.size(), 3u);
    EXPECT_EQ(h.observed->hooks[0]["stop_hook_active"], false);
    EXPECT_EQ(h.observed->hooks[1]["stop_hook_active"], true);
    EXPECT_EQ(h.observed->hooks[2]["stop_hook_active"], false);
    EXPECT_EQ(h.observed->done, 3);
}

// 场景:swarm 用户回合后提交普通回合。期望:仅首轮有 swarm 上下文,静态前缀不变;
// 回归会让后续普通聊天继续错误地启动群体协作。
TEST(AgentLoopLifecycleGolden, SwarmModeResetsBetweenVisibleTurns) {
    Isolation isolation;
    Harness h(isolation);
    h.tools.register_tool(h.probe("spawn_subagent", true));
    h.provider->push_text("swarm finished");
    UserInput swarm;
    swarm.text = "coordinate this turn";
    swarm.metadata = {{"swarm_mode", true}};
    ASSERT_TRUE(h.perform([loop = h.loop.get(), swarm] { loop->submit(swarm); }));
    h.provider->push_text("ordinary finished");
    ASSERT_TRUE(h.perform([loop = h.loop.get()] { loop->submit("ordinary next turn"); }));
    const auto first = h.provider->messages_for_turn(0);
    const auto second = h.provider->messages_for_turn(1);
    const auto count_swarm = [](const std::vector<ChatMessage>& messages) {
        return std::count_if(messages.begin(), messages.end(), [](const ChatMessage& message) {
            return message.content.find("# Swarm Mode") != std::string::npos;
        });
    };
    EXPECT_EQ(count_swarm(first), 1);
    EXPECT_EQ(count_swarm(second), 0);
    ASSERT_FALSE(first.empty());
    ASSERT_FALSE(second.empty());
    EXPECT_EQ(first[0].role, "system");
    EXPECT_EQ(first[0].content, second[0].content);
}
} // namespace
