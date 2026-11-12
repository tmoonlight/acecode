// 后台任务登记表 + bash 后台化的端到端测试。
//
// 覆盖:
//   - 纯函数:完成通知 / 工具结果文案、输出末尾截取、前置 sleep 识别;
//   - 登记表:后台命令跑完通知、停止、运行上限、逐行通知、静默提醒、
//     子代理观察项与「已同步拿到结果不再重复通知」;
//   - bash 工具:run_in_background、前台超时转后台(不杀进程)、
//     「转后台」信号让路、长 sleep 拦截。
// 子进程是真实的(cmd.exe / sh),命令挑跨平台都能跑的最小集合。

#include <gtest/gtest.h>

#include "tool/background_tasks.hpp"
#include "tool/bash_tool.hpp"
#include "tool/tool_executor.hpp"
#include "utils/utf8_path.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace acecode;
using namespace std::chrono_literals;

namespace {

#ifdef _WIN32
const std::string kLongCommand = "ping -n 30 127.0.0.1 >nul";
// 经 bash 工具跑的长命令不能是「ping 本机」这种睡眠写法,否则会被长 sleep 拦截。
const std::string kLongToolCommand = "cmd /d /c ping -n 30 127.0.0.1 >nul";
const std::string kTwoLinesWithGap = "echo first& ping -n 3 127.0.0.1 >nul& echo second";
#else
const std::string kLongCommand = "sleep 30";
const std::string kLongToolCommand = "sh -c 'sleep 30'";
const std::string kTwoLinesWithGap = "echo first; sleep 2; echo second";
#endif

std::filesystem::path unique_temp_dir(const std::string& name) {
    const auto unique = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    auto root = std::filesystem::temp_directory_path() / (name + "_" + unique);
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    return root;
}

// 收集登记表投出的通知,测试线程按条件等待。
struct NoticeCollector {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<BackgroundNotice> notices;
    std::vector<BackgroundTaskInfo> changes;

    BackgroundTaskRegistry::Sink sink() {
        BackgroundTaskRegistry::Sink sink;
        sink.notify = [this](BackgroundNotice notice) {
            std::lock_guard<std::mutex> lock(mu);
            notices.push_back(std::move(notice));
            cv.notify_all();
        };
        sink.changed = [this](const BackgroundTaskInfo& info) {
            std::lock_guard<std::mutex> lock(mu);
            changes.push_back(info);
            cv.notify_all();
        };
        return sink;
    }

    // 等到出现 event 字段为 event 的通知,返回它;超时返回空。
    std::optional<BackgroundNotice> wait_event(const std::string& event, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mu);
        std::optional<BackgroundNotice> found;
        cv.wait_for(lock, timeout, [&] {
            for (const auto& notice : notices) {
                if (notice.metadata.value("event", std::string{}) == event) {
                    found = notice;
                    return true;
                }
            }
            return false;
        });
        return found;
    }

    std::size_t count(const std::string& event) {
        std::lock_guard<std::mutex> lock(mu);
        std::size_t n = 0;
        for (const auto& notice : notices) {
            if (notice.metadata.value("event", std::string{}) == event) ++n;
        }
        return n;
    }
};

BackgroundTaskRegistry::ShellStart shell_start(const std::string& command, const std::filesystem::path& dir) {
    BackgroundTaskRegistry::ShellStart start;
    start.spec.command = command;
    start.spec.cwd = path_to_utf8(dir);
    start.title = command;
    start.output_dir = path_to_utf8(dir / "background");
    return start;
}

ToolContext background_context(BackgroundTaskRegistry& registry, const std::filesystem::path& dir,
                                const std::atomic<std::uint64_t>* generation = nullptr,
                                const std::atomic<int>* reason = nullptr) {
    ToolContext ctx;
    ctx.cwd = path_to_utf8(dir);
    ctx.scratch_dir = path_to_utf8(dir / "scratch");
    ctx.background_tasks = &registry;
    ctx.background_generation = generation;
    ctx.background_reason = reason;
    return ctx;
}

} // namespace

