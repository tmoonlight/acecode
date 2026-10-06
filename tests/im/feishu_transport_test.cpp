#include <gtest/gtest.h>

#include "im/feishu/feishu_transport.hpp"
#include "test_support/im/fake_feishu_server.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

// im/feishu/feishu_transport:飞书传输层端到端测试。本机假开放平台提供令牌、机器人信息、
// 长连接地址与二进制 WebSocket 长连接、发消息与上传下载接口;测试覆盖连接与心跳、事件 ACK、
// 私聊 / 群 @ / 去重 / 陈旧丢弃 / 拆包、回复与分段、post → 纯文本回退、限流重发、
// 回复目标消失、断线重连、致命错误停止重试、文件收发与 close 帧。

namespace acecode::im::feishu {
namespace {

using std::chrono::milliseconds;
using test::FakeFeishuServer;

FeishuTransportOptions local_options(const FakeFeishuServer& server) {
    FeishuTransportOptions options;
    options.api.app_id = "cli_app";
    options.api.app_secret = "secret-value-123";
    options.api.base = server.base();
    options.api.use_proxy = false;
    options.backoff = {milliseconds(50)};
    options.rate_limit_backoff = {milliseconds(50), milliseconds(50)};
    options.retry_backoff = {milliseconds(20)};
    options.send_gap = milliseconds(1);
    options.receive_poll = milliseconds(50);
    return options;
}

struct Recorder {
    std::mutex mu;
    std::vector<Inbound> inbound;
    std::vector<TransportStatus> statuses;
    TransportCallbacks callbacks() {
        TransportCallbacks cb;
        cb.on_inbound = [this](Inbound in) {
            std::lock_guard<std::mutex> lock(mu);
            inbound.push_back(std::move(in));
        };
        cb.on_status = [this](const TransportStatus& s) {
            std::lock_guard<std::mutex> lock(mu);
            statuses.push_back(s);
        };
        return cb;
    }
    std::size_t inbound_count() {
        std::lock_guard<std::mutex> lock(mu);
        return inbound.size();
    }
    Inbound inbound_at(std::size_t i) {
        std::lock_guard<std::mutex> lock(mu);
        return inbound.at(i);
    }
    std::vector<TransportStatus> all_statuses() {
        std::lock_guard<std::mutex> lock(mu);
        return statuses;
    }
};

bool wait_connected(FeishuTransport& transport, FakeFeishuServer& server, int connections = 1) {
    return FakeFeishuServer::wait_until([&] {
        return transport.status().state == LinkState::Connected && server.connections() >= connections;
    });
}

nlohmann::json p2p_text(const std::string& id, const std::string& text) {
    return FakeFeishuServer::message_event(id, "p2p", "oc_p2p", "ou_user", "text", {{"text", text}});
}

nlohmann::json bot_mention() {
    return nlohmann::json::array(
        {{{"key", "@_user_1"}, {"id", {{"open_id", "ou_bot"}}}, {"mentioned_type", "bot"}, {"name", "TestBot"}}});
}

nlohmann::json content_of(const FakeFeishuServer::Request& request) {
    return nlohmann::json::parse(request.body.value("content", std::string("{}")));
}

std::filesystem::path temp_file(const char* name, const std::string& data) {
    const auto path = std::filesystem::path(testing::TempDir()) / name;
    std::ofstream(path, std::ios::binary) << data;
    return path;
}

// 场景:开启飞书通道,长连接建立后收到一条私聊文本。
// 期望:先换令牌、取机器人信息,再用 AppID / AppSecret(PascalCase、locale: zh、带 channel 的 UA)
// 请求长连接地址;连上即发心跳(带地址里的 service_id);状态为已连接并显示机器人名;事件帧被
// 原样回显 ACK({"code":200}、带 biz_rt、instance_id 不变);入站消息地址、文本与回复上下文正确;
// 状态里不出现长连接票据与密钥;停止后状态为已停止。
TEST(FeishuTransport, ConnectsAcksAndDeliversPrivateMessage) {
    FakeFeishuServer server;
    Recorder recorder;  // 停机仍会回调:记录器必须晚于传输对象析构
    FeishuTransport transport(local_options(server));
    transport.start(recorder.callbacks());
    ASSERT_TRUE(wait_connected(transport, server));
    const auto status = transport.status();
    EXPECT_EQ(status.display_name, "TestBot");
    EXPECT_EQ(status.account, "cli_app");
    EXPECT_EQ(status.extra.value("bot_ready", false), true);
    EXPECT_EQ(transport.bot().open_id, "ou_bot");

    const auto endpoint = server.requests_to("/callback/ws/endpoint");
    ASSERT_EQ(endpoint.size(), 1u);
    EXPECT_EQ(endpoint[0].body.value("AppID", ""), "cli_app");
    EXPECT_EQ(endpoint[0].locale, "zh");
    EXPECT_NE(endpoint[0].user_agent.find(" channel"), std::string::npos);

    ASSERT_TRUE(FakeFeishuServer::wait_until([&] { return !server.frames_of_type("ping").empty(); }));
    EXPECT_EQ(server.frames_of_type("ping")[0].service, 1234);
    EXPECT_EQ(server.text_frames(), 0);

    server.push_event(p2p_text("om_1", "你好"));
    ASSERT_TRUE(FakeFeishuServer::wait_until([&] { return recorder.inbound_count() == 1; }));
    const auto inbound = recorder.inbound_at(0);
    EXPECT_EQ(inbound.address.kind, ChatKind::Private);
    EXPECT_EQ(inbound.address.chat, "ou_user");
    EXPECT_EQ(inbound.address.sender, "ou_user");
    EXPECT_EQ(inbound.address.account, "cli_app");
    EXPECT_TRUE(inbound.mentioned);
    EXPECT_EQ(inbound.text, "你好");
    EXPECT_EQ(inbound.reply_context.value("message_id", ""), "om_1");
    EXPECT_EQ(inbound.reply_context.value("chat_id", ""), "oc_p2p");

    ASSERT_TRUE(FakeFeishuServer::wait_until([&] { return server.frames_of_type("event").size() == 1; }));
    const auto ack = server.frames_of_type("event")[0];
    EXPECT_EQ(ack.method, kMethodData);
    EXPECT_EQ(ack.header("instance_id"), "inst-1");
    EXPECT_TRUE(ack.has_header("biz_rt"));
    EXPECT_EQ(ack.payload.value_or(""), R"({"code":200})");

    for (const auto& s : recorder.all_statuses()) {
        EXPECT_EQ(s.detail.find("TICKET-SECRET"), std::string::npos);
        EXPECT_EQ(s.detail.find("secret-value-123"), std::string::npos);
    }
    transport.stop();
    EXPECT_EQ(transport.status().state, LinkState::Stopped);
}

// 场景:群里依次收到 —— @机器人 的消息、同一条消息的重投、不 @ 机器人的消息、2 小时前的旧消息。
// 期望:@ 的消息 mentioned=true 且去掉了 @;重投只交给核心一次;不 @ 的消息 mentioned=false
// (是否响应由核心决定);旧消息被丢弃。四个事件帧全部 ACK(包括重复与丢弃的),避免平台重投。
TEST(FeishuTransport, GroupMentionsDedupeAndStaleEvents) {
    FakeFeishuServer server;
    Recorder recorder;  // 停机仍会回调:记录器必须晚于传输对象析构
    FeishuTransport transport(local_options(server));
    transport.start(recorder.callbacks());
    ASSERT_TRUE(wait_connected(transport, server));
    const auto mentioned = FakeFeishuServer::message_event("om_g1", "group", "oc_group", "ou_user", "text",
                                                           {{"text", "@_user_1 看看日志"}}, bot_mention());
    server.push_event(mentioned);
    server.push_event(mentioned);
    server.push_event(FakeFeishuServer::message_event("om_g2", "group", "oc_group", "ou_other", "text",
                                                      {{"text", "大家好"}}));
    const auto two_hours_ago = std::chrono::duration_cast<milliseconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count() -
                               2 * 3600 * 1000;
    server.push_event(FakeFeishuServer::message_event("om_old", "p2p", "oc_p2p", "ou_user", "text", {{"text", "旧"}},
                                                      nullptr, two_hours_ago));
    ASSERT_TRUE(FakeFeishuServer::wait_until([&] { return server.frames_of_type("event").size() == 4; }));
    ASSERT_TRUE(FakeFeishuServer::wait_until([&] { return recorder.inbound_count() == 2; }));
    std::this_thread::sleep_for(milliseconds(200));
    EXPECT_EQ(recorder.inbound_count(), 2u);
    const auto first = recorder.inbound_at(0);
    EXPECT_EQ(first.address.kind, ChatKind::Group);
    EXPECT_EQ(first.address.chat, "oc_group");
    EXPECT_EQ(first.address.sender, "ou_user");
    EXPECT_TRUE(first.mentioned);
    EXPECT_EQ(first.text, "看看日志");
    const auto second = recorder.inbound_at(1);
    EXPECT_FALSE(second.mentioned);
    EXPECT_EQ(second.address.sender, "ou_other");
}

// 场景:一个事件被服务端拆成 3 片推送。
// 期望:重组成一条入站消息;只对凑齐的最后一片回 ACK(共 1 个)。
TEST(FeishuTransport, ReassemblesSplitEvents) {
    FakeFeishuServer server;
    Recorder recorder;  // 停机仍会回调:记录器必须晚于传输对象析构
    FeishuTransport transport(local_options(server));
    transport.start(recorder.callbacks());
    ASSERT_TRUE(wait_connected(transport, server));
    server.push_event(p2p_text("om_split", std::string(300, 'x')), 3);
    ASSERT_TRUE(FakeFeishuServer::wait_until([&] { return recorder.inbound_count() == 1; }));
    EXPECT_EQ(recorder.inbound_at(0).text, std::string(300, 'x'));
    ASSERT_TRUE(FakeFeishuServer::wait_until([&] { return server.frames_of_type("event").size() == 1; }));
    std::this_thread::sleep_for(milliseconds(150));
    EXPECT_EQ(server.frames_of_type("event").size(), 1u);
}

// 场景:对一条私聊消息回复一段超过单条上限的 Markdown。
// 期望:整条按 post 发送;第一段走回复接口(回复触发消息),之后的分段按 chat_id 直接发到会话;
// 每段 content 是 {"zh_cn":{"content":[[{"tag":"md",…}]]}};每次请求带 uuid。
TEST(FeishuTransport, RepliesToTriggerThenCreatesLaterChunks) {
    FakeFeishuServer server;
    Recorder recorder;  // 停机仍会回调:记录器必须晚于传输对象析构
    FeishuTransport transport(local_options(server));
    transport.start(recorder.callbacks());
    ASSERT_TRUE(wait_connected(transport, server));
    server.push_event(p2p_text("om_1", "写个报告"));
    ASSERT_TRUE(FakeFeishuServer::wait_until([&] { return recorder.inbound_count() == 1; }));
    const auto inbound = recorder.inbound_at(0);

    std::string text = "# 报告\n\n";
    for (int i = 0; i < 80; ++i) text += "第 " + std::to_string(i) + " 段:" + std::string(60, 'x') + "\n\n";
    const auto result = transport.send_text(inbound.address, text, inbound.reply_context);
    EXPECT_EQ(result.outcome, SendOutcome::Sent) << result.error;
    const auto sends = server.sends();
    ASSERT_GE(sends.size(), 2u);
    EXPECT_EQ(sends[0].path, "/open-apis/im/v1/messages/om_1/reply");
    EXPECT_EQ(sends[0].body.value("msg_type", ""), "post");
    EXPECT_FALSE(sends[0].body.value("uuid", "").empty());
    const auto content = content_of(sends[0]);
    EXPECT_EQ(content["zh_cn"]["content"][0][0]["tag"], "md");
    EXPECT_NE(content["zh_cn"]["content"][0][0]["text"].get<std::string>().find("# 报告"), std::string::npos);
    for (std::size_t i = 1; i < sends.size(); ++i) {
        EXPECT_EQ(sends[i].path, "/open-apis/im/v1/messages");
        EXPECT_EQ(sends[i].query, "chat_id");
        EXPECT_EQ(sends[i].body.value("receive_id", ""), "oc_p2p");
        EXPECT_EQ(sends[i].body.value("msg_type", ""), "post");
    }
    EXPECT_EQ(sends[0].authorization, "Bearer t-1");
}

// 场景:平台拒收 post(230001,"content format of the post type is incorrect")。
// 期望:同一段改为纯文本(去掉 Markdown 标记)重发成功,结果为已发送;不重试同一个 post。
TEST(FeishuTransport, FallsBackToPlainTextWhenPostIsRejected) {
    FakeFeishuServer server;
    server.message_handler = [](const FakeFeishuServer::Request& r) {
        if (r.body.value("msg_type", "") == "post")
            return std::make_pair(400, nlohmann::json{{"code", 230001},
                                                      {"msg", "The content format of the post type is incorrect"}});
        return std::make_pair(200, nlohmann::json{{"code", 0}, {"data", {{"message_id", "om_r"}}}});
    };
    FeishuTransport transport(local_options(server));
    const Address dm{"feishu", "cli_app", ChatKind::Private, "ou_user", "ou_user", ""};
    const auto result = transport.send_text(dm, "**粗体**", nlohmann::json::object());
    EXPECT_EQ(result.outcome, SendOutcome::Sent) << result.error;
    const auto sends = server.sends();
    ASSERT_EQ(sends.size(), 2u);
    EXPECT_EQ(sends[0].body.value("msg_type", ""), "post");
    EXPECT_EQ(sends[1].body.value("msg_type", ""), "text");
    EXPECT_EQ(content_of(sends[1]).value("text", ""), "粗体");
}

// 场景:Desktop 里输入的回复,没有回复上下文;分别发往群与私聊。
// 期望:群按 receive_id_type=chat_id 发到群 id,私聊按 open_id 发到对方;普通文字用 text 类型。
TEST(FeishuTransport, SendsToAddressWithoutReplyContext) {
    FakeFeishuServer server;
    FeishuTransport transport(local_options(server));
    const Address group{"feishu", "cli_app", ChatKind::Group, "oc_group", "ou_user", ""};
    const Address dm{"feishu", "cli_app", ChatKind::Private, "ou_user", "ou_user", ""};
    EXPECT_EQ(transport.send_text(group, "hello", nlohmann::json::object()).outcome, SendOutcome::Sent);
    EXPECT_EQ(transport.send_text(dm, "hi", nullptr).outcome, SendOutcome::Sent);
    const auto sends = server.sends();
    ASSERT_EQ(sends.size(), 2u);
    EXPECT_EQ(sends[0].query, "chat_id");
    EXPECT_EQ(sends[0].body.value("receive_id", ""), "oc_group");
    EXPECT_EQ(sends[0].body.value("msg_type", ""), "text");
    EXPECT_EQ(content_of(sends[0]).value("text", ""), "hello");
    EXPECT_EQ(sends[1].query, "open_id");
    EXPECT_EQ(sends[1].body.value("receive_id", ""), "ou_user");
}

// 场景:第一次发送被限流(HTTP 429 + 99991400),第二次成功。
// 期望:等待后重发同一请求(uuid 不变,平台据此去重),结果为已发送;消息不丢。
TEST(FeishuTransport, RetriesRateLimitedSendsWithSameUuid) {
    FakeFeishuServer server;
    std::atomic<int> calls{0};
    server.message_handler = [&calls](const FakeFeishuServer::Request&) {
        if (calls++ == 0)
            return std::make_pair(429, nlohmann::json{{"code", 99991400}, {"msg", "request trigger frequency limit"}});
        return std::make_pair(200, nlohmann::json{{"code", 0}, {"data", {{"message_id", "om_r"}}}});
    };
    FeishuTransport transport(local_options(server));
    const Address group{"feishu", "cli_app", ChatKind::Group, "oc_group", "ou_user", ""};
    EXPECT_EQ(transport.send_text(group, "排队的消息", nullptr).outcome, SendOutcome::Sent);
    const auto sends = server.sends();
    ASSERT_EQ(sends.size(), 2u);
    EXPECT_EQ(sends[0].body.value("uuid", "a"), sends[1].body.value("uuid", "b"));
}

// 场景:要回复的那条消息已被撤回(230011)。
// 期望:改为按 chat_id 直接发到会话,结果为已发送。
TEST(FeishuTransport, FallsBackToCreateWhenReplyTargetIsGone) {
    FakeFeishuServer server;
    server.message_handler = [](const FakeFeishuServer::Request& r) {
        if (r.path.find("/reply") != std::string::npos)
            return std::make_pair(400, nlohmann::json{{"code", 230011}, {"msg", "message recalled"}});
        return std::make_pair(200, nlohmann::json{{"code", 0}, {"data", {{"message_id", "om_r"}}}});
    };
    FeishuTransport transport(local_options(server));
    const Address group{"feishu", "cli_app", ChatKind::Group, "oc_group", "ou_user", ""};
    const auto result = transport.send_text(
        group, "结果", {{"message_id", "om_x"}, {"chat_id", "oc_group"}, {"chat_type", "group"}});
    EXPECT_EQ(result.outcome, SendOutcome::Sent) << result.error;
    const auto sends = server.sends();
    ASSERT_EQ(sends.size(), 2u);
    EXPECT_EQ(sends[0].path, "/open-apis/im/v1/messages/om_x/reply");
    EXPECT_EQ(sends[1].path, "/open-apis/im/v1/messages");
    EXPECT_EQ(sends[1].body.value("receive_id", ""), "oc_group");
}

// 场景:平台明确拒绝(对方不在应用可用范围内,230013)。
// 期望:不重试,结果为失败并给出中文原因。
TEST(FeishuTransport, ReportsPermanentSendErrors) {
    FakeFeishuServer server;
    server.message_handler = [](const FakeFeishuServer::Request&) {
        return std::make_pair(400, nlohmann::json{{"code", 230013}, {"msg", "Bot has NO availability to this user."}});
    };
    FeishuTransport transport(local_options(server));
    const Address dm{"feishu", "cli_app", ChatKind::Private, "ou_user", "ou_user", ""};
    const auto result = transport.send_text(dm, "hi", nullptr);
    EXPECT_EQ(result.outcome, SendOutcome::Failed);
    EXPECT_NE(result.error.find("可用范围"), std::string::npos) << result.error;
    EXPECT_EQ(server.sends().size(), 1u);
}

// 场景:服务端主动关闭长连接。
// 期望:重新请求长连接地址(旧地址不复用)并重连,回到已连接;新连接上的消息照常收到。
TEST(FeishuTransport, ReconnectsWithFreshEndpointAfterServerClose) {
    FakeFeishuServer server;
    Recorder recorder;  // 停机仍会回调:记录器必须晚于传输对象析构
    FeishuTransport transport(local_options(server));
    transport.start(recorder.callbacks());
    ASSERT_TRUE(wait_connected(transport, server));
    server.close_connection(1000);
    ASSERT_TRUE(FakeFeishuServer::wait_until([&] { return server.connections() == 2; }));
    ASSERT_TRUE(wait_connected(transport, server, 2));
    EXPECT_EQ(server.endpoint_calls(), 2);
    server.push_event(p2p_text("om_after", "又连上了"));
    ASSERT_TRUE(FakeFeishuServer::wait_until([&] { return recorder.inbound_count() == 1; }));
}

// 场景:长连接地址接口第一次 HTTP 500(平台繁忙),第二次正常。
// 期望:按退避重试,最终连上;共请求两次地址。
TEST(FeishuTransport, RetriesTransientEndpointFailures) {
    FakeFeishuServer server;
    std::atomic<int> calls{0};
    server.endpoint_handler = [&calls, &server](const FakeFeishuServer::Request&) {
        if (calls++ == 0) return std::make_pair(500, nlohmann::json{{"msg", "system busy"}});
        return std::make_pair(200, nlohmann::json{{"code", 0},
                                                  {"data", {{"URL", server.ws_url()},
                                                            {"ClientConfig", server.client_config}}}});
    };
    Recorder recorder;  // 停机仍会回调:记录器必须晚于传输对象析构
    FeishuTransport transport(local_options(server));
    transport.start(recorder.callbacks());
    ASSERT_TRUE(wait_connected(transport, server));
    EXPECT_EQ(server.endpoint_calls(), 2);
    bool retried = false;
    for (const auto& s : recorder.all_statuses()) retried = retried || s.state == LinkState::Retrying;
    EXPECT_TRUE(retried);
}

// 场景:长连接地址接口以 1000040345 拒绝(App ID / App Secret 无效,或选错了飞书 / Lark)。
// 期望:状态为失败且停止重试,原因是中文且不含密钥;之后不再请求地址、不建立连接。
TEST(FeishuTransport, EndpointCredentialErrorStopsRetrying) {
    FakeFeishuServer server;
    server.endpoint_handler = [](const FakeFeishuServer::Request&) {
        return std::make_pair(200, nlohmann::json{{"code", 1000040345}, {"msg", "app_id or app_secret is invalid"},
                                                  {"data", {{"URL", ""}}}});
    };
    Recorder recorder;  // 停机仍会回调:记录器必须晚于传输对象析构
    FeishuTransport transport(local_options(server));
    transport.start(recorder.callbacks());
    ASSERT_TRUE(FakeFeishuServer::wait_until([&] { return transport.status().state == LinkState::Failed; }));
    const auto status = transport.status();
    EXPECT_TRUE(status.retry_stopped);
    EXPECT_NE(status.detail.find("App Secret"), std::string::npos) << status.detail;
    EXPECT_EQ(status.detail.find("secret-value-123"), std::string::npos);
    std::this_thread::sleep_for(milliseconds(300));
    EXPECT_EQ(server.endpoint_calls(), 1);
    EXPECT_EQ(server.connections(), 0);
}

// 场景:换令牌接口返回 10014(App ID 不存在,常见于把飞书应用配成了 Lark)。
// 期望:不请求长连接地址,状态直接为失败且停止重试,提示确认飞书 / Lark。
TEST(FeishuTransport, InvalidAppIdStopsRetrying) {
    FakeFeishuServer server;
    server.token_handler = [](const FakeFeishuServer::Request&) {
        return std::make_pair(200, nlohmann::json{{"code", 10014}, {"msg", "app id not exists"}});
    };
    Recorder recorder;  // 停机仍会回调:记录器必须晚于传输对象析构
    FeishuTransport transport(local_options(server));
    transport.start(recorder.callbacks());
    ASSERT_TRUE(FakeFeishuServer::wait_until([&] { return transport.status().state == LinkState::Failed; }));
    EXPECT_TRUE(transport.status().retry_stopped);
    EXPECT_NE(transport.status().detail.find("Lark"), std::string::npos);
    EXPECT_EQ(server.endpoint_calls(), 0);
}

// 场景:回传一张 PNG 与一个中文名的 PDF。
// 期望:图片先走图片上传(image_type=message)再以 image 消息发送;PDF 走文件上传
// (file_type=pdf,file_name 字段保留中文原名)再以 file 消息发送。
TEST(FeishuTransport, SendsImagesAndFiles) {
    FakeFeishuServer server;
    FeishuTransport transport(local_options(server));
    const Address dm{"feishu", "cli_app", ChatKind::Private, "ou_user", "ou_user", ""};
    const auto png = temp_file("acecode-feishu-pic.png", "PNGDATA");
    const auto pdf = temp_file("acecode-feishu-report.pdf", "PDFDATA");
    EXPECT_EQ(transport.send_file(dm, png, "pic.png", "image/png", nullptr).outcome, SendOutcome::Sent);
    EXPECT_EQ(transport.send_file(dm, pdf, "报告.pdf", "application/pdf", nullptr).outcome, SendOutcome::Sent);

    const auto images = server.requests_to("/open-apis/im/v1/images");
    ASSERT_EQ(images.size(), 1u);
    EXPECT_NE(images[0].raw.find("image_type"), std::string::npos);
    EXPECT_NE(images[0].raw.find("PNGDATA"), std::string::npos);
    const auto files = server.requests_to("/open-apis/im/v1/files");
    ASSERT_EQ(files.size(), 1u);
    EXPECT_NE(files[0].raw.find("file_type"), std::string::npos);
    EXPECT_NE(files[0].raw.find("报告.pdf"), std::string::npos);

    const auto sends = server.sends();
    ASSERT_EQ(sends.size(), 2u);
    EXPECT_EQ(sends[0].body.value("msg_type", ""), "image");
    EXPECT_EQ(content_of(sends[0]).value("image_key", ""), "img_v3_up");
    EXPECT_EQ(sends[1].body.value("msg_type", ""), "file");
    EXPECT_EQ(content_of(sends[1]).value("file_key", ""), "file_v3_up");
    std::filesystem::remove(png);
    std::filesystem::remove(pdf);
}

// 场景:要回传的文件超过上传上限(测试里把上限调到 4 字节),以及空文件。
// 期望:超限时不上传,改发一条带文件名的文字说明;空文件直接失败并说明原因。
TEST(FeishuTransport, OversizedFileBecomesTextNotice) {
    FakeFeishuServer server;
    auto options = local_options(server);
    options.max_upload_bytes = 4;
    FeishuTransport transport(options);
    const Address dm{"feishu", "cli_app", ChatKind::Private, "ou_user", "ou_user", ""};
    const auto big = temp_file("acecode-feishu-big.pdf", "PDF-DATA");
    EXPECT_EQ(transport.send_file(dm, big, "big.pdf", "application/pdf", nullptr).outcome, SendOutcome::Sent);
    EXPECT_TRUE(server.requests_to("/open-apis/im/v1/files").empty());
    const auto sends = server.sends();
    ASSERT_EQ(sends.size(), 1u);
    EXPECT_NE(sends[0].body.dump().find("big.pdf"), std::string::npos);
    const auto empty = temp_file("acecode-feishu-empty.txt", "");
    EXPECT_EQ(transport.send_file(dm, empty, "empty.txt", "text/plain", nullptr).outcome, SendOutcome::Failed);
    std::filesystem::remove(big);
    std::filesystem::remove(empty);
}

// 场景:收到一条图片消息,核心随后下载附件;另有一个附件超过本地下载上限。
// 期望:入站附件为图片;按“消息 id + image_key + type=image”下载成功;超限的直接失败并给出中文原因。
TEST(FeishuTransport, DownloadsInboundAttachments) {
    FakeFeishuServer server;
    auto options = local_options(server);
    options.max_download_bytes = 1024;
    Recorder recorder;  // 停机仍会回调:记录器必须晚于传输对象析构
    FeishuTransport transport(options);
    transport.start(recorder.callbacks());
    ASSERT_TRUE(wait_connected(transport, server));
    server.push_event(FakeFeishuServer::message_event("om_img", "p2p", "oc_p2p", "ou_user", "image",
                                                      {{"image_key", "img_k"}}));
    ASSERT_TRUE(FakeFeishuServer::wait_until([&] { return recorder.inbound_count() == 1; }));
    const auto inbound = recorder.inbound_at(0);
    ASSERT_EQ(inbound.attachments.size(), 1u);
    EXPECT_EQ(inbound.attachments[0].kind, AttachmentKind::Image);
    const auto dest = std::filesystem::path(testing::TempDir()) / "acecode-feishu-in.bin";
    std::string error;
    ASSERT_TRUE(transport.download(inbound.attachments[0], dest, &error)) << error;
    EXPECT_EQ(std::filesystem::file_size(dest), 16u);
    const auto downloads = server.requests_to("/open-apis/im/v1/messages/om_img/resources/img_k");
    ASSERT_EQ(downloads.size(), 1u);
    EXPECT_EQ(downloads[0].query, "image");

    Attachment big = inbound.attachments[0];
    big.remote_ref = make_resource_ref("om_img", "big", "file");
    EXPECT_FALSE(transport.download(big, dest, &error));
    EXPECT_NE(error.find("超过"), std::string::npos) << error;
    Attachment broken;
    broken.remote_ref = "not-a-ref";
    EXPECT_FALSE(transport.download(broken, dest, &error));
    std::filesystem::remove(dest);
}

// 场景:用户关闭通道。
// 期望:stop() 返回前向服务端发出 close 帧(状态码 1000)。回归意义:不发 close 帧时飞书会把
// 连接当作仍在线,继续把消息随机分给这条死连接,通道整段时间“收不到消息”(hermes #10202)。
TEST(FeishuTransport, StopSendsCloseFrame) {
    FakeFeishuServer server;
    Recorder recorder;  // 停机仍会回调:记录器必须晚于传输对象析构
    FeishuTransport transport(local_options(server));
    transport.start(recorder.callbacks());
    ASSERT_TRUE(wait_connected(transport, server));
    transport.stop();
    ASSERT_TRUE(FakeFeishuServer::wait_until([&] { return !server.close_codes().empty(); }));
    EXPECT_EQ(server.close_codes()[0], 1000);
}

} // namespace
} // namespace acecode::im::feishu
