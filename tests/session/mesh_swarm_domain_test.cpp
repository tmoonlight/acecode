#include <gtest/gtest.h>

#include "llm/message_predicates.hpp"
#include "session/agent_fork_history.hpp"
#include "session/inter_agent_message.hpp"
#include "session/mesh_tree_index.hpp"
#include "session/swarm_mode.hpp"
#include "utils/utf8_path.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

// 蜂群模式（网状）domain 层纯逻辑(add-mesh-swarm-mode):模式名解析与工具族互斥、
// agent 间信封、fork_turns 历史裁剪、agent 树索引落盘。

namespace fs = std::filesystem;
using namespace acecode;
using namespace acecode::mesh;

namespace {

ChatMessage text_message(const std::string& role, const std::string& content) {
    ChatMessage message;
    message.role = role;
    message.content = content;
    message.metadata = nlohmann::json::object();
    return message;
}

ChatMessage envelope_message(InterAgentMessageType type, const std::string& sender,
                             const std::string& recipient, const std::string& payload,
                             const std::string& status = {}) {
    InterAgentEnvelope envelope;
    envelope.type = type;
    envelope.sender = sender;
    envelope.recipient = recipient;
    envelope.payload = payload;
    envelope.final_status = status;
    ChatMessage message = text_message("user", render_inter_agent_message(envelope));
    message.metadata[kInterAgentMetadataKey] = inter_agent_metadata(envelope);
    return message;
}

ChatMessage tool_call_message() {
    ChatMessage message = text_message("assistant", "");
    message.tool_calls = nlohmann::json::array(
        {{{"id", "c1"}, {"type", "function"},
          {"function", {{"name", "bash"}, {"arguments", "{}"}}}}});
    return message;
}

} // namespace

// 场景:Web 旧客户端仍发布尔 swarm_mode,新客户端发 "star"/"mesh"/"off"。
// 期望:true=star、false=off,大小写不敏感;空串与未知值返回 nullopt(由调用方报错)。
TEST(MeshSwarmMode, ParsesCanonicalNamesAndLegacyBooleans) {
    EXPECT_EQ(parse_swarm_mode("mesh"), SwarmMode::Mesh);
    EXPECT_EQ(parse_swarm_mode("STAR"), SwarmMode::Star);
    EXPECT_EQ(parse_swarm_mode("off"), SwarmMode::Off);
    EXPECT_EQ(parse_swarm_mode("true"), SwarmMode::Star);
    EXPECT_EQ(parse_swarm_mode("false"), SwarmMode::Off);
    EXPECT_FALSE(parse_swarm_mode("").has_value());
    EXPECT_FALSE(parse_swarm_mode("grid").has_value());
    EXPECT_STREQ(swarm_mode_name(SwarmMode::Mesh), "mesh");
    EXPECT_STREQ(swarm_mode_name(SwarmMode::Off), "off");
}

// 场景:两套协作工具必须互斥 —— 网状下还能看到 spawn_subagent / 线程工具,
// 模型就有两条互相冲突的派发路径(用户拍板:网状模式隐藏星型与线程工具)。
// 期望:off/star 隐藏全部 agent_* 工具;mesh 隐藏 spawn/wait_subagent 与线程工具,
// 且 agent_* 本身可见;拒绝文案指向替代工具。
TEST(MeshSwarmMode, ToolFamiliesAreMutuallyExclusive) {
    for (const SwarmMode mode : {SwarmMode::Off, SwarmMode::Star}) {
        const auto hidden = swarm_mode_hidden_tools(mode);
        for (const auto& name : mesh_agent_tool_names()) {
            ASSERT_TRUE(hidden.count(name)) << name;
        }
        EXPECT_FALSE(hidden.count("spawn_subagent"));
    }
    const auto mesh_hidden = swarm_mode_hidden_tools(SwarmMode::Mesh);
    for (const auto& name : mesh_agent_tool_names()) EXPECT_FALSE(mesh_hidden.count(name)) << name;
    EXPECT_EQ(mesh_hidden.at("spawn_subagent"),
              "spawn_subagent is not available in mesh swarm mode; use agent_spawn instead.");
    EXPECT_TRUE(mesh_hidden.count("wait_subagent"));
    EXPECT_TRUE(mesh_hidden.count("send_message_to_thread"));
    EXPECT_TRUE(mesh_hidden.count("create_thread"));
}

