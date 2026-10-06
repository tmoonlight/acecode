#include <gtest/gtest.h>

#include "im/dingtalk/dingtalk_transport.hpp"
#include "test_support/im/fake_dingtalk_server.hpp"

#include <fstream>
#include <functional>
#include <mutex>
#include <vector>

// im/dingtalk/dingtalk_transport:钉钉传输层的发送端到端测试。覆盖会话 webhook 回复、
// webhook 失效 / 过期后改走机器人 OpenAPI、Desktop 里输入的回复(没有回复上下文)、
// 组织外用户与 OpenAPI 被拒时的暂存补发、限流等待重发、长文分段与文件 / 图片发送。

namespace acecode::im::dingtalk {
namespace {

using Server = test::FakeDingTalkServer;

DingTalkTransportOptions local_options(Server& server) {
    DingTalkTransportOptions options;
    options.api.client_id = server.client_id;
    options.api.client_secret = server.client_secret;
    options.api.api_base = server.base();
    options.api.oapi_base = server.base();
    options.api.use_proxy = false;
    options.backoff_base = std::chrono::milliseconds(50);
    options.backoff_cap = std::chrono::milliseconds(200);
    options.jitter = false;
    options.idle_timeout = std::chrono::milliseconds(0);
    options.throttle_delay = std::chrono::milliseconds(100);
    options.qps_delay = std::chrono::milliseconds(50);
    return options;
}

struct Recorder {
    std::mutex mu;
    std::vector<Inbound> inbound;
    std::function<void(const Inbound&)> hook;  // 在传输层分发线程上、记录之前调用
    TransportCallbacks callbacks() {
        TransportCallbacks cb;
        cb.on_inbound = [this](Inbound in) {
            if (hook) hook(in);
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

// 启动传输层并等到假网关登记了连接。
void start(DingTalkTransport& transport, Recorder& recorder, Server& server) {
    transport.start(recorder.callbacks());
    ASSERT_TRUE(Server::wait_until([&transport, &server] {
        return transport.status().state == LinkState::Connected && server.connections() >= 1;
    }));
}

// 推一条消息并等它到达上层,返回入站消息。
Inbound receive(Server& server, Recorder& recorder, const std::string& header_id, const nlohmann::json& data) {
    const auto before = recorder.count();
    server.push_callback(header_id, data);
    EXPECT_TRUE(Server::wait_until([&recorder, before] { return recorder.count() > before; }));
    return recorder.at(recorder.count() - 1);
}

nlohmann::json msg_param(const Server::Request& request) {
    return nlohmann::json::parse(request.body.value("msgParam", std::string("{}")));
}

// 场景:收到单聊消息后回复一段带编号列表的 Markdown。
// 期望:经该消息的会话 webhook 以 markdown 发送,标题取第一行;编号列表前补了空行(钉钉渲染怪癖);
// 不调用机器人 OpenAPI(不消耗月度额度)。
TEST(DingTalkSend, RepliesThroughSessionWebhook) {
    Server server;
    Recorder recorder;
    DingTalkTransport transport(local_options(server));
    start(transport, recorder, server);
    const auto inbound = receive(server, recorder, "h1", server.private_text("m1", "staff01", "hi"));
    const auto result = transport.send_text(inbound.address, u8"## 结果\n步骤:\n1. 构建\n2. 测试", inbound.reply_context);
    EXPECT_EQ(result.outcome, SendOutcome::Sent) << result.error;
    const auto sent = server.requests_to("/robot/sendBySession");
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0].query, "S1");
    EXPECT_EQ(sent[0].body.value("msgtype", ""), "markdown");
    EXPECT_EQ(sent[0].body["markdown"].value("title", ""), u8"结果");
    EXPECT_EQ(sent[0].body["markdown"].value("text", ""), u8"## 结果\n步骤:\n\n1. 构建\n2. 测试");
    EXPECT_TRUE(server.requests_to("/v1.0/robot/oToMessages/batchSend").empty());
}

// 场景:会话 webhook 报 errcode 300001(session 不存在),随后再回复一条。
// 期望:改走机器人单聊 OpenAPI(userIds=[staffId]、robotCode=Client ID、msgKey=sampleMarkdown、
// msgParam 为 JSON 字符串)且发送成功;第二条不再尝试那个已失效的 webhook。
TEST(DingTalkSend, FallsBackToRobotApiWhenWebhookIsGone) {
    Server server;
    server.webhook_handler = [](const Server::Request&) {
        return Server::Reply{200, {{"errcode", 300001}, {"errmsg", "session 不存在"}}};
    };
    Recorder recorder;
    DingTalkTransport transport(local_options(server));
    start(transport, recorder, server);
    const auto inbound = receive(server, recorder, "h1", server.private_text("m1", "staff01", "hi"));
    EXPECT_EQ(transport.send_text(inbound.address, "first", inbound.reply_context).outcome, SendOutcome::Sent);
    EXPECT_EQ(transport.send_text(inbound.address, "second", inbound.reply_context).outcome, SendOutcome::Sent);
    EXPECT_EQ(server.requests_to("/robot/sendBySession").size(), 1u);
    const auto oto = server.requests_to("/v1.0/robot/oToMessages/batchSend");
    ASSERT_EQ(oto.size(), 2u);
    EXPECT_EQ(oto[0].body["userIds"], nlohmann::json::array({"staff01"}));
    EXPECT_EQ(oto[0].body.value("robotCode", ""), server.client_id);
    EXPECT_EQ(oto[0].body.value("msgKey", ""), "sampleMarkdown");
    EXPECT_EQ(msg_param(oto[0]).value("text", ""), "first");
    EXPECT_EQ(msg_param(oto[1]).value("text", ""), "second");
}

// 场景:触发回复的消息带的 webhook 只剩 2 分钟有效(长回合之后常见)。
// 期望:提前 5 分钟视为过期,直接走 OpenAPI,不去撞一个可能在发送途中过期的 webhook。
TEST(DingTalkSend, NearlyExpiredWebhookIsSkipped) {
    Server server;
    Recorder recorder;
    DingTalkTransport transport(local_options(server));
    start(transport, recorder, server);
    const auto data = server.private_text("m1", "staff01", "hi", "S1", Server::now_ms() + 2 * 60 * 1000);
    const auto inbound = receive(server, recorder, "h1", data);
    EXPECT_EQ(transport.send_text(inbound.address, "late", inbound.reply_context).outcome, SendOutcome::Sent);
    EXPECT_TRUE(server.requests_to("/robot/sendBySession").empty());
    EXPECT_EQ(server.requests_to("/v1.0/robot/oToMessages/batchSend").size(), 1u);
}

// 场景:重启后在 Desktop 里输入的回复 —— 没有回复上下文,也没有缓存的 webhook。
// 期望:群会话走 groupMessages/send(openConversationId = conversationId);
// 组织内单聊走 oToMessages/batchSend(userIds = 地址里的 staffId)。
TEST(DingTalkSend, DesktopOutputWithoutContextUsesRobotApi) {
    Server server;
    DingTalkTransport transport(local_options(server));
    Address group{"dingtalk", server.client_id, ChatKind::Group, "cidG+/==", "staff09", {}};
    EXPECT_EQ(transport.send_text(group, "hello group", nlohmann::json::object()).outcome, SendOutcome::Sent);
    const auto sent = server.requests_to("/v1.0/robot/groupMessages/send");
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0].body.value("openConversationId", ""), "cidG+/==");
    EXPECT_EQ(msg_param(sent[0]).value("text", ""), "hello group");

