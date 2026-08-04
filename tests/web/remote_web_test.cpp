#include "web/remote_web.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

namespace {

using acecode::web::build_remote_web_url;
using acecode::web::rank_remote_web_hosts;
using acecode::web::remote_web_bind_for_enabled;
using acecode::web::remote_web_enabled_for_bind;
using acecode::web::remote_web_host_from_header;

TEST(RemoteWeb, BindModeUsesCanonicalValues) {
    EXPECT_FALSE(remote_web_enabled_for_bind("127.0.0.1"));
    EXPECT_FALSE(remote_web_enabled_for_bind("127.5.6.7"));
    EXPECT_FALSE(remote_web_enabled_for_bind(" ::1 "));
    EXPECT_FALSE(remote_web_enabled_for_bind("::ffff:127.0.0.1"));
    EXPECT_FALSE(remote_web_enabled_for_bind("LOCALHOST"));
    EXPECT_TRUE(remote_web_enabled_for_bind("0.0.0.0"));
    EXPECT_TRUE(remote_web_enabled_for_bind("192.168.1.10"));
    EXPECT_EQ(remote_web_bind_for_enabled(false), "127.0.0.1");
    EXPECT_EQ(remote_web_bind_for_enabled(true), "0.0.0.0");
}

TEST(RemoteWeb, HostHeaderParserRejectsUnsafeAndLoopbackValues) {
    EXPECT_EQ(
        remote_web_host_from_header("192.168.10.7:28080"),
        std::optional<std::string>("192.168.10.7"));
    EXPECT_EQ(
        remote_web_host_from_header("[2001:db8::7]:28080"),
        std::optional<std::string>("2001:db8::7"));
    EXPECT_EQ(
        remote_web_host_from_header("acecode.example.test"),
        std::optional<std::string>("acecode.example.test"));

    EXPECT_FALSE(remote_web_host_from_header("127.0.0.1:28080"));
    EXPECT_FALSE(remote_web_host_from_header("localhost:28080"));
    EXPECT_FALSE(remote_web_host_from_header("host.test/path"));
    EXPECT_FALSE(remote_web_host_from_header("host.test:bad"));
    EXPECT_FALSE(remote_web_host_from_header("evil.test@safe.test"));
}

TEST(RemoteWeb, RankingFiltersUnusableAddressesAndDeduplicates) {
    const auto result = rank_remote_web_hosts(
        {
            "127.0.0.1",
            "0.0.0.0",
            "169.254.2.3",
            "192.168.1.20",
            "192.168.1.20",
            "fe80::1234",
            "2001:db8::20",
            "not-an-address.test",
        },
        std::optional<std::string>("vpn.example.test"),
        {},
        std::optional<std::string>("ACE-DESKTOP"));

    ASSERT_EQ(result.size(), 4u);
    EXPECT_EQ(result[0], "ACE-DESKTOP");
    EXPECT_EQ(result[1], "vpn.example.test");
    EXPECT_EQ(result[2], "192.168.1.20");
    EXPECT_EQ(result[3], "2001:db8::20");
}

TEST(RemoteWeb, ComputerNameIsFirstAndDeduplicatedCaseInsensitively) {
    const auto result = rank_remote_web_hosts(
        {"192.168.1.20"},
        std::optional<std::string>("ace-desktop"),
        "0.0.0.0",
        std::optional<std::string>("ACE-DESKTOP"));

    ASSERT_EQ(result.size(), 2u);
    EXPECT_EQ(result[0], "ACE-DESKTOP");
    EXPECT_EQ(result[1], "192.168.1.20");
}

TEST(RemoteWeb, RankingMatchesTheEffectiveListenerAddressFamily) {
    const std::vector<std::string> discovered{
        "192.168.1.20",
        "192.168.1.21",
        "2001:db8::20",
    };

    const auto ipv4 = rank_remote_web_hosts(
        discovered,
        std::optional<std::string>("vpn.example.test"),
        "0.0.0.0");
    ASSERT_EQ(ipv4.size(), 3u);
    EXPECT_EQ(ipv4[0], "vpn.example.test");
    EXPECT_EQ(ipv4[1], "192.168.1.20");
    EXPECT_EQ(ipv4[2], "192.168.1.21");

    const auto ipv6 = rank_remote_web_hosts(
        discovered,
        std::nullopt,
        "::");
    ASSERT_EQ(ipv6.size(), 1u);
    EXPECT_EQ(ipv6[0], "2001:db8::20");

    const auto specific = rank_remote_web_hosts(
        discovered,
        std::nullopt,
        "192.168.1.21");
    ASSERT_EQ(specific.size(), 1u);
    EXPECT_EQ(specific[0], "192.168.1.21");
}

TEST(RemoteWeb, ConnectionUrlBracketsIpv6AndEncodesToken) {
    EXPECT_EQ(
        build_remote_web_url("192.168.1.20", 28080, "abc+def/ghi="),
        "http://192.168.1.20:28080/?token=abc%2Bdef%2Fghi%3D");
    EXPECT_EQ(
        build_remote_web_url("[2001:db8::20]", 28080, "secret"),
        "http://[2001:db8::20]:28080/?token=secret");
}

} // namespace