// 场景:agent 间信封是 Codex InterAgentMessage 文本外包 <inter_agent_message>
// (有意偏离:Codex 用 assistant 角色,这里用 user 角色避免两条 assistant 相邻)。
// 期望:正文逐行对齐 Codex;metadata 往返不丢字段;NEW_TASK 算真实用户消息
// (上下文锚点),MESSAGE / FINAL_ANSWER 是内部上下文。
TEST(MeshInterAgentMessage, RendersCodexBodyAndClassifiesTypes) {
    const auto task = envelope_message(InterAgentMessageType::NewTask, "/root", "/root/worker",
                                       "Investigate the flaky test");
    EXPECT_EQ(task.content,
              "<inter_agent_message>\nMessage Type: NEW_TASK\nTask name: /root/worker\n"
              "Sender: /root\nPayload:\nInvestigate the flaky test\n</inter_agent_message>");
    const auto parsed = inter_agent_envelope_from_metadata(task.metadata);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->type, InterAgentMessageType::NewTask);
    EXPECT_EQ(parsed->sender, "/root");
    EXPECT_EQ(parsed->recipient, "/root/worker");
    EXPECT_TRUE(is_inter_agent_message(task));
    EXPECT_TRUE(is_real_user_message(task));

    const auto note = envelope_message(InterAgentMessageType::Message, "/root/a", "/root/b", "fyi");
    EXPECT_TRUE(is_inter_agent_message(note));
    EXPECT_FALSE(is_real_user_message(note));
    EXPECT_FALSE(is_inter_agent_message(text_message("user", "plain")));
}

// 场景:TUI 把落盘的 user 信封显示成系统行,不能把 <inter_agent_message> 原文糊给用户。
// 期望:三种类型各有一行标题 + Payload 正文;普通消息返回空串(走原显示路径);
// Payload 里再出现 "Payload:" 字样也不会被截错。
TEST(MeshInterAgentMessage, DisplayTextShowsHeaderAndPayload) {
    EXPECT_EQ(inter_agent_display_text(envelope_message(
                  InterAgentMessageType::NewTask, "/root", "/root/w", "do it")),
              "Task from /root to /root/w:\ndo it");
    EXPECT_EQ(inter_agent_display_text(envelope_message(
                  InterAgentMessageType::Message, "/root/a", "/root/b", "x\nPayload:\ny")),
              "Message from /root/a to /root/b:\nx\nPayload:\ny");
    EXPECT_EQ(inter_agent_display_text(envelope_message(
                  InterAgentMessageType::FinalAnswer, "/root/w", "/root", "done", "completed")),
              "Agent /root/w completed:\ndone");
    EXPECT_EQ(inter_agent_display_text(text_message("user", "hello")), "");
    EXPECT_EQ(inter_agent_payload_from_content("no envelope"), "");
}

// 场景:agent_spawn 的 fork_turns 参数(Codex 同款取值与错误文案)。
// 期望:空/all = 全部,none = 不继承,正整数 = 最近 N 轮;0、负数、小数、文字报错。
TEST(MeshForkHistory, ParseForkTurnsMatchesCodex) {
    EXPECT_EQ(parse_fork_turns("")->kind, ForkTurns::Kind::All);
    EXPECT_EQ(parse_fork_turns(" ALL ")->kind, ForkTurns::Kind::All);
    EXPECT_EQ(parse_fork_turns("none")->kind, ForkTurns::Kind::None);
    const auto last = parse_fork_turns("2");
    ASSERT_TRUE(last.has_value());
    EXPECT_EQ(last->kind, ForkTurns::Kind::LastN);
    EXPECT_EQ(last->last_n, 2u);
    for (const char* bad : {"0", "-1", "1.5", "some"}) {
        std::string error;
        EXPECT_FALSE(parse_fork_turns(bad, &error).has_value()) << bad;
        EXPECT_EQ(error, "fork_turns must be `none`, `all`, or a positive integer string");
    }
}