// 场景:命令跑完 / 失败 / 被运行上限停掉三种完成通知。
// 期望:状态、退出码、完整输出文件路径和最后输出都在通知里;失败的退出码非 0
// 时状态是 failed 而不是 completed(模型据此判断要不要处理)。
TEST(BackgroundTaskFormatting, CompletionNoticeCarriesStateExitCodeFileAndTail) {
    BackgroundTaskInfo info;
    info.id = "bg_1";
    info.command = "npm run build";
    info.state = BackgroundTaskState::Failed;
    info.exit_code = 2;
    info.started_at_ms = 1000;
    info.ended_at_ms = 1000 + 192000;
    info.output_file = "C:/tmp/bg_1.log";
    const std::string text = format_background_completion(info, "error TS2304\n");
    EXPECT_NE(text.find("status=\"failed\""), std::string::npos);
    EXPECT_NE(text.find("exit-code=\"2\""), std::string::npos);
    EXPECT_NE(text.find("3m 12s"), std::string::npos);
    EXPECT_NE(text.find("C:/tmp/bg_1.log"), std::string::npos);
    EXPECT_NE(text.find("error TS2304"), std::string::npos);

    info.state = BackgroundTaskState::Stopped;
    info.stop_reason = "limit";
    EXPECT_NE(format_background_completion(info, "").find("background time limit"), std::string::npos);
    EXPECT_NE(format_background_completion(info, "").find("no output"), std::string::npos);
}

// 场景:输出很长时通知只带末尾。期望:从一整行开始截,并标出前面被省略;
// 短输出原样返回。非显然阈值:截取按字节计,不会切开 UTF-8 多字节字符。
TEST(BackgroundTaskFormatting, OutputTailStartsAtLineBoundaryAndKeepsUtf8Intact) {
    EXPECT_EQ(background_output_tail("short\n", 100), "short\n");
    std::string long_text;
    for (int i = 0; i < 100; ++i) long_text += "line " + std::to_string(i) + " 中文\n";
    const std::string tail = background_output_tail(long_text, 60);
    EXPECT_EQ(tail.rfind("[... earlier output omitted ...]\n", 0), 0u);
    const std::string body = tail.substr(std::string("[... earlier output omitted ...]\n").size());
    EXPECT_EQ(body.rfind("line ", 0), 0u);
    EXPECT_NE(body.find("line 99"), std::string::npos);
}

// 场景:前台命令被转进后台后给模型的工具结果。期望:说明原因、任务 ID、
// 输出文件,叮嘱别轮询 / 别再起一份,并给出停止工具名。
TEST(BackgroundTaskFormatting, BackgroundedToolResultExplainsOriginAndNoPolling) {
    BackgroundTaskInfo info;
    info.id = "bg_9";
    info.output_file = "C:/tmp/bg_9.log";
    info.origin = BackgroundOrigin::Timeout;
    const std::string text = format_backgrounded_tool_result(info, 120000, "stop_background_task");
    EXPECT_NE(text.find("2m timeout"), std::string::npos);
    EXPECT_NE(text.find("bg_9"), std::string::npos);
    EXPECT_NE(text.find("Do not poll"), std::string::npos);
    EXPECT_NE(text.find("second copy"), std::string::npos);
    EXPECT_NE(text.find("stop_background_task"), std::string::npos);

    info.origin = BackgroundOrigin::UserMessage;
    EXPECT_NE(format_backgrounded_tool_result(info, 0, "stop_background_task").find("message from the user"),
              std::string::npos);
}

