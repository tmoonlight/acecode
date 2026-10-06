#include <gtest/gtest.h>

#include "im/qqbot/qq_transport.hpp"
#include "test_support/im/fake_qq_server.hpp"

#include <fstream>
#include <mutex>
#include <vector>

// im/qqbot/qq_transport:QQ 传输层端到端测试。本机假开放平台提供令牌、/gateway、
// WebSocket 网关与发消息接口;测试覆盖连接、收消息、被动/主动/暂存三级发送、
// Markdown 回退、去重、致命关闭与凭据错误。

namespace acecode::im::qqbot {
namespace {

QqTransportOptions local_options(const test::FakeQqServer& server) {
    QqTransportOptions options;
    options.api.app_id = "APP";
    options.api.app_secret = "secret-value-123";
    options.api.api_base = server.base();
    options.api.token_url = server.base() + "/app/getAppAccessToken";
    options.api.use_proxy = false;
    options.backoff = {std::chrono::milliseconds(50)};
    options.rate_limit_delay = std::chrono::milliseconds(100);
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
};

bool wait_connected(QqTransport& transport) {
    return test::FakeQqServer::wait_until([&] { return transport.status().state == LinkState::Connected; });
}

// 场景:开启 QQ 通道,网关握手后推来一条单聊消息,随后用它回复一段 Markdown。
// 期望:状态变为已连接并显示机器人名;入站消息带正确地址与回复上下文;
// 回复以 Markdown 被动回复送达(带 msg_id 与 msg_seq),结果为已发送。
TEST(QqTransport, ConnectsReceivesAndRepliesPassively) {
    test::FakeQqServer server;
    Recorder recorder;  // 停机仍会回调:记录器必须晚于传输对象析构
    QqTransport transport(local_options(server));
    transport.start(recorder.callbacks());
    ASSERT_TRUE(wait_connected(transport));
    EXPECT_EQ(transport.status().display_name, "TestBot");
    const auto identify = server.frames().front();
    EXPECT_EQ(identify.value("op", 0), 2);
    EXPECT_EQ(identify["d"]["token"], "QQBot tok-1");

    server.push_c2c("M1", "U1", " 你好 ");
    ASSERT_TRUE(test::FakeQqServer::wait_until([&] { return recorder.inbound_count() == 1; }));
    const auto inbound = recorder.inbound_at(0);
    EXPECT_EQ(inbound.text, u8"你好");
    EXPECT_EQ(inbound.address.chat, "U1");

    const auto result = transport.send_text(inbound.address, "**好的**", inbound.reply_context);
    EXPECT_EQ(result.outcome, SendOutcome::Sent) << result.error;
    const auto sent = server.requests_to("/v2/users/U1/messages");
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0].body.value("msg_type", -1), kMsgTypeMarkdown);
    EXPECT_EQ(sent[0].body["markdown"].value("content", ""), "**好的**");
    EXPECT_EQ(sent[0].body.value("msg_id", ""), "M1");
    EXPECT_GE(sent[0].body.value("msg_seq", 0), 1);
    transport.stop();
    EXPECT_EQ(transport.status().state, LinkState::Stopped);
}

// 场景:平台拒绝带 msg_id 的被动回复(例如窗口已过),但接受主动消息。
// 期望:自动改发主动消息(不带 msg_id),结果为已发送;平台共收到两次请求。
TEST(QqTransport, FallsBackToActiveMessageWhenPassiveIsRejected) {
    test::FakeQqServer server;
    server.message_handler = [](const test::FakeQqServer::Request& r) {
        if (r.body.contains("msg_id"))
            return std::make_pair(400, nlohmann::json{{"code", 40034}, {"message", "msg_id expired"}});
        return std::make_pair(200, nlohmann::json{{"id", "R"}});
    };
    Recorder recorder;  // 停机仍会回调:记录器必须晚于传输对象析构
    QqTransport transport(local_options(server));
    transport.start(recorder.callbacks());
    ASSERT_TRUE(wait_connected(transport));
    server.push_c2c("M1", "U1", "hi");
    ASSERT_TRUE(test::FakeQqServer::wait_until([&] { return recorder.inbound_count() == 1; }));
    const auto inbound = recorder.inbound_at(0);
    const auto result = transport.send_text(inbound.address, "reply", inbound.reply_context);
    EXPECT_EQ(result.outcome, SendOutcome::Sent) << result.error;
    const auto sent = server.requests_to("/v2/users/U1/messages");
    ASSERT_EQ(sent.size(), 2u);
    EXPECT_TRUE(sent[0].body.contains("msg_id"));
    EXPECT_FALSE(sent[1].body.contains("msg_id"));
}

