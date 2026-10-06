#include <gtest/gtest.h>

#include "im/discord/discord_transport.hpp"
#include "test_support/im/fake_discord_server.hpp"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

// im/discord/discord_transport:Discord 传输层端到端测试(本机假 Discord,不连真实平台)。
// 覆盖:网关登录与状态、私信与服务器频道 @ / 回复机器人 / 过滤与去重、回复定位与分段、
// 回复目标失效的回退、429 等待重发、无回复上下文时打开私聊频道、断线 Resume、op 7 / op 9、
// Resume 被拒后重新登录、致命关闭码(4014)、token 无效、心跳假死、正在输入、文件收发、停机及时。

namespace acecode::im::discord {
namespace {

using test::FakeDiscordServer;
using std::chrono::milliseconds;

const std::string kUser = "100000000000000007";
const std::string kChannel = "500000000000000001";

DiscordTransportOptions local_options(const FakeDiscordServer& server) {
    DiscordTransportOptions options;
    options.api.token = server.token;
    options.api.api_base = server.api_base();
    options.api.use_proxy = false;
    options.rate_limit.send_retry_delay = milliseconds(10);
    options.backoff = {milliseconds(50)};
    options.rate_limit_close_delay = milliseconds(100);
    options.invalid_session_delay_min = milliseconds(10);
    options.invalid_session_delay_max = milliseconds(20);
    options.typing_interval = milliseconds(100);
    options.receive_slice = milliseconds(50);
    options.identify_limits.min_interval = milliseconds(10);
    options.download_hosts = {"127.0.0.1"};
    return options;
}

struct Recorder {
    std::mutex mu;
    std::vector<Inbound> inbound;
    TransportCallbacks callbacks() {
        TransportCallbacks cb;
        cb.on_inbound = [this](Inbound in) {
            std::lock_guard<std::mutex> lock(mu);
            inbound.push_back(std::move(in));
        };
        return cb;
    }
    std::size_t count() {
        std::lock_guard<std::mutex> lock(mu);
        return inbound.size();
    }
    Inbound at(std::size_t i) {
        std::lock_guard<std::mutex> lock(mu);
        return inbound.at(i);
    }
};

bool wait_state(DiscordTransport& transport, LinkState state) {
    return FakeDiscordServer::wait_until([&transport, state] { return transport.status().state == state; });
}

std::string dm_channel() { return FakeDiscordServer::dm_channel_for(kUser); }

// 连上网关并收一条私信,返回这条入站消息(用于后续发送测试)。
Inbound connect_and_receive_dm(FakeDiscordServer& server, DiscordTransport& transport, Recorder& recorder) {
    transport.start(recorder.callbacks());
    EXPECT_TRUE(wait_state(transport, LinkState::Connected));
    server.push_dm("600000000000000001", kUser, "hello");
    EXPECT_TRUE(FakeDiscordServer::wait_until([&recorder] { return recorder.count() >= 1; }));
    return recorder.at(0);
}

Address dm_address(const FakeDiscordServer& server, const std::string& user) {
    Address to;
    to.platform = "discord";
    to.account = server.bot_id;
    to.kind = ChatKind::Private;
    to.chat = to.sender = user;
    return to;
}

bool contains_code(const std::vector<int>& codes, int code) {
    return std::find(codes.begin(), codes.end(), code) != codes.end();
}

// 场景:开启 Discord 通道,网关握手(Hello → Identify → READY)后用户私信机器人。
// 期望:状态变为已连接,account 是机器人用户 id、显示名是机器人用户名、附带邀请链接;
// Identify 用原始 token 与 intents 37377;登录次数通过回调交给调用方持久化;
// 入站私信的地址 chat = sender = 用户 id,视为点名,回复上下文带私聊频道 id;停止后状态为已停止。
TEST(DiscordTransport, ConnectsIdentifiesAndReceivesDirectMessage) {
    FakeDiscordServer server;
    auto options = local_options(server);
    std::atomic<std::int64_t> ledger{0};
    options.on_identify_ledger = [&ledger](std::int64_t, std::int64_t count) { ledger = count; };
    Recorder recorder;  // 停机仍会回调:记录器必须晚于传输对象析构
    DiscordTransport transport(options);
    transport.start(recorder.callbacks());
    ASSERT_TRUE(wait_state(transport, LinkState::Connected));
    const auto status = transport.status();
    EXPECT_EQ(status.account, server.bot_id);
    EXPECT_EQ(status.display_name, "AceBot");
    EXPECT_NE(status.extra.value("invite_url", "").find("client_id=" + server.application_id), std::string::npos);
    EXPECT_EQ(transport.bot_user_id(), server.bot_id);
    const auto identifies = server.frames_with_op(2);
    ASSERT_EQ(identifies.size(), 1u);
    EXPECT_EQ(identifies[0]["d"]["token"], server.token);
    EXPECT_EQ(identifies[0]["d"]["intents"], 37377);
    EXPECT_EQ(ledger.load(), 1);
    const auto gateway = server.requests_to("GET", "/gateway/bot");
    ASSERT_EQ(gateway.size(), 1u);
    EXPECT_EQ(gateway[0].authorization, "Bot " + server.token);

    server.push_dm("600000000000000001", kUser, u8" 你好 ");
    ASSERT_TRUE(FakeDiscordServer::wait_until([&recorder] { return recorder.count() == 1; }));
    const auto in = recorder.at(0);
    EXPECT_EQ(in.address.platform, "discord");
    EXPECT_EQ(in.address.account, server.bot_id);
    EXPECT_EQ(in.address.kind, ChatKind::Private);
    EXPECT_EQ(in.address.chat, kUser);
    EXPECT_EQ(in.address.sender, kUser);
    EXPECT_TRUE(in.mentioned);
    EXPECT_EQ(in.text, u8"你好");
    EXPECT_EQ(in.sender_name, "Alice");
    EXPECT_EQ(in.reply_context.value("channel_id", ""), dm_channel());
    transport.stop();
    EXPECT_EQ(transport.status().state, LinkState::Stopped);
}

// 场景:服务器频道里依次出现:@机器人 的消息、普通闲聊、回复机器人的消息、别的机器人发言、
// 机器人自己消息的回显,以及第一条消息被重复推送(Resume 补发时常见)。
// 期望:上层只收到三条:@ 的那条(点名、去掉提及)、闲聊(未点名,交给核心忽略)、回复机器人
// (点名,带被回复内容);机器人消息、自己的回显和重复消息都不上报。
TEST(DiscordTransport, GuildMentionsRepliesAndFilters) {
    FakeDiscordServer server;
    Recorder recorder;  // 停机仍会回调:记录器必须晚于传输对象析构
    DiscordTransport transport(local_options(server));
    transport.start(recorder.callbacks());
    ASSERT_TRUE(wait_state(transport, LinkState::Connected));
    const auto bot = server.bot_id;
    const nlohmann::json mention{{"mentions", nlohmann::json::array({{{"id", bot}, {"username", "AceBot"}, {"bot", true}}})}};
    server.push_guild("600000000000000011", kChannel, kUser, "<@" + bot + u8"> 看看这个", mention);
    server.push_guild("600000000000000012", kChannel, kUser, u8"随便聊聊");
    server.push_guild("600000000000000013", kChannel, kUser, u8"继续",
                      {{"type", 19},
                       {"referenced_message",
                        {{"id", "600000000000000010"}, {"author", {{"id", bot}, {"bot", true}}}, {"content", u8"之前的回答"}}}});
    server.push_guild("600000000000000014", kChannel, "200000000000000009", u8"我是别的机器人",
                      {{"author", {{"id", "200000000000000009"}, {"username", "other"}, {"bot", true}}}});
    server.push_guild("600000000000000015", kChannel, bot, u8"自己发的",
                      {{"author", {{"id", bot}, {"username", "AceBot"}, {"bot", true}}}});
    server.push_guild("600000000000000011", kChannel, kUser, "<@" + bot + u8"> 看看这个", mention);
    ASSERT_TRUE(FakeDiscordServer::wait_until([&recorder] { return recorder.count() == 3; }));
    std::this_thread::sleep_for(milliseconds(200));
    ASSERT_EQ(recorder.count(), 3u);
    const auto first = recorder.at(0);
    EXPECT_EQ(first.address.kind, ChatKind::Group);
    EXPECT_EQ(first.address.chat, kChannel);
    EXPECT_EQ(first.address.sender, kUser);
    EXPECT_TRUE(first.mentioned);
    EXPECT_EQ(first.text, u8"看看这个");
    EXPECT_FALSE(recorder.at(1).mentioned);
    const auto reply = recorder.at(2);
    EXPECT_TRUE(reply.mentioned);
    EXPECT_EQ(reply.quote_text, u8"之前的回答");
}

// 场景:收到私信后回复一段约 4500 字符、三个段落的长文本。
// 期望:按段落切成三条(每条不超过 2000);只有第一条带 message_reference(fail_if_not_exists=false)
// 指向触发消息;每条都带 allowed_mentions 且只允许提及用户(不会 @everyone / 角色);带 nonce 幂等键。
TEST(DiscordTransport, RepliesWithReferenceAndSafeMentionsInChunks) {
    FakeDiscordServer server;
    Recorder recorder;  // 停机仍会回调:记录器必须晚于传输对象析构
    DiscordTransport transport(local_options(server));
    const auto in = connect_and_receive_dm(server, transport, recorder);
    const auto text = std::string(1500, 'a') + "\n\n" + std::string(1500, 'b') + "\n\n" + std::string(1500, 'c');
    const auto result = transport.send_text(in.address, text, in.reply_context);
    EXPECT_EQ(result.outcome, SendOutcome::Sent) << result.error;
    const auto sent = server.requests_to("POST", "/channels/" + dm_channel() + "/messages");
    ASSERT_EQ(sent.size(), 3u);
    ASSERT_TRUE(sent[0].body.contains("message_reference"));
    EXPECT_EQ(sent[0].body["message_reference"].value("message_id", ""), in.message_id);
    EXPECT_FALSE(sent[0].body["message_reference"].value("fail_if_not_exists", true));
    EXPECT_FALSE(sent[1].body.contains("message_reference"));
    EXPECT_FALSE(sent[2].body.contains("message_reference"));
    for (const auto& request : sent) {
        EXPECT_LE(request.body.value("content", "").size(), 2000u);
        EXPECT_EQ(request.body["allowed_mentions"]["parse"], nlohmann::json::array({"users"}));
        EXPECT_FALSE(request.body.value("nonce", "").empty());
    }
    EXPECT_EQ(sent[1].body.value("content", ""), std::string(1500, 'b'));
}

// 场景:被回复的那条消息已被删除,平台对带 message_reference 的请求返回 10008。
// 期望:去掉回复定位后重发同一段内容并成功;平台共收到两次请求。
TEST(DiscordTransport, ResendsWithoutReferenceWhenReplyTargetIsGone) {
    FakeDiscordServer server;
    server.message_handler = [](const FakeDiscordServer::Request& r) {
        if (r.body.contains("message_reference"))
            return std::make_pair(404, nlohmann::json{{"code", 10008}, {"message", "Unknown Message"}});
        return std::make_pair(0, nlohmann::json());
    };
    Recorder recorder;  // 停机仍会回调:记录器必须晚于传输对象析构
    DiscordTransport transport(local_options(server));
    const auto in = connect_and_receive_dm(server, transport, recorder);
    const auto result = transport.send_text(in.address, "reply", in.reply_context);
    EXPECT_EQ(result.outcome, SendOutcome::Sent) << result.error;
    const auto sent = server.requests_to("POST", "/channels/" + dm_channel() + "/messages");
    ASSERT_EQ(sent.size(), 2u);
    EXPECT_TRUE(sent[0].body.contains("message_reference"));
    EXPECT_FALSE(sent[1].body.contains("message_reference"));
    EXPECT_EQ(sent[1].body.value("content", ""), "reply");
}

// 场景:连续发两条消息,第一条先被 429(retry_after=0.1 秒)拒收。
// 期望:等待后重发,两条都送达,且平台看到的顺序仍是“第一条、第二条”(不丢、不乱序)。
TEST(DiscordTransport, WaitsAndResendsAfterRateLimitInOrder) {
    FakeDiscordServer server;
    std::atomic<int> calls{0};
    server.message_handler = [&calls](const FakeDiscordServer::Request&) {
        if (calls++ == 0)
            return std::make_pair(429, nlohmann::json{{"message", "You are being rate limited."},
                                                      {"retry_after", 0.1}, {"global", false}, {"code", 20028}});
        return std::make_pair(0, nlohmann::json());
    };
    DiscordTransport transport(local_options(server));
    const auto to = dm_address(server, kUser);
    const nlohmann::json context{{"message_id", "600000000000000001"}, {"channel_id", dm_channel()}};
    EXPECT_EQ(transport.send_text(to, u8"第一条", context).outcome, SendOutcome::Sent);
    EXPECT_EQ(transport.send_text(to, u8"第二条", context).outcome, SendOutcome::Sent);
    const auto sent = server.requests_to("POST", "/channels/" + dm_channel() + "/messages");
    ASSERT_EQ(sent.size(), 3u);
    EXPECT_EQ(sent[0].body.value("content", ""), u8"第一条");
    EXPECT_EQ(sent[1].body.value("content", ""), u8"第一条");
    EXPECT_EQ(sent[2].body.value("content", ""), u8"第二条");
}

// 场景:没有回复上下文的私聊发送(Desktop 里输入的回复、核心给机主发绑定码),且本进程还没收过
// 这个用户的私信;连续发两条。
// 期望:先 POST /users/@me/channels 打开私聊频道(只打开一次,之后复用),再往该频道发消息;
// 不带 message_reference;网关没连上也能发(REST 与网关无关)。
TEST(DiscordTransport, OpensDirectMessageChannelWithoutReplyContext) {
    FakeDiscordServer server;
    DiscordTransport transport(local_options(server));
    const std::string user = "100000000000000042";
    const auto to = dm_address(server, user);
    EXPECT_EQ(transport.send_text(to, u8"绑定码:123456", nlohmann::json::object()).outcome, SendOutcome::Sent);
    EXPECT_EQ(transport.send_text(to, u8"第二条", nlohmann::json::object()).outcome, SendOutcome::Sent);
    const auto opens = server.requests_to("POST", "/users/@me/channels");
    ASSERT_EQ(opens.size(), 1u);
    EXPECT_EQ(opens[0].body.value("recipient_id", ""), user);
    const auto sent = server.requests_to("POST", "/channels/" + FakeDiscordServer::dm_channel_for(user) + "/messages");
    ASSERT_EQ(sent.size(), 2u);
    EXPECT_FALSE(sent[0].body.contains("message_reference"));
}

// 场景:连接期间网关以 4000(未知错误,可恢复)断开。
// 期望:重连到 READY 给出的恢复地址(/resume/),用 Resume 带原 session_id 与断开前最后的 seq,
// 不重新 Identify(不消耗登录预算),最终回到已连接。
TEST(DiscordTransport, ResumesAfterRecoverableClose) {
    FakeDiscordServer server;
    Recorder recorder;  // 停机仍会回调:记录器必须晚于传输对象析构
    DiscordTransport transport(local_options(server));
    connect_and_receive_dm(server, transport, recorder);
    const auto seq_before = server.last_seq();
    server.close_connection(4000, "unknown error");
    ASSERT_TRUE(FakeDiscordServer::wait_until([&server] { return server.connections("resume") == 1; }));
    ASSERT_TRUE(FakeDiscordServer::wait_until([&server, &transport] {
        return !server.frames_with_op(6).empty() && transport.status().state == LinkState::Connected;
    }));
    const auto resume = server.frames_with_op(6).front();
    EXPECT_EQ(resume["d"]["session_id"], "S1");
    EXPECT_EQ(resume["d"]["seq"], seq_before);
    EXPECT_EQ(resume["d"]["token"], server.token);
    EXPECT_EQ(server.identify_count(), 1);
}

// 场景:网关发来 op 7(要求重连)。
// 期望:客户端以非 1000 关闭码断开(1000 会让会话作废),连到恢复地址并 Resume,不重新登录。
TEST(DiscordTransport, ServerReconnectRequestResumes) {
    FakeDiscordServer server;
    Recorder recorder;  // 停机仍会回调:记录器必须晚于传输对象析构
    DiscordTransport transport(local_options(server));
    transport.start(recorder.callbacks());
    ASSERT_TRUE(wait_state(transport, LinkState::Connected));
    server.push({{"op", 7}, {"d", nullptr}});
    ASSERT_TRUE(FakeDiscordServer::wait_until([&server] { return !server.frames_with_op(6).empty(); }));
    ASSERT_TRUE(wait_state(transport, LinkState::Connected));
    EXPECT_EQ(server.connections("resume"), 1);
    EXPECT_EQ(server.identify_count(), 1);
    EXPECT_FALSE(contains_code(server.client_close_codes(), 1000));
}

// 场景:网关发来 op 9(d=false,会话作废)。
// 期望:随机等待一小会儿后连回 /gateway/bot 给出的地址重新 Identify(第二次登录),回到已连接;
// 不去恢复地址。
TEST(DiscordTransport, InvalidSessionTriggersFreshIdentify) {
    FakeDiscordServer server;
    Recorder recorder;  // 停机仍会回调:记录器必须晚于传输对象析构
    DiscordTransport transport(local_options(server));
    transport.start(recorder.callbacks());
    ASSERT_TRUE(wait_state(transport, LinkState::Connected));
    server.push({{"op", 9}, {"d", false}});
    ASSERT_TRUE(FakeDiscordServer::wait_until([&server] { return server.identify_count() == 2; }));
    ASSERT_TRUE(wait_state(transport, LinkState::Connected));
    EXPECT_EQ(server.connections("gateway"), 2);
    EXPECT_EQ(server.connections("resume"), 0);
}

// 场景:断线后尝试 Resume,但会话已过期,网关回 op 9(d=false)。
// 期望:放弃恢复,回到 /gateway/bot 的地址重新 Identify,最终回到已连接。
TEST(DiscordTransport, RejectedResumeFallsBackToIdentify) {
    FakeDiscordServer server;
    server.resume_ok = false;
    Recorder recorder;  // 停机仍会回调:记录器必须晚于传输对象析构
    DiscordTransport transport(local_options(server));
    transport.start(recorder.callbacks());
    ASSERT_TRUE(wait_state(transport, LinkState::Connected));
    server.close_connection(4000, "unknown error");
    ASSERT_TRUE(FakeDiscordServer::wait_until([&server] { return server.identify_count() == 2; }));
    ASSERT_TRUE(wait_state(transport, LinkState::Connected));
    EXPECT_EQ(server.connections("resume"), 1);
    EXPECT_EQ(server.frames_with_op(6).size(), 1u);
}

// 场景:开发者后台没有开启 Message Content Intent,网关在 Identify 后以 4014 关闭。
// 期望:状态变为失败并停止重试(不会反复登录消耗预算),原因提示去开启 Message Content Intent。
TEST(DiscordTransport, DisallowedIntentIsFatal) {
    FakeDiscordServer server;
    server.identify_close_code = 4014;
    Recorder recorder;  // 停机仍会回调:记录器必须晚于传输对象析构
    DiscordTransport transport(local_options(server));
    transport.start(recorder.callbacks());
    ASSERT_TRUE(wait_state(transport, LinkState::Failed));
    const auto status = transport.status();
    EXPECT_TRUE(status.retry_stopped);
    EXPECT_NE(status.detail.find("Message Content Intent"), std::string::npos) << status.detail;
    std::this_thread::sleep_for(milliseconds(300));
    EXPECT_EQ(server.connections("gateway"), 1);
    EXPECT_EQ(server.identify_count(), 1);
}

// 场景:token 已被重置,GET /gateway/bot 返回 401。
// 期望:不去连网关,状态直接变为失败且停止重试;状态文字里没有 token。
TEST(DiscordTransport, InvalidTokenStopsBeforeConnecting) {
    FakeDiscordServer server;
    auto options = local_options(server);
    options.api.token = "OTk5OTk5.wrong.token-that-must-not-appear";
    Recorder recorder;  // 停机仍会回调:记录器必须晚于传输对象析构
    DiscordTransport transport(options);
    transport.start(recorder.callbacks());
    ASSERT_TRUE(wait_state(transport, LinkState::Failed));
    const auto status = transport.status();
    EXPECT_TRUE(status.retry_stopped);
    EXPECT_EQ(status.detail.find("token-that-must-not-appear"), std::string::npos);
    std::this_thread::sleep_for(milliseconds(200));
    EXPECT_EQ(server.connections("gateway"), 0);
    EXPECT_EQ(server.requests_to("GET", "/gateway/bot").size(), 1u);
}

// 场景:网关不再回心跳 ACK(连接假死;心跳间隔调成 150 毫秒)。
// 期望:下一拍到期时客户端主动断开(不用 1000)并 Resume,不重新登录;恢复 ACK 后回到已连接。
TEST(DiscordTransport, ZombieConnectionIsClosedAndResumed) {
    FakeDiscordServer server;
    server.heartbeat_interval_ms = 150;
    server.ack_heartbeats = false;
    Recorder recorder;  // 停机仍会回调:记录器必须晚于传输对象析构
    DiscordTransport transport(local_options(server));
    transport.start(recorder.callbacks());
    ASSERT_TRUE(FakeDiscordServer::wait_until([&server] { return server.connections("resume") >= 1; }));
    server.ack_heartbeats = true;
    ASSERT_TRUE(FakeDiscordServer::wait_until([&server] { return !server.frames_with_op(6).empty(); }));
    ASSERT_TRUE(wait_state(transport, LinkState::Connected));
    EXPECT_EQ(server.identify_count(), 1);
    EXPECT_FALSE(contains_code(server.client_close_codes(), 1000));
}

// 场景:回合进行中打开“正在输入”,一段时间后关闭(测试里间隔调成 100 毫秒;真实为 8 秒,
// 因为指示器 10 秒后自动消失)。
// 期望:打开期间持续向私聊频道 POST /typing;关闭后不再发送。
TEST(DiscordTransport, TypingIndicatorRepeatsUntilTurnedOff) {
    FakeDiscordServer server;
    Recorder recorder;  // 停机仍会回调:记录器必须晚于传输对象析构
    DiscordTransport transport(local_options(server));
    const auto in = connect_and_receive_dm(server, transport, recorder);
    EXPECT_TRUE(transport.capabilities().supports_typing);
    const auto path = "/channels/" + dm_channel() + "/typing";
    transport.set_typing(in.address, true);
    ASSERT_TRUE(FakeDiscordServer::wait_until([&server, &path] { return server.requests_to("POST", path).size() >= 3; }));
    transport.set_typing(in.address, false);
    std::this_thread::sleep_for(milliseconds(150));
    const auto settled = server.requests_to("POST", path).size();
    std::this_thread::sleep_for(milliseconds(350));
    EXPECT_EQ(server.requests_to("POST", path).size(), settled);
}

// 场景:回传一个文件名含中文的文件,带回复上下文。
// 期望:multipart 只有 payload_json 与 files[0] 两个字段;payload_json 的 attachments[0] 用原始
// 文件名(对方看到的名字)、id 为 0,并带回复定位;文件内容原样上传;结果为已发送。
TEST(DiscordTransport, SendsFileAsMultipartWithPayloadJson) {
    FakeDiscordServer server;
    DiscordTransport transport(local_options(server));
    const auto to = dm_address(server, kUser);
    const nlohmann::json context{{"message_id", "600000000000000001"}, {"channel_id", dm_channel()}};
    const auto file = std::filesystem::path(testing::TempDir()) / "acecode-discord-report.pdf";
    { std::ofstream(file, std::ios::binary) << "PDF-DATA"; }
    const auto result = transport.send_file(to, file, u8"报告.pdf", "application/pdf", context);
    EXPECT_EQ(result.outcome, SendOutcome::Sent) << result.error;
    const auto sent = server.requests_to("POST", "/channels/" + dm_channel() + "/messages");
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0].part_names, (std::vector<std::string>{"payload_json", "files[0]"}));
    EXPECT_EQ(sent[0].body["attachments"][0].value("filename", ""), u8"报告.pdf");
    EXPECT_EQ(sent[0].body["attachments"][0]["id"], 0);
    EXPECT_EQ(sent[0].body["message_reference"].value("message_id", ""), "600000000000000001");
    EXPECT_EQ(sent[0].file_body, "PDF-DATA");
    std::filesystem::remove(file);
}