// 场景:模型用「先睡一会儿再看」轮询。期望:识别 sleep / Start-Sleep / timeout /t /
// ping -n N 127.0.0.1 的秒数;sleep 不在开头、或只是被 start /B 包着的不算。
TEST(BashLeadingSleep, RecognizesCommonSleepIdiomsOnlyAtTheStart) {
    EXPECT_DOUBLE_EQ(*bash_leading_sleep_seconds("sleep 90"), 90.0);
    EXPECT_DOUBLE_EQ(*bash_leading_sleep_seconds("sleep 5 && npm test"), 5.0);
    EXPECT_DOUBLE_EQ(*bash_leading_sleep_seconds("Start-Sleep -Seconds 30; Get-Content x"), 30.0);
    EXPECT_DOUBLE_EQ(*bash_leading_sleep_seconds("Start-Sleep -Milliseconds 30000"), 30.0);
    EXPECT_DOUBLE_EQ(*bash_leading_sleep_seconds("timeout /t 60 /nobreak >nul"), 60.0);
    EXPECT_DOUBLE_EQ(*bash_leading_sleep_seconds("ping -n 31 127.0.0.1 >nul"), 30.0);
    EXPECT_FALSE(bash_leading_sleep_seconds("npm test").has_value());
    EXPECT_FALSE(bash_leading_sleep_seconds("echo sleep 90").has_value());
    EXPECT_FALSE(bash_leading_sleep_seconds("ping -n 4 example.com").has_value());
    EXPECT_FALSE(bash_leading_sleep_seconds("start /B cmd /c \"timeout /t 8 /nobreak >nul\" && echo done").has_value());
}

// 场景:bash run_in_background 起一个很快结束的命令。
// 期望:工具立即返回任务 ID;命令结束后登记表投出 finished 通知,带退出码 0、
// 输出内容和日志文件;日志文件里有完整输出;任务从运行中消失。
TEST(BackgroundTaskRegistryTest, BackgroundCommandNotifiesWithOutputWhenFinished) {
    const auto dir = unique_temp_dir("bg_finish");
    NoticeCollector collector;
    BackgroundTaskRegistry registry;
    registry.connect(collector.sink());
    ShellLaunchFailure failure;
    const std::string id = registry.start_shell(shell_start("echo hello-background", dir), failure);
    ASSERT_FALSE(id.empty()) << failure.message;

    const auto notice = collector.wait_event("finished", 15s);
    ASSERT_TRUE(notice.has_value());
    EXPECT_EQ(notice->task_id, id);
    EXPECT_TRUE(notice->trigger_turn);
    EXPECT_EQ(notice->metadata.value("state", std::string{}), "completed");
    EXPECT_EQ(notice->metadata.value("exit_code", -1), 0);
    EXPECT_NE(notice->text.find("hello-background"), std::string::npos);
    const auto info = registry.find(id);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->state, BackgroundTaskState::Completed);
    {
        std::ifstream log(path_from_utf8(info->output_file), std::ios::binary);
        std::stringstream content;
        content << log.rdbuf();
        EXPECT_NE(content.str().find("hello-background"), std::string::npos);
    }
    EXPECT_EQ(registry.running_count(), 0u);
    registry.shutdown();
    std::filesystem::remove_all(dir);
}

// 场景:后台命令(长时间运行的脚本)生成了一个文档类文件后结束。
// 期望:完成通知的元数据里带 output_files(绝对路径),摘要面板的「产物」分组靠它
// 列出后台命令生成的文件;日志文件(.log)不算产物。
// 背景:前台 bash 在工具结果里记 output_files,转后台的命令没有工具结果可记,
// 只能记在完成通知上,否则跑得久的脚本(超时自动转后台)生成的文件永远列不出来。
TEST(BackgroundTaskRegistryTest, FinishedNoticeCarriesFilesTheCommandProduced) {
    const auto dir = unique_temp_dir("bg_outputs");
    NoticeCollector collector;
    BackgroundTaskRegistry registry;
    registry.connect(collector.sink());
    ShellLaunchFailure failure;
    const std::string id = registry.start_shell(shell_start("echo hello > bg-report.txt", dir), failure);
    ASSERT_FALSE(id.empty()) << failure.message;

    const auto notice = collector.wait_event("finished", 15s);
    ASSERT_TRUE(notice.has_value());
    ASSERT_TRUE(notice->metadata.contains("output_files")) << notice->metadata.dump();
    const auto files = notice->metadata["output_files"].get<std::vector<std::string>>();
    ASSERT_EQ(files.size(), 1u) << notice->metadata.dump();
    EXPECT_EQ(path_from_utf8(files[0]).filename(), std::filesystem::path("bg-report.txt"));
    EXPECT_TRUE(path_from_utf8(files[0]).is_absolute());
    registry.shutdown();
    std::filesystem::remove_all(dir);
}