// 场景:被动与主动消息都被拒(对方关闭了主动消息);之后对方又发来一条新消息。
// 期望:第一次发送结果为“暂存”;新消息到来时先用它的被动额度补发暂存内容(带“补发”标记与
// 新的 msg_id),然后才把新消息交给上层。
TEST(QqTransport, HoldsOutputAndFlushesOnNextInbound) {
    test::FakeQqServer server;
    std::atomic<bool> reject{true};
    server.message_handler = [&](const test::FakeQqServer::Request&) {
        if (reject) return std::make_pair(403, nlohmann::json{{"code", 304}, {"message", "proactive disabled"}});
        return std::make_pair(200, nlohmann::json{{"id", "R"}});
    };
    Recorder recorder;  // 停机仍会回调:记录器必须晚于传输对象析构
    QqTransport transport(local_options(server));
    transport.start(recorder.callbacks());
    ASSERT_TRUE(wait_connected(transport));
    server.push_c2c("M1", "U1", "first");
    ASSERT_TRUE(test::FakeQqServer::wait_until([&] { return recorder.inbound_count() == 1; }));
    const auto first = recorder.inbound_at(0);
    EXPECT_EQ(transport.send_text(first.address, "late answer", first.reply_context).outcome, SendOutcome::Held);
    EXPECT_EQ(transport.held_count(), 1u);

    reject = false;
    const auto before = server.requests_to("/v2/users/U1/messages").size();
    server.push_c2c("M2", "U1", "second");
    ASSERT_TRUE(test::FakeQqServer::wait_until([&] { return recorder.inbound_count() == 2; }));
    const auto sent = server.requests_to("/v2/users/U1/messages");
    ASSERT_EQ(sent.size(), before + 1);
    EXPECT_EQ(sent.back().body.value("msg_id", ""), "M2");
    EXPECT_NE(sent.back().body["markdown"].value("content", "").find("late answer"), std::string::npos);
    EXPECT_EQ(sent.back().body["markdown"].value("content", "").rfind(u8"(补发)", 0), 0u);
    EXPECT_EQ(transport.held_count(), 0u);
}

// 场景:机器人不支持 Markdown,平台对 msg_type 2 返回带 markdown 字样的 400。
// 期望:同一段内容改用纯文本(msg_type 0,去掉标记)重发成功;之后的发送直接用纯文本。
TEST(QqTransport, FallsBackToPlainTextWhenMarkdownIsRejected) {
    test::FakeQqServer server;
    server.message_handler = [](const test::FakeQqServer::Request& r) {
        if (r.body.value("msg_type", 0) == kMsgTypeMarkdown)
            return std::make_pair(400, nlohmann::json{{"code", 11255}, {"message", "markdown not allowed"}});
        return std::make_pair(200, nlohmann::json{{"id", "R"}});
    };
    Recorder recorder;  // 停机仍会回调:记录器必须晚于传输对象析构
    QqTransport transport(local_options(server));
    transport.start(recorder.callbacks());
    ASSERT_TRUE(wait_connected(transport));
    server.push_c2c("M1", "U1", "hi");
    ASSERT_TRUE(test::FakeQqServer::wait_until([&] { return recorder.inbound_count() == 1; }));
    const auto inbound = recorder.inbound_at(0);
    EXPECT_EQ(transport.send_text(inbound.address, "**粗体**", inbound.reply_context).outcome, SendOutcome::Sent);
    EXPECT_EQ(transport.send_text(inbound.address, "第二条", inbound.reply_context).outcome, SendOutcome::Sent);
    const auto sent = server.requests_to("/v2/users/U1/messages");
    ASSERT_EQ(sent.size(), 3u);
    EXPECT_EQ(sent[1].body.value("msg_type", -1), kMsgTypeText);
    EXPECT_EQ(sent[1].body.value("content", ""), u8"粗体");
    EXPECT_EQ(sent[2].body.value("msg_type", -1), kMsgTypeText);
}