    Address user{"dingtalk", server.client_id, ChatKind::Private, "staff05", "staff05", {}};
    EXPECT_EQ(transport.send_text(user, "hello", nlohmann::json::object()).outcome, SendOutcome::Sent);
    const auto oto = server.requests_to("/v1.0/robot/oToMessages/batchSend");
    ASSERT_EQ(oto.size(), 1u);
    EXPECT_EQ(oto[0].body["userIds"], nlohmann::json::array({"staff05"}));
}

// 场景:组织外用户(没有 staffId)发来消息,回复时 webhook 已过期;之后对方又发来一条。
// 期望:第一次发送结果为“暂存”,不调用任何发送接口;新消息到来时先用它的新 webhook 补发
// (带“补发”标记),然后才把新消息交给上层。
TEST(DingTalkSend, ExternalUserOutputIsHeldUntilNextMessage) {
    Server server;
    Recorder recorder;
    std::atomic<std::size_t> webhooks_before_second{0};
    recorder.hook = [&server, &webhooks_before_second](const Inbound& in) {
        if (in.message_id == "m2") webhooks_before_second = server.requests_to("/robot/sendBySession").size();
    };
    DingTalkTransport transport(local_options(server));
    start(transport, recorder, server);
    const auto first = receive(server, recorder, "h1",
                               server.private_text("m1", "", "hi", "S1", Server::now_ms() - 1000));
    EXPECT_EQ(first.address.chat, "$:LWCP_v1:$ext");
    EXPECT_EQ(transport.send_text(first.address, "late answer", first.reply_context).outcome, SendOutcome::Held);
    EXPECT_EQ(transport.held_count(), 1u);
    EXPECT_TRUE(server.requests_to("/robot/sendBySession").empty());
    EXPECT_TRUE(server.requests_to("/v1.0/robot/oToMessages/batchSend").empty());

    receive(server, recorder, "h2", server.private_text("m2", "", "again", "S2"));
    EXPECT_EQ(webhooks_before_second.load(), 1u);
    const auto sent = server.requests_to("/robot/sendBySession");
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0].query, "S2");
    const auto text = sent[0].body["markdown"].value("text", "");
    EXPECT_EQ(text.rfind(u8"(补发)", 0), 0u) << text;
    EXPECT_NE(text.find("late answer"), std::string::npos);
    EXPECT_EQ(transport.held_count(), 0u);
}