// 场景:模型 / 用户停止一个还在跑的后台命令。期望:几秒内结束,通知状态 stopped;
// 再停一次报「不在运行」。
TEST(BackgroundTaskRegistryTest, StopKillsRunningCommandAndNotifiesStopped) {
    const auto dir = unique_temp_dir("bg_stop");
    NoticeCollector collector;
    BackgroundTaskRegistry registry;
    registry.connect(collector.sink());
    ShellLaunchFailure failure;
    const std::string id = registry.start_shell(shell_start(kLongCommand, dir), failure);
    ASSERT_FALSE(id.empty()) << failure.message;
    EXPECT_EQ(registry.running_count(), 1u);
    std::string error;
    ASSERT_TRUE(registry.stop(id, error)) << error;
    const auto notice = collector.wait_event("finished", 15s);
    ASSERT_TRUE(notice.has_value());
    EXPECT_EQ(notice->metadata.value("state", std::string{}), "stopped");
    EXPECT_FALSE(registry.stop(id, error));
    registry.shutdown();
    std::filesystem::remove_all(dir);
}

// 场景:run_in_background 给了 timeout_ms(= 后台运行上限)。期望:到点被停掉,
// 停止原因是 limit,通知里写明「达到后台运行上限」。
TEST(BackgroundTaskRegistryTest, MaxRuntimeStopsCommandWithLimitReason) {
    const auto dir = unique_temp_dir("bg_limit");
    NoticeCollector collector;
    BackgroundTaskRegistry registry;
    registry.connect(collector.sink());
    auto start = shell_start(kLongCommand, dir);
    start.max_runtime_ms = 300;
    ShellLaunchFailure failure;
    ASSERT_FALSE(registry.start_shell(std::move(start), failure).empty()) << failure.message;
    const auto notice = collector.wait_event("finished", 15s);
    ASSERT_TRUE(notice.has_value());
    EXPECT_EQ(notice->metadata.value("stop_reason", std::string{}), "limit");
    EXPECT_NE(notice->text.find("background time limit"), std::string::npos);
    registry.shutdown();
    std::filesystem::remove_all(dir);
}

// 场景:monitor=true 盯日志。期望:命令还没结束,先出现的整行就以 output 通知送达;
// 结束时照常有 finished 通知。
TEST(BackgroundTaskRegistryTest, MonitorDeliversNewLinesWhileRunning) {
    const auto dir = unique_temp_dir("bg_monitor");
    NoticeCollector collector;
    BackgroundRegistryOptions options;
    options.monitor_batch_window = 100ms;
    BackgroundTaskRegistry registry(options);
    registry.connect(collector.sink());
    auto start = shell_start(kTwoLinesWithGap, dir);
    start.monitor = true;
    ShellLaunchFailure failure;
    ASSERT_FALSE(registry.start_shell(std::move(start), failure).empty()) << failure.message;
    const auto output = collector.wait_event("output", 15s);
    ASSERT_TRUE(output.has_value());
    EXPECT_NE(output->text.find("first"), std::string::npos);
    EXPECT_TRUE(collector.wait_event("finished", 15s).has_value());
    registry.shutdown();
    std::filesystem::remove_all(dir);
}

// 场景:后台命令长时间没有任何输出(可能在等输入或卡住)。期望:到阈值后提醒
// 模型一次(event=quiet),之后间隔翻倍,不会每次轮询都提醒。
TEST(BackgroundTaskRegistryTest, QuietCommandGetsOneCheckinPerDoubledInterval) {
    const auto dir = unique_temp_dir("bg_quiet");
    NoticeCollector collector;
    BackgroundRegistryOptions options;
    options.quiet_checkin_after = 300ms;
    BackgroundTaskRegistry registry(options);
    registry.connect(collector.sink());
    ShellLaunchFailure failure;
    const std::string id = registry.start_shell(shell_start(kLongCommand, dir), failure);
    ASSERT_FALSE(id.empty()) << failure.message;
    const auto quiet = collector.wait_event("quiet", 10s);
    ASSERT_TRUE(quiet.has_value());
    EXPECT_NE(quiet->text.find("no output"), std::string::npos);
    std::this_thread::sleep_for(500ms);
    // 300ms 后提醒一次,下一次要到 900ms;第一次之后 500ms 内不会再来。
    EXPECT_EQ(collector.count("quiet"), 1u);
    std::string error;
    registry.stop(id, error);
    registry.shutdown();
    std::filesystem::remove_all(dir);
}