// 场景:上传请求返回 200,但消息里的 attachments 是空的(hermes 记录过的平台偶发问题)。
// 期望:不当作成功,返回失败并提示没有收到文件。
TEST(DiscordTransport, UploadWithoutAttachmentsInResponseFails) {
    FakeDiscordServer server;
    server.message_handler = [](const FakeDiscordServer::Request& r) {
        if (!r.file_name.empty())
            return std::make_pair(200, nlohmann::json{{"id", "990000000000000001"}, {"attachments", nlohmann::json::array()}});
        return std::make_pair(0, nlohmann::json());
    };
    DiscordTransport transport(local_options(server));
    const auto file = std::filesystem::path(testing::TempDir()) / "acecode-discord-empty.txt";
    { std::ofstream(file, std::ios::binary) << "data"; }
    const auto result = transport.send_file(dm_address(server, kUser), file, "a.txt", "text/plain", nlohmann::json::object());
    EXPECT_EQ(result.outcome, SendOutcome::Failed);
    EXPECT_NE(result.error.find(u8"没有收到文件"), std::string::npos) << result.error;
    std::filesystem::remove(file);
}

// 场景:文件超过本地上传上限(测试里调到 4 字节);以及没超本地上限但平台返回 413 / 40005
// (服务器的实际上限更低)。
// 期望:两种情况都改发一条带文件名的文字说明,结果为已发送;前者根本不上传。
TEST(DiscordTransport, OversizedFileBecomesTextNotice) {
    FakeDiscordServer server;
    std::atomic<int> uploads{0};
    server.message_handler = [&uploads](const FakeDiscordServer::Request& r) {
        if (!r.file_name.empty()) {
            ++uploads;
            return std::make_pair(413, nlohmann::json{{"code", 40005}, {"message", "Request entity too large"}});
        }
        return std::make_pair(0, nlohmann::json());
    };
    const auto file = std::filesystem::path(testing::TempDir()) / "acecode-discord-big.pdf";
    { std::ofstream(file, std::ios::binary) << "PDF-DATA"; }
    const auto to = dm_address(server, kUser);
    {
        auto options = local_options(server);
        options.max_upload_bytes = 4;
        DiscordTransport transport(options);
        EXPECT_EQ(transport.send_file(to, file, "big.pdf", "application/pdf", {}).outcome, SendOutcome::Sent);
        EXPECT_EQ(uploads.load(), 0);
    }
    {
        DiscordTransport transport(local_options(server));
        EXPECT_EQ(transport.send_file(to, file, "big.pdf", "application/pdf", {}).outcome, SendOutcome::Sent);
        EXPECT_EQ(uploads.load(), 1);
    }
    const auto sent = server.requests_to("POST", "/channels/" + dm_channel() + "/messages");
    std::size_t notices = 0;
    for (const auto& r : sent)
        if (r.file_name.empty() && r.body.value("content", "").find("big.pdf") != std::string::npos) ++notices;
    EXPECT_EQ(notices, 2u);
    std::filesystem::remove(file);
}