// 场景:应用没开通机器人发消息权限:webhook 已失效,OpenAPI 返回 403 AccessTokenPermissionDenied;
// 之后用户再发一条消息(带新 webhook)。
// 期望:发送结果为暂存,原因提示开通 qyapi_robot_sendmsg;新消息到来时经新 webhook 补发成功。
TEST(DingTalkSend, RefusedRobotApiOutputIsHeldAndFlushed) {
    Server server;
    server.webhook_handler = [](const Server::Request& r) {
        if (r.query == "S1") return Server::Reply{200, {{"errcode", 300001}, {"errmsg", "session 不存在"}}};
        return Server::Reply{0, nullptr};
    };
    server.oto_handler = [](const Server::Request&) {
        return Server::Reply{403, {{"code", "Forbidden.AccessDenied.AccessTokenPermissionDenied"},
                                   {"message", "没有调用该接口的权限"}}};
    };
    Recorder recorder;
    DingTalkTransport transport(local_options(server));
    start(transport, recorder, server);
    const auto first = receive(server, recorder, "h1", server.private_text("m1", "staff01", "hi"));
    const auto result = transport.send_text(first.address, "answer", first.reply_context);
    EXPECT_EQ(result.outcome, SendOutcome::Held);
    EXPECT_NE(result.error.find("qyapi_robot_sendmsg"), std::string::npos) << result.error;

    receive(server, recorder, "h2", server.private_text("m2", "staff01", "again", "S2"));
    const auto sent = server.requests_to("/robot/sendBySession");
    ASSERT_EQ(sent.size(), 2u);
    EXPECT_EQ(sent[1].query, "S2");
    EXPECT_NE(sent[1].body["markdown"].value("text", "").find("answer"), std::string::npos);
    EXPECT_EQ(transport.held_count(), 0u);
}