// 场景:spawn_subagent(wait=false) 的子代理在后台跑完。期望:观察项轮询到结果后
// 投 kind=agent 的 finished 通知,带最终答复;如果 wait_subagent 已经同步拿到
// 结果(consume_agent_result),就不再重复通知。
TEST(BackgroundTaskRegistryTest, AgentWatchNotifiesUnlessResultWasConsumed) {
    NoticeCollector collector;
    BackgroundRegistryOptions options;
    options.agent_poll_interval = 20ms;
    BackgroundTaskRegistry registry(options);
    registry.connect(collector.sink());

    auto polls = std::make_shared<std::atomic<int>>(0);
    BackgroundTaskRegistry::AgentWatch watch;
    watch.child_session_id = "child-1";
    watch.title = "调研 opencode";
    watch.poll = [polls]() -> std::optional<BackgroundAgentOutcome> {
        if (++*polls < 3) return std::nullopt;
        return BackgroundAgentOutcome{BackgroundTaskState::Completed, "结论:三点"};
    };
    ASSERT_EQ(registry.watch_agent(std::move(watch)), "child-1");
    const auto notice = collector.wait_event("finished", 5s);
    ASSERT_TRUE(notice.has_value());
    EXPECT_EQ(notice->metadata.value("kind", std::string{}), "agent");
    EXPECT_NE(notice->text.find("结论:三点"), std::string::npos);

    auto release = std::make_shared<std::atomic<bool>>(false);
    BackgroundTaskRegistry::AgentWatch consumed;
    consumed.child_session_id = "child-2";
    consumed.poll = [release]() -> std::optional<BackgroundAgentOutcome> {
        if (!release->load()) return std::nullopt;
        return BackgroundAgentOutcome{BackgroundTaskState::Completed, "done"};
    };
    registry.watch_agent(std::move(consumed));
    registry.consume_agent_result("child-2");
    release->store(true);
    std::this_thread::sleep_for(300ms);
    EXPECT_EQ(collector.count("finished"), 1u);
    EXPECT_EQ(registry.find("child-2")->state, BackgroundTaskState::Completed);
    registry.shutdown();
}

// 场景:模型显式 run_in_background。期望:工具立即成功返回任务 ID 与输出文件,
// 提示不要轮询;命令结束后登记表投出通知。
TEST(BashToolBackground, RunInBackgroundReturnsImmediatelyWithTaskId) {
    const auto dir = unique_temp_dir("bash_bg");
    NoticeCollector collector;
    BackgroundTaskRegistry registry;
    registry.connect(collector.sink());
    ToolImpl tool = create_bash_tool();
    const auto ctx = background_context(registry, dir);
    const auto started = std::chrono::steady_clock::now();
    ToolResult result = tool.execute(nlohmann::json{
        {"command", "echo from-background-tool"}, {"run_in_background", true}}.dump(), ctx);
    EXPECT_LT(std::chrono::steady_clock::now() - started, 5s);
    ASSERT_TRUE(result.success) << result.output;
    const std::string id = result.metadata.value("background_task_id", std::string{});
    ASSERT_FALSE(id.empty());
    EXPECT_NE(result.output.find("running in the background"), std::string::npos);
    EXPECT_NE(result.output.find("Do not poll"), std::string::npos);
    const auto notice = collector.wait_event("finished", 15s);
    ASSERT_TRUE(notice.has_value());
    EXPECT_NE(notice->text.find("from-background-tool"), std::string::npos);
    registry.shutdown();
    std::filesystem::remove_all(dir);
}