// 场景:下载入站附件(CDN 签名地址),以及一个超过下载上限的附件(测试里上限 1024 字节)。
// 期望:前者下载成功、请求里不带 bot token(签名地址自带授权,token 不能发给 CDN);
// 后者失败,原因是超过大小限制,不留下半截文件。
TEST(DiscordTransport, DownloadsAttachmentWithoutBotToken) {
    FakeDiscordServer server;
    auto options = local_options(server);
    options.max_download_bytes = 1024;
    DiscordTransport transport(options);
    Attachment small;
    small.remote_ref = server.cdn_url(kChannel, "1290000000000000099", "notes.txt");
    const auto dest = std::filesystem::path(testing::TempDir()) / "acecode-discord-notes.txt";
    std::string error;
    ASSERT_TRUE(transport.download(small, dest, &error)) << error;
    EXPECT_EQ(std::filesystem::file_size(dest), 32u);
    const auto cdn = server.requests_to("GET", "/cdn/attachments/");
    ASSERT_EQ(cdn.size(), 1u);
    EXPECT_TRUE(cdn[0].authorization.empty());
    std::filesystem::remove(dest);

    Attachment big;
    big.remote_ref = server.cdn_url(kChannel, "1290000000000000098", "big.bin");
    const auto big_dest = std::filesystem::path(testing::TempDir()) / "acecode-discord-big.bin";
    EXPECT_FALSE(transport.download(big, big_dest, &error));
    EXPECT_NE(error.find(u8"大小限制"), std::string::npos) << error;
    EXPECT_FALSE(std::filesystem::exists(big_dest));
}

