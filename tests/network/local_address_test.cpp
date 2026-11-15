#include <gtest/gtest.h>
#include "network/proxy_resolver.hpp"

namespace acecode::network {
TEST(LocalAddress, PrivateAndLinkLocalHostsAlwaysBypassProxy) {
    // 场景：手动代理和会话覆盖都已开启，局域网目标仍必须直连。
    NetworkConfig config; config.proxy_mode = "manual"; config.proxy_url = "http://proxy.example:8080";
    ProxyResolver resolver; resolver.init(config);
    resolver.set_session_override_url("http://override.example:8080");
    for (const auto* host : {"10.1.2.3", "172.16.0.1", "172.31.255.255", "192.168.1.1", "169.254.1.1", "ACE-PC", "[fc00::1]", "[fd12::1]", "[fe80::1%25eth0]", "127.0.0.1"})
        EXPECT_TRUE(resolver.effective(std::string("http://") + host + ":12400").url.empty()) << host;
    EXPECT_FALSE(resolver.effective("https://api.example.com").url.empty());
    EXPECT_FALSE(resolver.effective("https://api.example.com/path@ACE-PC").url.empty());
    EXPECT_FALSE(is_direct_local_host("172.32.0.1"));
    EXPECT_FALSE(is_direct_local_host("2001:4860:4860::8888"));
    EXPECT_TRUE(host_matches_no_proxy("intranet", {"<local>"}));
    EXPECT_TRUE(host_matches_no_proxy("host.example.com", {"*.example.com"}));
}
} // namespace acecode::network
