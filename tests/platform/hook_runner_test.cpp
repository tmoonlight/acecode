#include <gtest/gtest.h>

#include "hooks/hook_manager.hpp"
#include "hooks/hook_runner.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

TEST(HookRunner, SendsPayloadOnStdinWithZeroTimeoutAsInfinite) {
    acecode::HookCommandSpec cmd;
#ifdef _WIN32
    cmd.command = "cmd.exe";
    cmd.args = {"/c", "more"};
#else
    cmd.command = "/bin/cat";
#endif

    auto result = acecode::run_hook_process(cmd, "hello hook\n", 0, "");
    EXPECT_TRUE(result.started) << result.error;
    EXPECT_FALSE(result.timed_out);
    EXPECT_EQ(result.exit_code, 0);
    EXPECT_NE(result.output.find("hello hook"), std::string::npos);
}

TEST(HookRunner, ShellCommandRunsThroughPlatformShellWithStdin) {
#ifdef _WIN32
    const std::string command = "more";
#else
    const std::string command = "cat";
#endif
    auto result = acecode::run_hook_shell_command(command, "hello shell hook\n", 3000, "");
    EXPECT_TRUE(result.started) << result.error;
    EXPECT_FALSE(result.timed_out);
    EXPECT_EQ(result.exit_code, 0);
    EXPECT_NE(result.stdout_text.find("hello shell hook"), std::string::npos);
}

TEST(HookRunner, ShellCommandReceivesExactEnvironmentOverride) {
    const std::string expected = "修复 \"登录\" & deploy %PATH% $HOME";
    acecode::HookEnvironment environment{
        {"ACECODE_HOOK_SESSION_TITLE", expected},
    };
#ifdef _WIN32
    const std::string command =
        "powershell.exe -NoLogo -NoProfile -NonInteractive -Command "
        "\"[Console]::Out.Write($env:ACECODE_HOOK_SESSION_TITLE)\"";
#else
    const std::string command = "printf '%s' \"$ACECODE_HOOK_SESSION_TITLE\"";
#endif
    auto result = acecode::run_hook_shell_command(
        command, "", 3000, "", environment);
    EXPECT_TRUE(result.started) << result.error;
    EXPECT_FALSE(result.timed_out);
    EXPECT_EQ(result.exit_code, 0) << result.stderr_text;
    EXPECT_EQ(result.stdout_text, expected);
}

TEST(HookRunner, PositiveTimeoutTerminatesLongRunningHook) {
    acecode::HookCommandSpec cmd;
#ifdef _WIN32
    cmd.command = "cmd.exe";
    cmd.args = {"/c", "ping -n 3 127.0.0.1 > nul"};
#else
    cmd.command = "/bin/sh";
    cmd.args = {"-c", "sleep 2"};
#endif

    auto result = acecode::run_hook_process(cmd, "", 50, "");
    EXPECT_TRUE(result.started) << result.error;
    EXPECT_TRUE(result.timed_out);
}

TEST(HookRunner, BoundedOptionsAbortLongRunningProcess) {
    acecode::HookCommandSpec cmd;
#ifdef _WIN32
    cmd.command = "cmd.exe";
    cmd.args = {"/d", "/s", "/c", "ping -n 6 127.0.0.1 > nul"};
#else
    cmd.command = "/bin/sh";
    cmd.args = {"-c", "sleep 5"};
#endif

    std::atomic<bool> abort{false};
    acecode::HookProcessOptions options;
    options.timeout_ms = 5000;
    options.abort_flag = &abort;
    options.terminate_process_tree = true;

    std::thread canceller([&abort] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        abort.store(true);
    });
    const auto started = std::chrono::steady_clock::now();
    auto result = acecode::run_hook_process(cmd, "", "", options);
    canceller.join();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);

    EXPECT_TRUE(result.started) << result.error;
    EXPECT_TRUE(result.aborted);
    EXPECT_FALSE(result.timed_out);
    EXPECT_LT(elapsed.count(), 2000);
}

TEST(HookRunner, BoundedOptionsStopAtStdoutLineLimit) {
    acecode::HookCommandSpec cmd;
#ifdef _WIN32
    cmd.command = "powershell.exe";
    cmd.args = {"-NoLogo", "-NoProfile", "-NonInteractive", "-Command",
                "1..50 | ForEach-Object { Write-Output ('line-' + $_) }; "
                "Start-Sleep -Seconds 5"};
#else
    cmd.command = "/bin/sh";
    cmd.args = {"-c", "i=0; while [ $i -lt 50 ]; do echo line-$i; "
                         "i=$((i+1)); done; sleep 5"};
#endif

    acecode::HookProcessOptions options;
    options.timeout_ms = 5000;
    options.max_stdout_lines = 3;
    options.terminate_on_stdout_limit = true;
    options.terminate_process_tree = true;
    options.append_output_truncation_notice = false;

    const auto started = std::chrono::steady_clock::now();
    auto result = acecode::run_hook_process(cmd, "", "", options);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);

    EXPECT_TRUE(result.started) << result.error;
    EXPECT_TRUE(result.stdout_truncated);
    EXPECT_TRUE(result.output_limit_reached);
    EXPECT_FALSE(result.timed_out);
    EXPECT_EQ(static_cast<std::size_t>(
                  std::count(result.stdout_text.begin(), result.stdout_text.end(), '\n')),
              3u);
    EXPECT_LT(elapsed.count(), 2000);
}