// 场景:网关重复推送同一条消息(重连补发时常见),以及推送一条群 @ 消息。
// 期望:重复的消息只交给上层一次;群消息按“群 + 成员”定位。
TEST(QqTransport, DeduplicatesMessagesAndParsesGroupMentions) {
    test::FakeQqServer server;
    Recorder recorder;  // 停机仍会回调:记录器必须晚于传输对象析构
    QqTransport transport(local_options(server));
    transport.start(recorder.callbacks());
    ASSERT_TRUE(wait_connected(transport));
    server.push_c2c("M1", "U1", "hi");
    server.push_c2c("M1", "U1", "hi");
    server.push_group_at("M2", "G1", "MB1", "看看");
    ASSERT_TRUE(test::FakeQqServer::wait_until([&] { return recorder.inbound_count() == 2; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(recorder.inbound_count(), 2u);
    const auto group = recorder.inbound_at(1);
    EXPECT_EQ(group.address.kind, ChatKind::Group);
    EXPECT_EQ(group.address.chat, "G1");
    EXPECT_EQ(group.address.sender, "MB1");
}

// 场景:连接期间网关以 4008(频控)断开。
// 期望:等待频控延迟后自动重连,并用 Resume 恢复原会话,最终回到已连接。
TEST(QqTransport, ReconnectsAndResumesAfterRateLimitClose) {
    test::FakeQqServer server;
    Recorder recorder;  // 停机仍会回调:记录器必须晚于传输对象析构
    QqTransport transport(local_options(server));
    transport.start(recorder.callbacks());
    ASSERT_TRUE(wait_connected(transport));
    server.close_connection(4008, "rate limited");
    ASSERT_TRUE(test::FakeQqServer::wait_until([&] { return server.connections() == 2; }));
    ASSERT_TRUE(wait_connected(transport));
    bool resumed = false;
    for (const auto& frame : server.frames()) resumed = resumed || frame.value("op", 0) == 6;
    EXPECT_TRUE(resumed);
}

// 场景:网关以 4915(机器人被封禁)关闭连接。
// 期望:状态变为失败、停止自动重试,原因为“机器人已被封禁”,之后不再重连。
TEST(QqTransport, StopsRetryingAfterBan) {
    test::FakeQqServer server;
    Recorder recorder;  // 停机仍会回调:记录器必须晚于传输对象析构
    QqTransport transport(local_options(server));
    transport.start(recorder.callbacks());
    ASSERT_TRUE(wait_connected(transport));
    server.close_connection(4915, "banned");
    ASSERT_TRUE(test::FakeQqServer::wait_until([&] { return transport.status().state == LinkState::Failed; }));
    const auto status = transport.status();
    EXPECT_TRUE(status.retry_stopped);
    EXPECT_NE(status.detail.find(u8"封禁"), std::string::npos) << status.detail;
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(server.connections(), 1);
}

// 场景:AppSecret 错误,平台拒绝换令牌。
// 期望:不去连网关,状态直接变为失败且停止重试;错误信息不含 AppSecret。
TEST(QqTransport, InvalidCredentialsFailWithoutRetrying) {
    test::FakeQqServer server;
    server.token_handler = [](const nlohmann::json&) {
        return nlohmann::json{{"code", 100016}, {"message", "invalid appsecret"}};
    };
    Recorder recorder;  // 停机仍会回调:记录器必须晚于传输对象析构
    QqTransport transport(local_options(server));
    transport.start(recorder.callbacks());
    ASSERT_TRUE(test::FakeQqServer::wait_until([&] { return transport.status().state == LinkState::Failed; }));
    const auto status = transport.status();
    EXPECT_TRUE(status.retry_stopped);
    EXPECT_EQ(status.detail.find("secret-value-123"), std::string::npos);
    EXPECT_EQ(server.connections(), 0);
}

// 场景:回传一个会话生成的文件。
// 期望:先上传(base64、file_type=4、带文件名),再以 msg_type 7 带 file_info 发送。
TEST(QqTransport, SendsFilesThroughRichMediaUpload) {
    test::FakeQqServer server;
    Recorder recorder;  // 停机仍会回调:记录器必须晚于传输对象析构
    QqTransport transport(local_options(server));
    transport.start(recorder.callbacks());
    ASSERT_TRUE(wait_connected(transport));
    server.push_c2c("M1", "U1", "send me the report");
    ASSERT_TRUE(test::FakeQqServer::wait_until([&] { return recorder.inbound_count() == 1; }));
    const auto inbound = recorder.inbound_at(0);
    const auto file = std::filesystem::path(testing::TempDir()) / "acecode-qq-report.pdf";
    { std::ofstream(file, std::ios::binary) << "PDF-DATA"; }
    const auto result = transport.send_file(inbound.address, file, "report.pdf", "application/pdf",
                                            inbound.reply_context);
    EXPECT_EQ(result.outcome, SendOutcome::Sent) << result.error;
    const auto uploads = server.requests_to("/v2/users/U1/files");
    ASSERT_EQ(uploads.size(), 1u);
    EXPECT_EQ(uploads[0].body.value("file_type", 0), kFileTypeFile);
    EXPECT_EQ(uploads[0].body.value("file_name", ""), "report.pdf");
    const auto sent = server.requests_to("/v2/users/U1/messages");
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0].body.value("msg_type", -1), kMsgTypeMedia);
    EXPECT_EQ(sent[0].body["media"].value("file_info", ""), "INFO-1");
    std::filesystem::remove(file);
}

// 场景:会话产出的文件超过 QQ 直传上限(测试里把上限调到 4 字节)。
// 期望:不上传,改发一条带文件名的文字说明,发送结果为已发送。
TEST(QqTransport, OversizedFileBecomesTextNotice) {
    test::FakeQqServer server;
    auto options = local_options(server);
    options.max_upload_bytes = 4;
    Recorder recorder;  // 停机仍会回调:记录器必须晚于传输对象析构
    QqTransport transport(options);
    transport.start(recorder.callbacks());
    ASSERT_TRUE(wait_connected(transport));
    server.push_c2c("M1", "U1", "send me the report");
    ASSERT_TRUE(test::FakeQqServer::wait_until([&] { return recorder.inbound_count() == 1; }));
    const auto inbound = recorder.inbound_at(0);
    const auto file = std::filesystem::path(testing::TempDir()) / "acecode-qq-big.pdf";
    { std::ofstream(file, std::ios::binary) << "PDF-DATA"; }
    const auto result = transport.send_file(inbound.address, file, "big.pdf", "application/pdf",
                                            inbound.reply_context);
    EXPECT_EQ(result.outcome, SendOutcome::Sent) << result.error;
    EXPECT_TRUE(server.requests_to("/v2/users/U1/files").empty());
    const auto sent = server.requests_to("/v2/users/U1/messages");
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_NE(sent[0].body.dump().find("big.pdf"), std::string::npos);
    std::filesystem::remove(file);
}

} // namespace
} // namespace acecode::im::qqbot