// 场景:webhook 第一次返回 errcode 130101(发送过快),第二次正常;群发接口第一次返回 QPS 超限。
// 期望:等待后重发同一条内容并成功,不丢消息;两次请求内容完全相同;webhook 重发至少等了限流延迟。
TEST(DingTalkSend, RetriesThrottledSendsWithoutLosingThem) {
    Server server;
    std::atomic<int> webhook_calls{0}, group_calls{0};
    server.webhook_handler = [&webhook_calls](const Server::Request&) {
        if (webhook_calls++ == 0) return Server::Reply{200, {{"errcode", 130101}, {"errmsg", "send too fast"}}};
        return Server::Reply{0, nullptr};
    };
    server.group_handler = [&group_calls](const Server::Request&) {
        if (group_calls++ == 0)
            return Server::Reply{403, {{"code", "Forbidden.AccessDenied.QpsLimitForApi"}, {"message", "qps"}}};
        return Server::Reply{0, nullptr};
    };
    Recorder recorder;
    DingTalkTransport transport(local_options(server));
    start(transport, recorder, server);
    const auto inbound = receive(server, recorder, "h1", server.private_text("m1", "staff01", "hi"));
    const auto t0 = std::chrono::steady_clock::now();
    EXPECT_EQ(transport.send_text(inbound.address, "once", inbound.reply_context).outcome, SendOutcome::Sent);
    EXPECT_GE(std::chrono::steady_clock::now() - t0, std::chrono::milliseconds(100));
    const auto sent = server.requests_to("/robot/sendBySession");
    ASSERT_EQ(sent.size(), 2u);
    EXPECT_EQ(sent[0].body, sent[1].body);

    Address group{"dingtalk", server.client_id, ChatKind::Group, "cidG", "staff09", {}};
    EXPECT_EQ(transport.send_text(group, "group once", nlohmann::json::object()).outcome, SendOutcome::Sent);
    EXPECT_EQ(server.requests_to("/v1.0/robot/groupMessages/send").size(), 2u);
}

// 场景:回复一段约 6000 字的长文(三段,每段 2000 字)。
// 期望:按段落切成多条依次发送,每条不超过 3500 个字符,顺序与原文一致。
TEST(DingTalkSend, SplitsLongTextInOrder) {
    Server server;
    Recorder recorder;
    DingTalkTransport transport(local_options(server));
    start(transport, recorder, server);
    const auto inbound = receive(server, recorder, "h1", server.private_text("m1", "staff01", "hi"));
    std::string text;
    for (const char* mark : {u8"甲", u8"乙", u8"丙"}) {
        if (!text.empty()) text += "\n\n";
        for (int i = 0; i < 2000; ++i) text += mark;
    }
    EXPECT_EQ(transport.send_text(inbound.address, text, inbound.reply_context).outcome, SendOutcome::Sent);
    const auto sent = server.requests_to("/robot/sendBySession");
    ASSERT_GE(sent.size(), 2u);
    std::string joined;
    for (const auto& r : sent) {
        const auto part = r.body["markdown"].value("text", "");
        EXPECT_LE(part.size() / 3, kMaxTextChars);
        joined += part;
    }
    EXPECT_LT(joined.find(u8"甲"), joined.find(u8"乙"));
    EXPECT_LT(joined.find(u8"乙"), joined.find(u8"丙"));
}

