#include <gtest/gtest.h>

#include "web/http_address.hpp"

TEST(WebHttpAddress, FormatsCanonicalDaemonLoopbackAddress) {
    EXPECT_EQ(
        acecode::web::format_http_address("127.0.0.1", 12399),
        "http://127.0.0.1:12399/");
}

TEST(WebHttpAddress, BracketsIpv6Hosts) {
    EXPECT_EQ(
        acecode::web::format_http_address("::1", 12399),
        "http://[::1]:12399/");
}
