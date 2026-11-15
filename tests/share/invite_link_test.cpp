#include <gtest/gtest.h>
#include "share/invite_link.hpp"
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace acecode::share {

TEST(InviteLink, SharedFixturesMatchFrontend) {
    // 场景：前后端读取同一组合法及畸形链接，逐字段结果必须相同。
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path() / "fixtures/invite_links";
    for (const auto& file : std::filesystem::directory_iterator(root)) {
        std::ifstream input(file.path());
        const auto fixture = nlohmann::json::parse(input);
        SCOPED_TRACE(file.path().string());
        auto result = decode_invite_link(fixture.at("text").get<std::string>(), fixture.at("now"));
        EXPECT_EQ(result.error, fixture.value("error", ""));
        if (!result || !fixture.contains("value")) continue;
        const auto& expected = fixture.at("value");
        EXPECT_EQ(result.value.owner, expected.at("owner"));
        EXPECT_EQ(result.value.title, expected.at("title"));
        EXPECT_EQ(result.value.addresses, expected.at("addresses").get<std::vector<std::string>>());
        EXPECT_EQ(result.value.port, expected.at("port"));
        EXPECT_EQ(result.value.credential, expected.at("credential"));
        EXPECT_EQ(result.value.expires_at, expected.at("expires_at"));
        EXPECT_TRUE(decode_invite_link(encode_invite_link(result.value), fixture.at("now")));
    }
}

TEST(InviteLink, RejectsInvalidOutputAndUnsafeHosts) {
    // 场景：调用方误传空地址、URL 或超长标题，编码不能生成可用邀请。
    InviteLink invite{"owner", "title", {"host"}, 12400,
        "0123456789abcdef.0123456789abcdefghijklmnopqrstuv", 2000000000};
    EXPECT_NO_THROW(encode_invite_link(invite));
    for (const auto* host : {"https://host", "host@evil", "host/path", "a..b", "256.1.1.1", "-host"}) {
        invite.addresses = {host};
        EXPECT_THROW(encode_invite_link(invite), std::invalid_argument);
    }
    invite.addresses = {"host"};
    invite.title = std::string(513, 'x');
    EXPECT_THROW(encode_invite_link(invite), std::invalid_argument);
}

} // namespace acecode::share