TEST(HookRunner, BoundedOptionsCapStdoutWithoutProtocolMarker) {
    acecode::HookCommandSpec cmd;
#ifdef _WIN32
    cmd.command = "cmd.exe";
    cmd.args = {"/d", "/s", "/c", "<nul set /p =abcdefghijklmnopqrstuvwxyz"};
#else
    cmd.command = "/bin/sh";
    cmd.args = {"-c", "printf abcdefghijklmnopqrstuvwxyz"};
#endif

    acecode::HookProcessOptions options;
    options.timeout_ms = 3000;
    options.max_stdout_bytes = 10;
    options.append_output_truncation_notice = false;

    auto result = acecode::run_hook_process(cmd, "", "", options);
    EXPECT_TRUE(result.started) << result.error;
    EXPECT_TRUE(result.stdout_truncated);
    EXPECT_FALSE(result.output_limit_reached);
    EXPECT_EQ(result.stdout_text, "abcdefghij");
}

TEST(HookRunner, ResolvesCommandBesideCurrentExecutableBeforePathSearch) {
#ifdef _WIN32
    const std::string binary_name = "acecode_unit_tests.exe";
#else
    const std::string binary_name = "acecode_unit_tests";
#endif

    auto resolved = acecode::resolve_hook_command_path(binary_name);
    EXPECT_NE(resolved, binary_name);
    EXPECT_TRUE(std::filesystem::path(resolved).is_absolute());
    EXPECT_TRUE(std::filesystem::exists(std::filesystem::path(resolved)));
}

TEST(HookRunner, MissingBareCommandStaysAvailableForPathSearch) {
    const std::string command = "__acecode_missing_hook_command_for_path_search__";
    EXPECT_EQ(acecode::resolve_hook_command_path(command), command);
}

TEST(HookRunner, LegacyDispatchPreservesDirectRunnerAndDiagnosticOnlyOutput) {
    acecode::HookConfig cfg;
    cfg.enabled = true;

    for (const std::string event : {
             acecode::kHookEventStartupBeforeModelLoad,
             acecode::kHookEventStartupModelsLoaded,
             acecode::kHookEventAssistantMessageCompleted,
         }) {
        acecode::HookDefinition hook;
        hook.id = event + ".test";
        hook.event = event;
        hook.mode = acecode::HookMode::Sync;
        hook.timeout_ms = 123;
        hook.command.command = "legacy-hook";
        hook.command.args = {event};
        cfg.events[event].push_back(std::move(hook));
    }

    std::vector<std::string> events_seen;
    std::vector<std::string> cwd_seen;
    acecode::HookManager manager(std::move(cfg),
        [&](const acecode::HookCommandSpec& command,
            const std::string& stdin_text,
            int timeout_ms,
            const std::string& cwd) -> acecode::HookProcessResult {
            EXPECT_EQ(command.command, "legacy-hook");
            EXPECT_EQ(command.args.size(), 1u);
            if (!command.args.empty()) {
                EXPECT_EQ(timeout_ms, 123);
            }
            auto payload = nlohmann::json::parse(stdin_text);
            events_seen.push_back(payload.value("event", ""));
            cwd_seen.push_back(cwd);

            acecode::HookProcessResult result;
            result.started = true;
            result.exit_code = 0;
            result.stdout_text = R"({"decision":"block","reason":"ignored for legacy"})";
            result.output = result.stdout_text;
            return result;
        });

    const std::string cwd = "/tmp/acecode-legacy-hooks";
    for (const std::string event : {
             acecode::kHookEventStartupBeforeModelLoad,
             acecode::kHookEventStartupModelsLoaded,
             acecode::kHookEventAssistantMessageCompleted,
         }) {
        EXPECT_EQ(manager.dispatch(event, nlohmann::json::object(), cwd), 1u);
    }

    ASSERT_EQ(events_seen.size(), 3u);
    EXPECT_EQ(events_seen[0], acecode::kHookEventStartupBeforeModelLoad);
    EXPECT_EQ(events_seen[1], acecode::kHookEventStartupModelsLoaded);
    EXPECT_EQ(events_seen[2], acecode::kHookEventAssistantMessageCompleted);
    ASSERT_EQ(cwd_seen.size(), 3u);
    EXPECT_EQ(cwd_seen[0], cwd);
    EXPECT_EQ(cwd_seen[1], cwd);
    EXPECT_EQ(cwd_seen[2], cwd);
}
