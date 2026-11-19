#include <gtest/gtest.h>

#include "desktop/desktop_restart.hpp"
#include "desktop/window_foreground.hpp"
#include "test_support/utils/state_file_fixture.hpp"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using acecode::desktop::desktop_relaunched_after_update;
using acecode::desktop::kRelaunchedAfterUpdateArgument;
using acecode::desktop::launch_desktop_replacement;
using acecode::desktop::macos_app_bundle_for_executable;
using acecode::desktop::note_desktop_version_launch;
using acecode::desktop::valid_desktop_replacement_arguments;
using acecode::desktop::validate_desktop_restart_target;
using acecode::test_support::StateFileTest;
using acecode::test_support::write_raw;

namespace {

class TempDir {
public:
    TempDir() {
        std::random_device rd;
        std::mt19937 rng(rd());
        std::uniform_int_distribution<int> dist(0, 0x7FFFFFFF);
        for (int attempt = 0; attempt < 8; ++attempt) {
            fs::path candidate = fs::temp_directory_path() /
                ("acecode_desktop_restart_" + std::to_string(dist(rng)));
            std::error_code ec;
            if (fs::create_directories(candidate, ec) && !ec) {
                path_ = candidate;
                return;
            }
        }
        ADD_FAILURE() << "could not create unique temp dir";
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

} // namespace

TEST(DesktopRestart, RejectsEmptyAndMissingTargets) {
    auto empty = validate_desktop_restart_target({});
    EXPECT_FALSE(empty.ok);
    EXPECT_NE(empty.error.find("empty"), std::string::npos);

    TempDir temp;
    auto missing = validate_desktop_restart_target(temp.path() / "missing.exe");
    EXPECT_FALSE(missing.ok);
    EXPECT_NE(missing.error.find("does not exist"), std::string::npos);
}

#ifdef __APPLE__
TEST(DesktopRestart, CapturesCanonicalExecutablePath) {
    const auto executable = acecode::desktop::current_desktop_executable_path();
    ASSERT_FALSE(executable.empty());
    EXPECT_TRUE(executable.is_absolute());
    EXPECT_EQ(executable, fs::canonical(executable));
    EXPECT_TRUE(validate_desktop_restart_target(executable).ok);
}
#endif

TEST(DesktopRestart, RejectsDirectoryTarget) {
    TempDir temp;
    auto result = validate_desktop_restart_target(temp.path());
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("regular file"), std::string::npos);
}

TEST(DesktopRestart, AcceptsExecutableFileTarget) {
    TempDir temp;
#ifdef _WIN32
    fs::path executable = temp.path() / "acecode-desktop.exe";
#else
    fs::path executable = temp.path() / "acecode-desktop";
#endif
    std::ofstream(executable, std::ios::binary) << "stub";
#ifndef _WIN32
    fs::permissions(executable,
                    fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec,
                    fs::perm_options::replace);
#endif

    auto result = validate_desktop_restart_target(executable);
    EXPECT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(result.error.empty());
}

// 触发场景:升级后旧进程拉起新进程时带上 --relaunched-after-update。
// 期望行为:新进程从自己的 argv 认出这次启动不是用户亲手点的(要自己把窗口放到最前);
// argv[0] 是程序路径,不参与匹配;普通启动与别的开关不误判。
// 回归:升级完成后程序自动启动,窗口出现在别的窗口后面。
TEST(DesktopRestart, DetectsRelaunchArgument) {
    EXPECT_TRUE(desktop_relaunched_after_update(
        {"acecode-desktop.exe", kRelaunchedAfterUpdateArgument}));
    EXPECT_TRUE(desktop_relaunched_after_update(
        {"acecode-desktop.exe", "--webapp", kRelaunchedAfterUpdateArgument}));
    EXPECT_FALSE(desktop_relaunched_after_update({"acecode-desktop.exe"}));
    EXPECT_FALSE(desktop_relaunched_after_update({"acecode-desktop.exe", "--webapp"}));
    EXPECT_FALSE(desktop_relaunched_after_update({kRelaunchedAfterUpdateArgument}));
    EXPECT_FALSE(desktop_relaunched_after_update({}));
}

// 触发场景:给替换进程传参数。参数直接拼进 Windows 命令行、不做转义。
// 期望行为:只接受不需要引号的纯开关;带空格 / 引号 / 反斜杠 / 空串的一律拒绝,
// 且在创建进程之前就失败(不会带着拼坏的命令行启动)。
TEST(DesktopRestart, ReplacementArgumentsMustBePlainSwitches) {
    EXPECT_TRUE(valid_desktop_replacement_arguments({}));
    EXPECT_TRUE(valid_desktop_replacement_arguments({kRelaunchedAfterUpdateArgument}));

    std::string error;
    EXPECT_FALSE(valid_desktop_replacement_arguments({"--flag value"}, &error));
    EXPECT_NE(error.find("plain switch"), std::string::npos);
    EXPECT_FALSE(valid_desktop_replacement_arguments({"--flag=\"x\""}));
    EXPECT_FALSE(valid_desktop_replacement_arguments({"--path=C:\\x"}));
    EXPECT_FALSE(valid_desktop_replacement_arguments({""}));

    TempDir temp;
#ifdef _WIN32
    fs::path executable = temp.path() / "acecode-desktop.exe";
#else
    fs::path executable = temp.path() / "acecode-desktop";
#endif
    std::ofstream(executable, std::ios::binary) << "stub";
#ifndef _WIN32
    fs::permissions(executable,
                    fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec,
                    fs::perm_options::replace);
#endif
    error.clear();
    EXPECT_FALSE(launch_desktop_replacement(executable, {"--bad arg"}, &error));
    EXPECT_NE(error.find("plain switch"), std::string::npos);
}

// 触发场景:macOS 上要经 LaunchServices(open -n <bundle>)重启,先从可执行文件路径
// 推出外层 .app。
// 期望行为:只认 <名字>.app/Contents/MacOS/<可执行文件> 这一种布局;开发时直接跑的
// 裸二进制、相对路径、层级不对的路径都返回空(调用方退回直接 exec)。
TEST(DesktopRestart, ResolvesMacosBundleFromExecutableLayout) {
    const auto bundle = macos_app_bundle_for_executable(
        "/Applications/ACECode.app/Contents/MacOS/acecode-desktop");
    ASSERT_TRUE(bundle.has_value());
    EXPECT_EQ(bundle->generic_string(), "/Applications/ACECode.app");

    EXPECT_FALSE(macos_app_bundle_for_executable(
        "/Users/me/build/acecode-desktop").has_value());
    EXPECT_FALSE(macos_app_bundle_for_executable(
        "/Applications/ACECode.app/Contents/Resources/acecode-desktop").has_value());
    EXPECT_FALSE(macos_app_bundle_for_executable(
        "/Applications/ACECode/Contents/MacOS/acecode-desktop").has_value());
    EXPECT_FALSE(macos_app_bundle_for_executable(
        "ACECode.app/Contents/MacOS/acecode-desktop").has_value());
    EXPECT_FALSE(macos_app_bundle_for_executable({}).has_value());
}

// 触发场景:新版本装好后的第一次启动(包括被还不会传 --relaunched-after-update 的
// 旧版本拉起的那一次)。
// 期望行为:state.json 里记的版本与当前不同(或从没记过)时返回 true 并记下当前版本,
// 同一版本再启动返回 false —— 只有第一次启动强制把窗口放到最前。
TEST_F(StateFileTest, FirstLaunchOfAVersionIsReportedOnce) {
    EXPECT_TRUE(note_desktop_version_launch("1.0.30"));  // 从没记过
    EXPECT_FALSE(note_desktop_version_launch("1.0.30")); // 同一版本再启动
    EXPECT_TRUE(note_desktop_version_launch("1.0.31"));  // 升级后的第一次
    EXPECT_FALSE(note_desktop_version_launch("1.0.31"));
    EXPECT_FALSE(note_desktop_version_launch(""));       // 拿不到版本号时不触发

    // 字段类型被写坏:按「没记过」处理并修正,不抛异常。
    write_raw(path_, R"({"desktop_last_launch_version": 7})");
    EXPECT_TRUE(note_desktop_version_launch("1.0.31"));
    EXPECT_FALSE(note_desktop_version_launch("1.0.31"));
}

// 触发场景:窗口句柄无效(窗口已销毁 / 启动失败)时仍被要求显示并前置。
// 期望行为:show 回调恰好执行一次,不崩溃;Windows 上因没有可前置的窗口返回 false。
TEST(DesktopWindowForeground, InvalidWindowStillRunsShowOnce) {
    int shown = 0;
    const bool front = acecode::desktop::show_window_in_foreground(
        nullptr, [&shown] { ++shown; });
    EXPECT_EQ(shown, 1);
#ifdef _WIN32
    EXPECT_FALSE(front);
#else
    EXPECT_TRUE(front);
#endif
}

// 触发场景:主 WebView 失效后程序自己重启,新进程带 --relaunched-after-webview-failure。
// 期望行为:两种自动拉起都被认出来(新进程要自己把窗口放到最前),普通启动不误判。
TEST(DesktopRestart, RecognisesBothRelaunchSwitches) {
    using acecode::desktop::desktop_was_relaunched;
    using acecode::desktop::kRelaunchedAfterWebviewFailureArgument;
    EXPECT_TRUE(desktop_was_relaunched({"acecode-desktop.exe", kRelaunchedAfterUpdateArgument}));
    EXPECT_TRUE(desktop_was_relaunched(
        {"acecode-desktop.exe", kRelaunchedAfterWebviewFailureArgument}));
    EXPECT_FALSE(desktop_was_relaunched({"acecode-desktop.exe"}));
    EXPECT_FALSE(desktop_was_relaunched({kRelaunchedAfterWebviewFailureArgument}));
    // 新开关也必须是不需要转义的纯开关,否则拉起替身时会被拒绝。
    EXPECT_TRUE(valid_desktop_replacement_arguments({kRelaunchedAfterWebviewFailureArgument}));
}

// 触发场景:升级重启与 WebView 恢复重启几乎同时被请求。
// 期望行为:先到的原因生效,后到的被拒绝;None 不是一个可以请求的原因。
TEST(DesktopRestart, FirstRestartReasonWins) {
    using acecode::desktop::DesktopRestartReason;
    acecode::desktop::DesktopRestartRequest request;
    EXPECT_FALSE(request.pending());
    EXPECT_FALSE(request.request(DesktopRestartReason::None));
    EXPECT_TRUE(request.request(DesktopRestartReason::WebviewRecovery));
    EXPECT_FALSE(request.request(DesktopRestartReason::Upgrade));
    EXPECT_TRUE(request.pending());
    EXPECT_EQ(request.reason(), DesktopRestartReason::WebviewRecovery);
}

// 触发场景:GUI 循环结束后按重启原因收尾。
// 期望行为:普通退出不拉起替身;升级必须先停掉全部 daemon(它们跑的是旧版本);
// WebView 恢复保留 daemon 让替身接管,正在跑的回合不中断,并带上自己的开关。
// 回归点:若恢复重启也停 daemon,无人值守时跑到一半的任务会被自动重启打断。
TEST(DesktopRestart, ExitPlanKeepsDaemonsForWebviewRecovery) {
    using acecode::desktop::DesktopRestartReason;
    using acecode::desktop::plan_desktop_exit;

    const auto none = plan_desktop_exit(DesktopRestartReason::None);
    EXPECT_FALSE(none.relaunch);
    EXPECT_FALSE(none.keep_daemons_running);
    EXPECT_FALSE(none.stop_daemons_for_upgrade);

    const auto upgrade = plan_desktop_exit(DesktopRestartReason::Upgrade);
    EXPECT_TRUE(upgrade.relaunch);
    EXPECT_TRUE(upgrade.stop_daemons_for_upgrade);
    EXPECT_FALSE(upgrade.keep_daemons_running);
    EXPECT_STREQ(upgrade.relaunch_argument, kRelaunchedAfterUpdateArgument);

    const auto recovery = plan_desktop_exit(DesktopRestartReason::WebviewRecovery);
    EXPECT_TRUE(recovery.relaunch);
    EXPECT_TRUE(recovery.keep_daemons_running);
    EXPECT_FALSE(recovery.stop_daemons_for_upgrade);
    EXPECT_STREQ(recovery.relaunch_argument,
                 acecode::desktop::kRelaunchedAfterWebviewFailureArgument);
}

// 触发场景:浏览器进程一启动就退出,每个新进程都想再自动重启一次。
// 期望行为:限次记在 state.json 里跨进程生效,10 分钟内第 3 次被拒绝;
// 字段被写坏时按没有记录处理并修正。
TEST_F(StateFileTest, WebviewRecoveryRestartsAreLimitedInStateFile) {
    using acecode::desktop::note_webview_recovery_restart;
    const std::int64_t minute = 60 * 1000;
    EXPECT_TRUE(note_webview_recovery_restart(1000 * minute));
    EXPECT_TRUE(note_webview_recovery_restart(1001 * minute));
    EXPECT_FALSE(note_webview_recovery_restart(1002 * minute));
    EXPECT_TRUE(note_webview_recovery_restart(1011 * minute));  // 最早一次已过期

    write_raw(path_, R"({"desktop_webview_recovery_restarts": "broken"})");
    EXPECT_TRUE(note_webview_recovery_restart(2000 * minute));
}

// 触发场景:WebHost 报告主 WebView 已失效,回调决定要不要重启。
// 期望行为:自动触发受限次约束,超限后不再退出程序;用户从托盘主动要求时不受限;
// 重启目标不存在时什么都不做(否则程序退出后起不来)。
TEST_F(StateFileTest, WebviewRecoveryHandlerRespectsTheLimitUnlessTheUserAsks) {
    using acecode::desktop::DesktopRestartReason;
    using acecode::desktop::make_webview_recovery_handler;
    TempDir temp;
#ifdef _WIN32
    const fs::path executable = temp.path() / "acecode-desktop.exe";
#else
    const fs::path executable = temp.path() / "acecode-desktop";
#endif
    std::ofstream(executable, std::ios::binary) << "stub";
#ifndef _WIN32
    fs::permissions(executable,
                    fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec,
                    fs::perm_options::replace);
#endif

    int quits = 0;
    {
        acecode::desktop::DesktopRestartRequest request;
        const auto handler = make_webview_recovery_handler(
            request, temp.path() / "missing.exe", [&quits] { ++quits; });
        handler(/*user_initiated=*/true);
        EXPECT_EQ(quits, 0);
        EXPECT_FALSE(request.pending());
    }
    for (int round = 0; round < 2; ++round) {
        acecode::desktop::DesktopRestartRequest request;
        const auto handler =
            make_webview_recovery_handler(request, executable, [&quits] { ++quits; });
        handler(/*user_initiated=*/false);
        EXPECT_EQ(request.reason(), DesktopRestartReason::WebviewRecovery);
        handler(/*user_initiated=*/false);  // 已经在重启,不重复退出
    }
    EXPECT_EQ(quits, 2);

    acecode::desktop::DesktopRestartRequest request;
    const auto handler =
        make_webview_recovery_handler(request, executable, [&quits] { ++quits; });
    handler(/*user_initiated=*/false);  // 第 3 次自动重启:超限
    EXPECT_EQ(quits, 2);
    EXPECT_FALSE(request.pending());
    handler(/*user_initiated=*/true);   // 用户明确要求:放行
    EXPECT_EQ(quits, 3);
    EXPECT_EQ(request.reason(), DesktopRestartReason::WebviewRecovery);
}