// 场景:回传一张会话生成的 PNG 图片。
// 期望:按 type=image 上传拿到 mediaId,再以 Markdown 图片嵌入经 webhook 发送(不消耗 OpenAPI 额度)。
TEST(DingTalkSend, SendsImagesAsEmbeddedMarkdown) {
    Server server;
    Recorder recorder;
    DingTalkTransport transport(local_options(server));
    start(transport, recorder, server);
    const auto inbound = receive(server, recorder, "h1", server.private_text("m1", "staff01", "chart"));
    const auto file = std::filesystem::path(testing::TempDir()) / "acecode-dingtalk-chart.png";
    { std::ofstream(file, std::ios::binary) << "PNGDATA"; }
    const auto result = transport.send_file(inbound.address, file, "chart.png", "image/png", inbound.reply_context);
    EXPECT_EQ(result.outcome, SendOutcome::Sent) << result.error;
    const auto uploads = server.requests_to("/media/upload");
    ASSERT_EQ(uploads.size(), 1u);
    EXPECT_EQ(uploads[0].query, "image");
    const auto sent = server.requests_to("/robot/sendBySession");
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0].body["markdown"].value("text", ""), "![chart.png](@media-1)");
    std::filesystem::remove(file);
}

// 场景:在群里回传一个 PDF 报告。
// 期望:按 type=file 上传,再经 groupMessages/send 以 sampleFile 发送(mediaId、原文件名、扩展名)。
TEST(DingTalkSend, SendsDocumentsThroughRobotApi) {
    Server server;
    Recorder recorder;
    DingTalkTransport transport(local_options(server));
    start(transport, recorder, server);
    const auto inbound = receive(server, recorder, "h1", server.group_text("g1", "cidG", "staff02", "report"));
    const auto file = std::filesystem::path(testing::TempDir()) / "acecode-dingtalk-report.pdf";
    { std::ofstream(file, std::ios::binary) << "PDF-DATA"; }
    const auto result =
        transport.send_file(inbound.address, file, u8"周报.pdf", "application/pdf", inbound.reply_context);
    EXPECT_EQ(result.outcome, SendOutcome::Sent) << result.error;
    const auto uploads = server.requests_to("/media/upload");
    ASSERT_EQ(uploads.size(), 1u);
    EXPECT_EQ(uploads[0].query, "file");
    const auto sent = server.requests_to("/v1.0/robot/groupMessages/send");
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0].body.value("msgKey", ""), "sampleFile");
    const auto param = msg_param(sent[0]);
    EXPECT_EQ(param.value("mediaId", ""), "@media-1");
    EXPECT_EQ(param.value("fileName", ""), u8"周报.pdf");
    EXPECT_EQ(param.value("fileType", ""), "pdf");
    std::filesystem::remove(file);
}

// 场景:文件超过上传上限(测试里调到 4 字节);以及要把文件发给组织外用户。
// 期望:两种情况都不上传,改发一条带文件名的文字说明,发送结果为已发送。
TEST(DingTalkSend, UndeliverableFilesBecomeTextNotices) {
    Server server;
    Recorder recorder;
    auto options = local_options(server);
    options.max_upload_bytes = 4;
    DingTalkTransport transport(options);
    start(transport, recorder, server);
    const auto inbound = receive(server, recorder, "h1", server.private_text("m1", "staff01", "big"));
    const auto file = std::filesystem::path(testing::TempDir()) / "acecode-dingtalk-big.pdf";
    { std::ofstream(file, std::ios::binary) << "PDF-DATA"; }
    EXPECT_EQ(transport.send_file(inbound.address, file, "big.pdf", "application/pdf", inbound.reply_context).outcome,
              SendOutcome::Sent);
    const auto external = receive(server, recorder, "h2", server.private_text("m2", "", "ext", "S2"));
    std::filesystem::resize_file(file, 2);
    EXPECT_EQ(transport.send_file(external.address, file, "small.pdf", "application/pdf", external.reply_context).outcome,
              SendOutcome::Sent);
    EXPECT_TRUE(server.requests_to("/media/upload").empty());
    const auto sent = server.requests_to("/robot/sendBySession");
    ASSERT_EQ(sent.size(), 2u);
    EXPECT_NE(sent[0].body["markdown"].value("text", "").find("big.pdf"), std::string::npos);
    EXPECT_NE(sent[1].body["markdown"].value("text", "").find(u8"组织外"), std::string::npos);
    std::filesystem::remove(file);
}

} // namespace
} // namespace acecode::im::dingtalk
