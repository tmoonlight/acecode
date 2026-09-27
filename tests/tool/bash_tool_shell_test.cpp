#include <gtest/gtest.h>
#include "environment/terminal_runtime.hpp"
#include "tool/bash_tool.hpp"
#include <nlohmann/json.hpp>
#include "test_support/sandbox/test_support.hpp"

namespace {
class BashToolShellTest : public testing::Test {
    std::optional<acecode::environment::TerminalResolution> previous;
    void SetUp() override { previous = acecode::environment::terminal().last(); }
    void TearDown() override {
        if (previous) acecode::environment::terminal().publish(*previous);
        else acecode::environment::terminal().reset_for_test();
    }
};
}

TEST_F(BashToolShellTest, SelectedShellExecutesQuotedText) {
    acecode::ConsoleConfig config;
#ifdef _WIN32
    config.default_shell = "powershell";
    const std::string command = "Write-Output 'quoted \"value\" & spaces'";
#else
    config.default_shell = "shell";
    const std::string command = "printf '%s' 'quoted \"value\" & spaces'";
#endif
    auto resolution = acecode::environment::terminal().reresolve(config);
    ASSERT_TRUE(resolution.resolved.usable) << resolution.resolved.fallback_reason;
    ASSERT_EQ(resolution.resolved.id, config.default_shell) << resolution.resolved.fallback_reason;
    acecode::ToolContext context;
    auto result = acecode::create_bash_tool().execute(nlohmann::json{{"command", command}}.dump(), context);
    EXPECT_TRUE(result.success) << result.output;
    EXPECT_NE(result.output.find("quoted \"value\" & spaces"), std::string::npos);
}

TEST_F(BashToolShellTest, FailedDetectionReportsAnErrorInsteadOfExecutingAnotherShell) {
    acecode::environment::TerminalResolution unavailable;
    unavailable.resolved.fallback_reason = "launch denied";
    acecode::environment::terminal().publish(unavailable);
    acecode::ToolContext context;
    auto result = acecode::create_bash_tool().execute(R"({"command":"echo should-not-run"})", context);
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.output.find("launch denied"), std::string::npos);
    EXPECT_EQ(result.output.find("should-not-run"), std::string::npos);
}

#if defined(__linux__)
TEST_F(BashToolShellTest, MissingSandboxBackendNeverRunsTheCommandWithoutRestrictions) {
    acecode::environment::terminal().reset_for_test();
    acecode::sandbox::test::TempTree tree;
    acecode::ToolContext context;
    context.cwd = acecode::path_to_utf8(tree.root);
    acecode::sandbox::ExecSandboxRequest request;
    request.policy.mode = acecode::sandbox::SandboxMode::ReadOnly;
    request.backend = acecode::sandbox::BackendKind::LinuxBwrap;
    request.backend_executable = acecode::path_to_utf8(tree.root / "missing-bwrap");
    context.exec_sandbox = request;
    auto result = acecode::create_bash_tool().execute(R"({"command":"touch should-not-exist"})", context);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.metadata.value("sandbox_unavailable", false));
    EXPECT_FALSE(std::filesystem::exists(tree.root / "should-not-exist"));
    context.exec_sandbox.reset();
    auto ordinary_failure = acecode::create_bash_tool().execute(R"({"command":"exit 127"})", context);
    EXPECT_FALSE(ordinary_failure.success);
    EXPECT_FALSE(ordinary_failure.metadata.value("sandbox_unavailable", false));
}
#endif