// 场景:子 agent 继承父会话历史时只保留「用户消息 + 最终回答」,工具调用 / 结果、
// 推理与 agent 间信封全部丢弃(Codex keep_forked_rollout_item)。
// 期望:all 保留两轮的 user + 最终 assistant 并打 mesh_inherited 标记、清掉推理;
// LastN=1 只从最后一个轮次边界开始;none 为空。NEW_TASK 信封算轮次边界但本身不继承。
TEST(MeshForkHistory, KeepsUserTurnsAndFinalAnswersOnly) {
    std::vector<ChatMessage> history;
    history.push_back(text_message("user", "first question"));
    history.push_back(tool_call_message());
    history.push_back(text_message("tool", "tool output"));
    auto answer = text_message("assistant", "first answer");
    answer.reasoning_content = "hidden reasoning";
    history.push_back(answer);
    history.push_back(envelope_message(InterAgentMessageType::NewTask, "/root", "/root/me", "task"));
    history.push_back(envelope_message(InterAgentMessageType::Message, "/root/x", "/root/me", "fyi"));
    history.push_back(text_message("assistant", "second answer"));

    const auto all = build_fork_history(history, ForkTurns{});
    ASSERT_EQ(all.size(), 3u);
    EXPECT_EQ(all[0].content, "first question");
    EXPECT_EQ(all[1].content, "first answer");
    EXPECT_TRUE(all[1].reasoning_content.empty());
    EXPECT_EQ(all[2].content, "second answer");
    for (const auto& message : all) EXPECT_TRUE(message.metadata.value("mesh_inherited", false));

    ForkTurns last_one;
    last_one.kind = ForkTurns::Kind::LastN;
    last_one.last_n = 1;
    const auto recent = build_fork_history(history, last_one);
    ASSERT_EQ(recent.size(), 1u);
    EXPECT_EQ(recent[0].content, "second answer");

    ForkTurns none;
    none.kind = ForkTurns::Kind::None;
    EXPECT_TRUE(build_fork_history(history, none).empty());
}

// 场景:daemon 重启后靠根会话目录里的 mesh_agents.json 重建 agent 树。
// 期望:写入后原样读回;缺文件返回空;根路径 / 非法路径 / 空 session_id 条目被跳过,
// 不让一条坏数据毁掉整棵树;JSON 损坏时整体返回空而不是抛异常。
TEST(MeshTreeIndex, RoundTripsAndSkipsMalformedEntries) {
    const fs::path dir = fs::temp_directory_path() /
        ("acecode_mesh_tree_index_" + std::to_string(std::random_device{}()));
    fs::remove_all(dir);
    const std::string project_dir = path_to_utf8(dir);
    EXPECT_TRUE(read_mesh_tree_index(project_dir, "root-1").empty());

    ASSERT_TRUE(write_mesh_tree_index(project_dir, "root-1",
                                      {{"/root/a", "s-a"}, {"/root/a/b", "s-b"}}));
    const auto entries = read_mesh_tree_index(project_dir, "root-1");
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[1].path, "/root/a/b");
    EXPECT_EQ(entries[1].session_id, "s-b");

    {
        std::ofstream out(path_from_utf8(mesh_tree_index_path(project_dir, "root-1")),
                          std::ios::binary | std::ios::trunc);
        out << R"({"version":1,"agents":[{"path":"/root","session_id":"r"},)"
            << R"({"path":"/root/Bad","session_id":"x"},{"path":"/root/ok","session_id":""},)"
            << R"({"path":"/root/good","session_id":"g"},7]})";
    }
    const auto filtered = read_mesh_tree_index(project_dir, "root-1");
    ASSERT_EQ(filtered.size(), 1u);
    EXPECT_EQ(filtered[0].path, "/root/good");

    {
        std::ofstream out(path_from_utf8(mesh_tree_index_path(project_dir, "root-1")),
                          std::ios::binary | std::ios::trunc);
        out << "{not json";
    }
    EXPECT_TRUE(read_mesh_tree_index(project_dir, "root-1").empty());
    fs::remove_all(dir);
}
