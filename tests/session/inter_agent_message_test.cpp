// 覆盖蜂群模式（网状）的跨 agent 信封与会话元数据身份字段:
//   1. 信封正文逐字等于 Codex InterAgentMessage 的 body,外层包 <inter_agent_message>
//   2. metadata.inter_agent 往返:类型、发件人、收件人、发件会话 id、完成状态
//   3. 消息谓词:NEW_TASK 视同真实用户请求,MESSAGE / FINAL_ANSWER 属内部上下文
//   4. SessionMeta 的 swarm_mode / mesh_agent 字段往返,off 与空身份序列化省略
//   5. 蜂群模式名解析:canonical 名、大小写、旧布尔 true/false、非法值
//   6. 信封不计入可见用户回合、不进入会话摘要

#include <gtest/gtest.h>

#include "llm/message_predicates.hpp"
#include "session/inter_agent_message.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "session/swarm_mode.hpp"
#include "utils/utf8_path.hpp"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace fs = std::filesystem;
using acecode::ChatMessage;
using acecode::SessionMeta;
using acecode::SessionStorage;
using acecode::SwarmMode;
using acecode::mesh::InterAgentEnvelope;
using acecode::mesh::InterAgentMessageType;

namespace {

fs::path make_tmp_dir(const std::string& hint) {
    auto dir = fs::temp_directory_path() /
        ("acecode_inter_agent_" + hint + "_" + std::to_string(std::random_device{}()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

ChatMessage envelope_message(InterAgentMessageType type) {
    InterAgentEnvelope envelope;
    envelope.type = type;
    envelope.recipient = "/root/a";
    envelope.sender = "/root";
    envelope.payload = "run the tests";
    ChatMessage message;
    message.role = "user";
    message.content = acecode::mesh::render_inter_agent_message(envelope);
    message.metadata = nlohmann::json::object();
    message.metadata[acecode::mesh::kInterAgentMetadataKey] =
        acecode::mesh::inter_agent_metadata(envelope);
    return message;
}

} // namespace

// 场景:/root 给 /root/a 发一条 MESSAGE,正文 "请先跑测试"。
// 期望:body 四行字段与 Codex 的 format!("Message Type: {}\nTask name: {}\nSender: {}\nPayload:\n{}")
// 逐字一致;完整消息外层包 <inter_agent_message> 标签(有意偏离:Codex 用 assistant
// 角色且无标签,ACECode 用 user 角色需要标签与真实用户输入区分)。
TEST(MeshInterAgentMessage, RendersCodexBodyInsideTags) {
    InterAgentEnvelope envelope;
    envelope.type = InterAgentMessageType::Message;
    envelope.recipient = "/root/a";
    envelope.sender = "/root";
    envelope.payload = u8"请先跑测试";
    EXPECT_EQ(acecode::mesh::render_inter_agent_body(envelope),
              std::string("Message Type: MESSAGE\nTask name: /root/a\nSender: /root\nPayload:\n") +
                  u8"请先跑测试");
    EXPECT_EQ(acecode::mesh::render_inter_agent_message(envelope),
              std::string("<inter_agent_message>\nMessage Type: MESSAGE\nTask name: /root/a\n"
                          "Sender: /root\nPayload:\n") + u8"请先跑测试" + "\n</inter_agent_message>");
    envelope.type = InterAgentMessageType::FinalAnswer;
    EXPECT_NE(acecode::mesh::render_inter_agent_body(envelope).find("Message Type: FINAL_ANSWER"),
              std::string::npos);
}

// 场景:带发件会话 id 与完成状态的 FINAL_ANSWER 元数据写入后再读回。
// 期望:类型、路径、会话 id、status 全部还原;非对象或缺类型的元数据读不出信封。
TEST(MeshInterAgentMessage, MetadataRoundTrips) {
    InterAgentEnvelope envelope;
    envelope.type = InterAgentMessageType::FinalAnswer;
    envelope.recipient = "/root";
    envelope.sender = "/root/a";
    envelope.sender_session_id = "20261003-010203-abcd";
    envelope.final_status = "errored";
    nlohmann::json metadata = {{"inter_agent", acecode::mesh::inter_agent_metadata(envelope)}};
    const auto parsed = acecode::mesh::inter_agent_envelope_from_metadata(metadata);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->type, InterAgentMessageType::FinalAnswer);
    EXPECT_EQ(parsed->recipient, "/root");
    EXPECT_EQ(parsed->sender, "/root/a");
    EXPECT_EQ(parsed->sender_session_id, "20261003-010203-abcd");
    EXPECT_EQ(parsed->final_status, "errored");
    EXPECT_FALSE(acecode::mesh::inter_agent_envelope_from_metadata(nlohmann::json::array()));
    EXPECT_FALSE(acecode::mesh::inter_agent_envelope_from_metadata(
        nlohmann::json{{"inter_agent", {{"type", "BOGUS"}}}}));
}

// 场景:三种信封分别交给消息谓词判断。
// 期望:NEW_TASK 是真实用户消息(子 agent 的任务就是它的「用户请求」,可变上下文
// 要锚在它前面);MESSAGE 与 FINAL_ANSWER 不是 —— 否则回合中途每到一封信,
// insert_context_before_last_real_user_or_summary 的锚点就后移一次,同回合的
// 前缀缓存从锚点处被打穿。普通用户消息不受影响。
TEST(MeshInterAgentMessage, OnlyNewTaskCountsAsRealUserMessage) {
    EXPECT_TRUE(acecode::is_real_user_message(envelope_message(InterAgentMessageType::NewTask)));
    EXPECT_FALSE(acecode::is_real_user_message(envelope_message(InterAgentMessageType::Message)));
    EXPECT_FALSE(acecode::is_real_user_message(envelope_message(InterAgentMessageType::FinalAnswer)));
    EXPECT_TRUE(acecode::mesh::is_inter_agent_message(envelope_message(InterAgentMessageType::Message)));
    ChatMessage plain;
    plain.role = "user";
    plain.content = "hello";
    EXPECT_TRUE(acecode::is_real_user_message(plain));
    EXPECT_FALSE(acecode::mesh::is_inter_agent_message(plain));
}

// 场景:子 agent 的 meta 写入 swarm_mode=mesh、父会话(=树根)与 mesh_agent 路径后读回;另一个
// 普通会话 swarm_mode 为空。
// 期望:身份字段原样还原;普通会话的 JSON 里不出现 swarm_mode / mesh_agent 键
// (老 meta 逐字节不变,旧版本读新 meta 时也只是忽略多出的键)。
TEST(MeshInterAgentMessage, SessionMetaSwarmFieldsRoundTripAndAreOmittedWhenOff) {
    const auto dir = make_tmp_dir("meta");
    const auto mesh_path = (dir / "20261003-000000-aaaa.meta.json").string();
    SessionMeta mesh;
    mesh.id = "20261003-000000-aaaa";
    mesh.swarm_mode = "mesh";
    mesh.parent_session_id = "20261003-000000-root";
    mesh.agent_path = "/root/explore/tests";
    ASSERT_TRUE(SessionStorage::write_meta(mesh_path, mesh));
    const SessionMeta read = SessionStorage::read_meta(mesh_path);
    EXPECT_EQ(read.swarm_mode, "mesh");
    EXPECT_EQ(read.parent_session_id, "20261003-000000-root");
    EXPECT_EQ(read.agent_path, "/root/explore/tests");

    const auto plain_path = (dir / "20261003-000000-bbbb.meta.json").string();
    SessionMeta plain;
    plain.id = "20261003-000000-bbbb";
    plain.swarm_mode = "off";
    ASSERT_TRUE(SessionStorage::write_meta(plain_path, plain));
    std::ifstream in(plain_path);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(text.find("swarm_mode"), std::string::npos) << text;
    EXPECT_EQ(text.find("mesh_agent"), std::string::npos) << text;
    in.close();
    fs::remove_all(dir);
}

// 场景:解析蜂群模式名,含旧客户端的布尔字面量。
// 期望:off/star/mesh 不分大小写;"true" 兼容为 star、"false" 为 off;空串与
// 未知值返回 nullopt(由调用方报 400 / 命令错误)。
TEST(MeshInterAgentMessage, ParsesSwarmModeNames) {
    EXPECT_EQ(acecode::parse_swarm_mode("mesh"), SwarmMode::Mesh);
    EXPECT_EQ(acecode::parse_swarm_mode("STAR"), SwarmMode::Star);
    EXPECT_EQ(acecode::parse_swarm_mode("off"), SwarmMode::Off);
    EXPECT_EQ(acecode::parse_swarm_mode("true"), SwarmMode::Star);
    EXPECT_EQ(acecode::parse_swarm_mode("false"), SwarmMode::Off);
    EXPECT_FALSE(acecode::parse_swarm_mode("ring").has_value());
    EXPECT_FALSE(acecode::parse_swarm_mode("").has_value());
    EXPECT_STREQ(acecode::swarm_mode_name(SwarmMode::Mesh), "mesh");
}

// 场景:一个只收到过 NEW_TASK 与 MESSAGE 信封的子 agent 会话落盘;然后设置
// 蜂群模式与网状身份。
// 期望:可见用户回合数为 0、摘要为空(侧栏 / 列表不会把信封当成用户输入);
// 模式与身份经 SessionManager 写进 meta,重新读取一致。
TEST(MeshInterAgentMessage, EnvelopesDoNotCountAsVisibleUserTurns) {
    const auto dir = make_tmp_dir("manager");
    const std::string cwd = acecode::path_to_utf8(dir);
    acecode::SessionManager manager;
    manager.start_session(cwd, "openai", "gpt-test", "20261003-111111-cccc", "", "daemon");
    manager.set_swarm_mode("mesh");
    manager.set_mesh_agent_path("/root/a");
    manager.on_message(envelope_message(InterAgentMessageType::NewTask));
    manager.on_message(envelope_message(InterAgentMessageType::Message));
    const std::string project_dir = SessionStorage::get_project_dir(cwd);
    const SessionMeta meta = SessionStorage::read_meta(
        SessionStorage::meta_path(project_dir, "20261003-111111-cccc"));
    EXPECT_EQ(meta.turn_count, 0);
    EXPECT_TRUE(meta.summary.empty()) << meta.summary;
    EXPECT_EQ(meta.swarm_mode, "mesh");
    EXPECT_EQ(meta.agent_path, "/root/a");
    EXPECT_EQ(manager.current_swarm_mode(), "mesh");
    manager.end_current_session();
    fs::remove_all(project_dir);
    fs::remove_all(dir);
}
