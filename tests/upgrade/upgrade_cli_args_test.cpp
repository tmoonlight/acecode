#include <gtest/gtest.h>
#include "upgrade/upgrade_cli_args.hpp"
#include <sstream>
#include <vector>

namespace {
struct Args {
    std::vector<std::string> values;
    std::vector<char*> pointers;
    explicit Args(std::vector<std::string> input) : values(std::move(input)) {
        for (auto& value : values) pointers.push_back(value.data());
    }
};
}
TEST(UpgradeCliArgs, LastServerWinsAndEmptyValueRemainsExplicit) {
    // 空的 --server= 必须交给后续 URL 校验,不能变成没有提供覆盖值。
    Args args({"acecode", "upgrade", "--server=https://old.example", "--force", "--server="});
    bool force = false;
    std::optional<std::string> server;
    std::ostringstream error;
    EXPECT_FALSE(acecode::upgrade::parse_upgrade_cli_args(
        static_cast<int>(args.pointers.size()), args.pointers.data(), force, server, error));
    EXPECT_TRUE(force);
    ASSERT_TRUE(server.has_value());
    EXPECT_TRUE(server->empty());
    EXPECT_TRUE(error.str().empty());
}
TEST(UpgradeCliArgs, MissingAndUnknownOptionsKeepExitCodeAndExactUsage) {
    // update 别名必须原样出现在错误和用法中,并在读取配置前结束。
    for (const auto& value : {std::string("--server"), std::string("--unknown")}) {
        Args args({"acecode", "update", value});
        bool force = false;
        std::optional<std::string> server;
        std::ostringstream error;
        EXPECT_EQ(acecode::upgrade::parse_upgrade_cli_args(
            static_cast<int>(args.pointers.size()), args.pointers.data(), force, server, error), 64);
        EXPECT_EQ(error.str(), "acecode update: " +
            (value == "--server" ? std::string("missing value for --server\n") :
             std::string("unknown option: --unknown\n")) +
            "usage: acecode update [--force] [--server=<url>]\n");
    }
}