// 场景:前台命令跑到 timeout_ms 还没完。期望:不杀进程,而是转进后台
// (origin=timeout),工具结果说明这一点;登记表里多一个运行中的任务。
// 回归:以前这里会杀掉进程并报「timed out」,长构建的进度全丢。
TEST(BashToolBackground, ForegroundTimeoutMovesCommandToBackground) {
    const auto dir = unique_temp_dir("bash_timeout_bg");
    NoticeCollector collector;
    BackgroundTaskRegistry registry;
    registry.connect(collector.sink());
    ToolImpl tool = create_bash_tool();
    const auto ctx = background_context(registry, dir);
    ToolResult result = tool.execute(nlohmann::json{
        {"command", kLongToolCommand}, {"timeout_ms", 400}}.dump(), ctx);
    ASSERT_TRUE(result.success) << result.output;
    EXPECT_EQ(result.metadata.value("background_origin", std::string{}), "timeout");
    EXPECT_NE(result.output.find("was moved to the background"), std::string::npos);
    EXPECT_EQ(result.output.find("timed out after"), std::string::npos);
    EXPECT_EQ(registry.running_count(), 1u);
    std::string error;
    registry.stop(result.metadata.value("background_task_id", std::string{}), error);
    registry.shutdown();
    std::filesystem::remove_all(dir);
}

// 场景:前台命令运行中用户插话(AgentLoop 递增「转后台」信号)。期望:命令立即
// 转进后台(origin=user_message),工具不必等命令结束就返回。
TEST(BashToolBackground, BackgroundSignalHandsRunningCommandOver) {
    const auto dir = unique_temp_dir("bash_signal_bg");
    BackgroundTaskRegistry registry;
    NoticeCollector collector;
    registry.connect(collector.sink());
    std::atomic<std::uint64_t> generation{0};
    std::atomic<int> reason{0};
    ToolImpl tool = create_bash_tool();
    const auto ctx = background_context(registry, dir, &generation, &reason);
    std::thread signaller([&generation, &reason] {
        std::this_thread::sleep_for(500ms);
        reason.store(static_cast<int>(BackgroundOrigin::UserMessage));
        generation.fetch_add(1);
    });
    const auto started = std::chrono::steady_clock::now();
    ToolResult result = tool.execute(nlohmann::json{{"command", kLongToolCommand}}.dump(), ctx);
    signaller.join();
    EXPECT_LT(std::chrono::steady_clock::now() - started, 10s);
    ASSERT_TRUE(result.success) << result.output;
    EXPECT_EQ(result.metadata.value("background_origin", std::string{}), "user_message");
    EXPECT_EQ(registry.running_count(), 1u);
    std::string error;
    registry.stop(result.metadata.value("background_task_id", std::string{}), error);
    registry.shutdown();
    std::filesystem::remove_all(dir);
}

// 场景:模型用开头的长 sleep 等待。期望:有后台任务能力时直接拒绝并说明正确做法;
// 没有登记表(侧边对话等)时不拦,也不提供 run_in_background。
TEST(BashToolBackground, LongLeadingSleepIsRejectedOnlyWhenBackgroundIsAvailable) {
    const auto dir = unique_temp_dir("bash_sleep_block");
    BackgroundTaskRegistry registry;
    ToolImpl tool = create_bash_tool();
    ToolResult blocked = tool.execute(nlohmann::json{{"command", "sleep 90"}}.dump(),
                                      background_context(registry, dir));
    EXPECT_FALSE(blocked.success);
    EXPECT_NE(blocked.output.find("run_in_background"), std::string::npos);

    ToolContext plain;
    plain.cwd = path_to_utf8(dir);
    ToolResult unsupported = tool.execute(nlohmann::json{
        {"command", "echo x"}, {"run_in_background", true}}.dump(), plain);
    EXPECT_FALSE(unsupported.success);
    EXPECT_NE(unsupported.output.find("not available"), std::string::npos);
    registry.shutdown();
    std::filesystem::remove_all(dir);
}
