#include "test_support/agent/agent_loop_fixture.hpp"
#include <gtest/gtest.h>
#include "cli/channels_cli.hpp"
#include "tui/commands/command_registry.hpp"
#include "tui/commands/builtin_commands.hpp"
#include "channels/runtime.hpp"
#include "test_support/channels/test_support.hpp"
#include "tool/tool_executor.hpp"
#include <sstream>

namespace acecode::channels {
TEST(ChannelCommand, ParsesTextAndPathsWithoutShellEvaluation) {
    EXPECT_EQ(parse_command("")["op"], "setup");
    EXPECT_EQ(parse_command("setup")["op"], "setup");
    auto send = parse_command("send session-1 /aq text with spaces");
    EXPECT_EQ(send["session_id"], "session-1");
    EXPECT_EQ(send["text"], "/aq text with spaces");
    EXPECT_EQ(parse_command("file s1 \"C:/space dir/file.txt\"")["path"], "C:/space dir/file.txt");
    const auto windows_file = parse_command(R"(file s1 "C:\space dir\file.txt")");
    EXPECT_EQ(windows_file["path"], "C:\\space dir\\file.txt");
    EXPECT_THROW(parse_command("file s1 \"unclosed"), std::exception);
    EXPECT_THROW(parse_command("send s1"), std::exception);
    EXPECT_THROW(parse_command("on extra"), std::exception);
    EXPECT_THROW(parse_command("shell command"), std::exception);
}
TEST(ChannelCommand, RendersStatusWithoutRawPairingOrControlSecrets) {
    const auto text = format_result({{"state", "pairing"}, {"account", "100@s.whatsapp.net"},
                                    {"token", "secret-token"}, {"qr", "raw-secret"}, {"qr_text", "QR cells"}});
    EXPECT_NE(text.find("QR cells"), std::string::npos);
    EXPECT_EQ(text.find("secret-token"), std::string::npos);
    EXPECT_EQ(text.find("raw-secret"), std::string::npos);
}
TEST(ChannelCommand, MainTuiDoesNotRegisterChannelCommands) {
    TuiState state;
    AppConfig config;
    TokenTracker tracker;
    PermissionManager permissions;
    ToolExecutor tools;
    AgentLoop loop(
        acecode_test::AgentLoopFixture::dependencies([] { return std::shared_ptr<LlmProvider>{}; }, tools, AgentCallbacks{}, permissions),
        acecode_test::AgentLoopFixture::configuration(""));
    loop.start();
    CommandRegistry registry;
    register_builtin_commands(registry);
    CommandContext context{state, loop, nullptr, config, tracker, permissions};
    bool refreshed = false;
    context.post_event = [&] { refreshed = true; };
    EXPECT_FALSE(registry.has_command("channels"));
    EXPECT_TRUE(registry.dispatch("/channels", context));
    EXPECT_TRUE(registry.dispatch("/channels help", context));
    EXPECT_TRUE(registry.dispatch("/channels on", context));
    EXPECT_FALSE(refreshed);
    ASSERT_EQ(state.conversation.size(), 3);
    for (const auto& message : state.conversation) {
        EXPECT_EQ(message.role, "system");
        EXPECT_NE(message.content.find("Unknown command: /channels"), std::string::npos);
    }
    loop.shutdown();
}
TEST(ChannelCommand, CliHelpNeedsNoDaemon) {
    std::ostringstream out, error;
    EXPECT_EQ(run_cli({"help"}, out, error), 0);
    EXPECT_TRUE(error.str().empty());
    EXPECT_NE(out.str().find("no daemon is started"), std::string::npos);
    EXPECT_NE(out.str().find("already running daemon/Desktop"), std::string::npos);
    EXPECT_EQ(out.str().find("/channels"), std::string::npos);
}
TEST(ChannelCommand, RuntimeCommandsNeverStartAMissingDaemon) {
    const auto home = test::temporary("cli-no-host");
    {
        test::Home isolated(home);
        for (const auto* op : {"on", "reconnect", "off", "qr", "sessions"}) {
            std::ostringstream out, error;
            EXPECT_EQ(run_cli({op}, out, error), 1);
            EXPECT_TRUE(out.str().empty());
            EXPECT_NE(error.str().find("Start acecode daemon or Desktop first"), std::string::npos);
        }
        std::ostringstream out, error;
        EXPECT_EQ(run_cli({"status"}, out, error), 0);
        EXPECT_TRUE(error.str().empty());
        EXPECT_NE(out.str().find("disabled"), std::string::npos);
        EXPECT_FALSE(std::filesystem::exists(channel_directory() / "run"));
        EXPECT_FALSE(std::filesystem::exists(channel_directory() / "owner.json"));
        EXPECT_FALSE(std::filesystem::exists(channel_directory() / "state.json"));
    }
    std::error_code ec; std::filesystem::remove_all(home, ec);
}
} // namespace acecode::channels