// 场景:附件地址不在 Discord CDN 的主机上(默认只允许 cdn.discordapp.com / media.discordapp.net)。
// 期望:拒绝下载并说明原因,不发出任何请求。
TEST(DiscordTransport, RejectsAttachmentHostsOutsideTheCdn) {
    FakeDiscordServer server;
    auto options = local_options(server);
    options.download_hosts = {"cdn.discordapp.com", "media.discordapp.net"};
    DiscordTransport transport(options);
    Attachment attachment;
    attachment.remote_ref = server.cdn_url(kChannel, "1290000000000000099", "notes.txt");
    std::string error;
    EXPECT_FALSE(transport.download(attachment, std::filesystem::path(testing::TempDir()) / "acecode-discord-x.txt", &error));
    EXPECT_NE(error.find("CDN"), std::string::npos) << error;
    EXPECT_TRUE(server.requests_to("GET", "/cdn/").empty());
}

// 场景:收到带附件的私信,但等到下载时签名地址已过期(CDN 返回 404)。
// 期望:重新取一次这条消息拿到新的签名地址,再下载成功。
TEST(DiscordTransport, RefreshesExpiredAttachmentUrl) {
    FakeDiscordServer server;
    const std::string attachment_id = "1290000000000000099";
    server.expired_attachments = {attachment_id};
    Recorder recorder;  // 停机仍会回调:记录器必须晚于传输对象析构
    DiscordTransport transport(local_options(server));
    transport.start(recorder.callbacks());
    ASSERT_TRUE(wait_state(transport, LinkState::Connected));
    server.push_dm("600000000000000021", kUser, u8"看附件",
                   nlohmann::json::array({{{"id", attachment_id}, {"filename", "notes.txt"}, {"size", 32},
                                           {"content_type", "text/plain"},
                                           {"url", server.cdn_url(dm_channel(), attachment_id, "notes.txt")}}}));
    ASSERT_TRUE(FakeDiscordServer::wait_until([&recorder] { return recorder.count() == 1; }));
    const auto in = recorder.at(0);
    ASSERT_EQ(in.attachments.size(), 1u);
    const auto dest = std::filesystem::path(testing::TempDir()) / "acecode-discord-refreshed.txt";
    std::string error;
    ASSERT_TRUE(transport.download(in.attachments[0], dest, &error)) << error;
    EXPECT_EQ(server.requests_to("GET", "/channels/" + dm_channel() + "/messages/600000000000000021").size(), 1u);
    EXPECT_EQ(server.requests_to("GET", "/cdn/attachments/").size(), 2u);
    std::filesystem::remove(dest);
}

// 场景:/gateway/bot 一直返回 500,传输层处于 30 秒的重连退避中,此时关闭通道。
// 期望:stop() 立即打断等待并返回(2 秒内),状态为已停止。
TEST(DiscordTransport, StopIsPromptDuringBackoff) {
    FakeDiscordServer server;
    server.gateway_bot_status = 500;
    auto options = local_options(server);
    options.backoff = {std::chrono::seconds(30)};
    Recorder recorder;  // 停机仍会回调:记录器必须晚于传输对象析构
    DiscordTransport transport(options);
    transport.start(recorder.callbacks());
    ASSERT_TRUE(wait_state(transport, LinkState::Retrying));
    const auto begin = std::chrono::steady_clock::now();
    transport.stop();
    EXPECT_LT(std::chrono::steady_clock::now() - begin, std::chrono::seconds(2));
    EXPECT_EQ(transport.status().state, LinkState::Stopped);
}

} // namespace
} // namespace acecode::im::discord
