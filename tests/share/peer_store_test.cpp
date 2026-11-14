#include <gtest/gtest.h>
#include "peers/peer_store.hpp"
#include "peers/peer_tools.hpp"
#include "tool/tool_executor.hpp"
#include "test_support/network/crow_test_server.hpp"
#include "utils/utf8_path.hpp"
#include "utils/uuid.hpp"
#include <filesystem>

namespace acecode::peers {
namespace {
using nlohmann::json;
share::InviteLink invitation(int port) {
    return {"Owner", "Session", {"127.0.0.2", "127.0.0.1"}, port,
        "0123456789abcdef.0123456789abcdefghijklmnopqrstuv", 4102444800};
}
class PeerStoreTest : public testing::Test {
protected:
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("acecode-peer-" + generate_uuid());
    void TearDown() override { std::error_code ec; std::filesystem::remove_all(root, ec); }
};

TEST_F(PeerStoreTest, ReplacesInvitationWithoutDuplicatingSavedSession) {
    // 场景：重新连接同一电脑的同一会话，替换失效邀请且保留对端 id。
    PeerStore store(path_to_utf8(root));
    PeerRecord peer;
    peer.invite = invitation(12400); peer.address = "127.0.0.1";
    peer.identity = "owner-identity"; peer.session_id = "session-a";
    const auto first = store.save(peer);
    peer.invite.credential.replace(17, 32, std::string(32, 'a'));
    peer.allow_questions = true;
    const auto updated = store.save(peer);
    EXPECT_EQ(updated.id, first.id);
    PeerStore restarted(path_to_utf8(root));
    ASSERT_EQ(restarted.list().size(), 1u);
    const auto saved = restarted.find(first.id);
    ASSERT_TRUE(saved);
    EXPECT_TRUE(saved->allow_questions);
    EXPECT_EQ(saved->invite.credential, peer.invite.credential);
    const auto public_value = peer_public_json(*saved);
    EXPECT_EQ(public_value.at("credential_suffix"), "aaaa");
    EXPECT_EQ(public_value.dump().find(peer.invite.credential), std::string::npos);
    store.set_status(first.id, "invalid");
    EXPECT_EQ(restarted.find(first.id)->status, "invalid");
    EXPECT_TRUE(store.erase(first.id));
    EXPECT_FALSE(restarted.find(first.id));
}

TEST_F(PeerStoreTest, CandidateFallbackAndProtocolVersionAreEnforced) {
    // 场景：第一地址不可达，使用第二地址；错误主版本和全部失败不得保存。
    crow::SimpleApp app;
    std::atomic<int> version{1};
    CROW_ROUTE(app, "/api/share/v1/hello").methods(crow::HTTPMethod::POST)([&](const crow::request& req) {
        EXPECT_EQ(req.get_header_value("X-ACECode-Share"), invitation(1).credential);
        EXPECT_TRUE(req.get_header_value("X-ACECode-Token").empty());
        return crow::response(json{{"version", version.load()}, {"identity", "owner"}, {"session_id", "session"},
            {"allow_questions", false}, {"include_tools", false}}.dump());
    });
    test::CrowTestServer server(app);
    auto invite = invitation(server.port());
    auto peer = connect_peer(invite);
    EXPECT_EQ(peer.address, "127.0.0.1");
    PeerStore store(path_to_utf8(root));
    peer = store.save(peer);
    EXPECT_EQ(reconnect_peer(store, peer).id, peer.id);
    version = 2;
    EXPECT_THROW(connect_peer(invite), std::runtime_error);
    invite.addresses = {"127.0.0.2"};
    EXPECT_THROW(connect_peer(invite), std::runtime_error);
}

TEST_F(PeerStoreTest, ToolsRequireSavedIdsAndLabelExternalData) {
    // 场景：三个工具始终注册，只接受用户保存过的 id，并标记外部不可信数据。
    auto store = std::make_shared<PeerStore>(path_to_utf8(root));
    ToolExecutor tools;
    register_peer_tools(tools, store);
    EXPECT_EQ(tools.get_model_tool_definitions().size(), 3u);
    const auto listed = tools.execute("list_peers", "{}");
    ASSERT_TRUE(listed.success);
    EXPECT_EQ(json::parse(listed.output).at("source"), "external_untrusted");
    EXPECT_FALSE(tools.execute("read_peer_session", R"({"peer_id":"http://127.0.0.1"})").success);
    PeerRecord peer;
    peer.invite = invitation(1); peer.address = "127.0.0.1"; peer.identity = "owner"; peer.session_id = "session";
    peer = store->save(peer);
    EXPECT_FALSE(tools.execute("ask_peer_session", json{{"peer_id", peer.id}, {"question", "hello"}}.dump()).success);
    store.reset();
    EXPECT_FALSE(tools.execute("list_peers", "{}").success);
}
} // namespace
} // namespace acecode::peers
