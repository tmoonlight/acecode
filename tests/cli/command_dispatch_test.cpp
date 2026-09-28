#include <gtest/gtest.h>
#include "cli/command_dispatch.hpp"
#include "version.hpp"
#include <vector>

namespace {
struct Args {
    std::vector<std::string> values;
    std::vector<char*> pointers;
    explicit Args(std::vector<std::string> input) : values(std::move(input)) {
        for (auto& value : values) pointers.push_back(value.data());
    }
    std::optional<int> run() {
        return acecode::cli::dispatch_non_tui_command(
            static_cast<int>(pointers.size()), pointers.data());
    }
};
}
TEST(CommandDispatch, VersionPrecedesOtherArgumentScanning) {
    // version 后面的服务标志不能启动服务;四个旧别名保持一致。
    for (const auto* alias : {"version", "-version", "--version", "/version"}) {
        Args args({"acecode", alias, "--service-main"});
        testing::internal::CaptureStdout();
        const auto result = args.run();
        const auto output = testing::internal::GetCapturedStdout();
        EXPECT_EQ(result, 0);
        EXPECT_EQ(output, "acecode v" ACECODE_VERSION "\n");
    }
}
TEST(CommandDispatch, HelpPrecedesPrintModeAndUnknownArguments) {
    // 顶层帮助优先返回,不会把后续 -p 或未知参数交给无头运行器。
    for (const auto* alias : {"help", "-h", "--help", "/?"}) {
        Args args({"acecode", alias, "-p", "--unknown"});
        testing::internal::CaptureStdout();
        const auto result = args.run();
        const auto output = testing::internal::GetCapturedStdout();
        EXPECT_EQ(result, 0);
        EXPECT_NE(output.find("Usage:\n"), std::string::npos);
        EXPECT_NE(output.find("Start the interactive TUI"), std::string::npos);
    }
}
TEST(CommandDispatch, InvalidUpgradeStopsAtUsageBeforeConfigurationOrNetwork) {
    // 非法升级参数不应触发更新检查或读取、修改用户配置。
    Args args({"acecode", "upgrade", "--server"});
    testing::internal::CaptureStderr();
    const auto result = args.run();
    const auto output = testing::internal::GetCapturedStderr();
    EXPECT_EQ(result, 64);
    EXPECT_EQ(output, "acecode upgrade: missing value for --server\n"
                     "usage: acecode upgrade [--force] [--server=<url>]\n");
}
