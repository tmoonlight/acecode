#include <gtest/gtest.h>
#include "web/remote_web.hpp"
#include "web/remote_web_proxy.hpp"

TEST(InviteCandidates, PhysicalInterfacesPrecedeVirtualOnes) {
    // 场景：虚拟网卡先枚举到，仍将物理网卡排序在前；电脑名继续保持第一候选。
    const auto hosts = acecode::web::order_remote_interface_hosts({
        {"Hyper-V vEthernet", "172.20.0.1"}, {"Ethernet", "192.168.1.20"},
        {"Docker", "172.21.0.1"}, {"Wi-Fi", "10.0.0.20"}});
    EXPECT_EQ(hosts, (std::vector<std::string>{"192.168.1.20", "10.0.0.20", "172.20.0.1", "172.21.0.1"}));
    const auto ranked = acecode::web::rank_remote_web_hosts(hosts, {}, {}, "ACE-PC");
    EXPECT_EQ(ranked.front(), "ACE-PC");
}

TEST(InviteCandidates, StablePortHasOrderedFallbacksWithoutDuplicates) {
    // 场景：Desktop daemon 随机端口，转发仍优先 12400；CLI 默认端口不重复探测。
    using acecode::web::remote_web_proxy_port_candidates;
    EXPECT_EQ(remote_web_proxy_port_candidates(0, 32000), (std::vector<int>{12400, 32001, 0}));
    EXPECT_EQ(remote_web_proxy_port_candidates(0, 12399), (std::vector<int>{12400, 0}));
    EXPECT_EQ(remote_web_proxy_port_candidates(0, 12400), (std::vector<int>{12401, 0}));
    EXPECT_EQ(remote_web_proxy_port_candidates(18000, 32000), (std::vector<int>{18000}));
}
