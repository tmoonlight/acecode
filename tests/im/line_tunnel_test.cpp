#include <gtest/gtest.h>

#include "im/line/line_tunnel.hpp"
#include "test_support/im/fake_line_server.hpp"

#include <algorithm>
#include <atomic>
#include <map>

// im/line/line_tunnel:Cloudflare 快速隧道管理(假 cloudflared:指标服务 + 假子进程)。
// 覆盖:命令行参数、/quicktunnel 解析、安装位置候选、启动并取得公网地址、未安装、
// 进程提前退出、就绪超时、取消、空闲端口。不会启动真实 cloudflared,也不连 Cloudflare。

namespace acecode::im::line {
namespace {

using test::FakeCloudflared;

TunnelOptions fake_options(const FakeCloudflared& fake) {
    TunnelOptions options;
    options.locate = [] { return std::string("C:/tools/cloudflared.exe"); };
    options.launch = fake.launcher();
    options.metrics_port = fake.metrics_port();
    options.url_scheme = "http";
    options.ready_timeout = std::chrono::seconds(3);
    options.poll_interval = std::chrono::milliseconds(20);
    return options;
}

bool has_pair(const std::vector<std::string>& argv, const std::string& flag, const std::string& value) {
    for (std::size_t i = 0; i + 1 < argv.size(); ++i)
        if (argv[i] == flag && argv[i + 1] == value) return true;
    return false;
}

// 场景:按选项拼 cloudflared 命令行(含可选的协议、配置文件、日志文件)。
// 期望:关闭自动更新;指标服务只在 127.0.0.1;目标用 127.0.0.1 而不是 localhost(避免解析到 ::1);
// 可选参数只在设置时出现。
TEST(LineTunnel, BuildsCommandLine) {
    TunnelOptions options;
    auto argv = cloudflared_argv("cloudflared", 8123, 20241, options);
    EXPECT_EQ(argv[0], "cloudflared");
    EXPECT_EQ(argv[1], "tunnel");
    EXPECT_NE(std::find(argv.begin(), argv.end(), "--no-autoupdate"), argv.end());
    EXPECT_TRUE(has_pair(argv, "--metrics", "127.0.0.1:20241"));
    EXPECT_TRUE(has_pair(argv, "--url", "http://127.0.0.1:8123"));
    EXPECT_EQ(std::find(argv.begin(), argv.end(), "--protocol"), argv.end());

    options.protocol = "http2";
    options.config_path = "C:/data/empty.yml";
    options.log_file = "C:/data/cloudflared.log";
    argv = cloudflared_argv("cloudflared", 8123, 20241, options);
    EXPECT_TRUE(has_pair(argv, "--protocol", "http2"));
    EXPECT_TRUE(has_pair(argv, "--config", "C:/data/empty.yml"));
    EXPECT_TRUE(has_pair(argv, "--logfile", "C:/data/cloudflared.log"));
    EXPECT_EQ(argv.back(), "http://127.0.0.1:8123");
}

// 场景:指标服务 /quicktunnel 的几种应答。
// 期望:只接受非空、只含主机名字符的值;未分配(空串)、带路径或注入字符、非 JSON 一律拒绝。
TEST(LineTunnel, ParsesQuickTunnelHostname) {
    EXPECT_EQ(parse_quicktunnel_hostname(R"({"hostname":"abc-def.trycloudflare.com"})").value_or(""),
              "abc-def.trycloudflare.com");
    EXPECT_FALSE(parse_quicktunnel_hostname(R"({"hostname":""})").has_value());
    EXPECT_FALSE(parse_quicktunnel_hostname(R"({"hostname":"evil.com/path"})").has_value());
    EXPECT_FALSE(parse_quicktunnel_hostname(R"({"hostname":"a b"})").has_value());
    EXPECT_FALSE(parse_quicktunnel_hostname("OK\n").has_value());
}

// 场景:Windows 上 winget 便携版、MSI(x86 / x64 目录)的安装位置;某个环境变量缺失。
// 期望:按 WinGet Links → Program Files (x86) → Program Files 的顺序给出候选;缺失的变量跳过。
TEST(LineTunnel, InstallCandidatesFollowWindowsLayout) {
    std::map<std::string, std::string> env{{"LOCALAPPDATA", "C:\\Users\\u\\AppData\\Local"},
                                           {"ProgramFiles", "C:\\Program Files"}};
    const auto candidates = cloudflared_install_candidates([&env](const char* name) {
        const auto it = env.find(name);
        return it == env.end() ? std::string{} : it->second;
    });
    ASSERT_EQ(candidates.size(), 2u);
    EXPECT_EQ(candidates[0], "C:\\Users\\u\\AppData\\Local\\Microsoft\\WinGet\\Links\\cloudflared.exe");
    EXPECT_EQ(candidates[1], "C:\\Program Files\\cloudflared\\cloudflared.exe");
}

// 场景:启动隧道,假 cloudflared 很快分配主机名并就绪。
// 期望:返回 <scheme>://<主机名>;启动参数指向给定的回调端口;stop 后进程被结束、地址清空。
TEST(LineTunnel, StartsAndReportsPublicUrl) {
    FakeCloudflared fake;
    CloudflareTunnel tunnel(fake_options(fake));
    TunnelFailure failure = TunnelFailure::Cancelled;
    std::string error;
    const auto url = tunnel.start(9555, {}, &failure, &error);
    EXPECT_EQ(url, "http://127.0.0.1:9555") << error;
    EXPECT_EQ(failure, TunnelFailure::None);
    EXPECT_TRUE(tunnel.alive());
    EXPECT_TRUE(tunnel.ready());
    EXPECT_TRUE(has_pair(fake.last_argv(), "--url", "http://127.0.0.1:9555"));
    EXPECT_TRUE(has_pair(fake.last_argv(), "--metrics", "127.0.0.1:" + std::to_string(fake.metrics_port())));
    tunnel.stop();
    EXPECT_FALSE(tunnel.alive());
    EXPECT_TRUE(tunnel.public_url().empty());
}

// 场景:电脑上没装 cloudflared。
// 期望:失败类型为 Missing,原因里给出 winget 安装命令与“填写自己的公网地址”两条出路。
TEST(LineTunnel, MissingCloudflaredGivesInstallHint) {
    FakeCloudflared fake;
    auto options = fake_options(fake);
    options.locate = [] { return std::string{}; };
    CloudflareTunnel tunnel(options);
    TunnelFailure failure = TunnelFailure::None;
    std::string error;
    EXPECT_TRUE(tunnel.start(9555, {}, &failure, &error).empty());
    EXPECT_EQ(failure, TunnelFailure::Missing);
    EXPECT_NE(error.find("winget install --id Cloudflare.cloudflared"), std::string::npos);
    EXPECT_NE(error.find("公网"), std::string::npos);
    EXPECT_EQ(fake.launch_count(), 0u);
}

// 场景:cloudflared 启动后还没就绪就退出了(例如 trycloudflare.com 限流)。
// 期望:不会一直等到超时,立即以 Exited 失败返回。
TEST(LineTunnel, ProcessExitBeforeReadyFailsFast) {
    FakeCloudflared fake;
    fake.state()->hostname_for = [](int, const std::string&) { return std::string{}; };  // 一直拿不到主机名
    auto options = fake_options(fake);
    options.ready_timeout = std::chrono::seconds(30);
    auto launch = options.launch;
    options.launch = [launch](const std::vector<std::string>& argv, std::string* error) {
        auto process = launch(argv, error);
        process->stop();  // 立刻“退出”
        return process;
    };
    CloudflareTunnel tunnel(options);
    TunnelFailure failure = TunnelFailure::None;
    std::string error;
    const auto begin = std::chrono::steady_clock::now();
    EXPECT_TRUE(tunnel.start(9555, {}, &failure, &error).empty());
    EXPECT_EQ(failure, TunnelFailure::Exited);
    EXPECT_LT(std::chrono::steady_clock::now() - begin, std::chrono::seconds(5));
}

// 场景:主机名已分配,但一直连不上 Cloudflare 边缘(/ready 503)。
// 期望:等到 ready_timeout 后以 Timeout 失败,并结束子进程。
TEST(LineTunnel, NeverReadyTimesOut) {
    FakeCloudflared fake;
    fake.state()->ready = false;
    auto options = fake_options(fake);
    options.ready_timeout = std::chrono::milliseconds(300);
    CloudflareTunnel tunnel(options);
    TunnelFailure failure = TunnelFailure::None;
    std::string error;
    EXPECT_TRUE(tunnel.start(9555, {}, &failure, &error).empty());
    EXPECT_EQ(failure, TunnelFailure::Timeout);
    EXPECT_FALSE(tunnel.alive());
}

// 场景:等待就绪期间用户关闭了通道(取消)。
// 期望:很快以 Cancelled 返回并结束子进程。
TEST(LineTunnel, CancelStopsWaiting) {
    FakeCloudflared fake;
    fake.state()->ready = false;
    auto options = fake_options(fake);
    options.ready_timeout = std::chrono::seconds(30);
    CloudflareTunnel tunnel(options);
    std::atomic<int> polls{0};
    TunnelFailure failure = TunnelFailure::None;
    std::string error;
    const auto begin = std::chrono::steady_clock::now();
    EXPECT_TRUE(tunnel.start(9555, [&polls] { return ++polls > 5; }, &failure, &error).empty());
    EXPECT_EQ(failure, TunnelFailure::Cancelled);
    EXPECT_FALSE(tunnel.alive());
    EXPECT_LT(std::chrono::steady_clock::now() - begin, std::chrono::seconds(5));
}

// 场景:启动子进程本身失败。
// 期望:以 Launch 失败,原因带上启动错误。
TEST(LineTunnel, LaunchFailureIsReported) {
    FakeCloudflared fake;
    fake.state()->fail_launch = true;
    CloudflareTunnel tunnel(fake_options(fake));
    TunnelFailure failure = TunnelFailure::None;
    std::string error;
    EXPECT_TRUE(tunnel.start(9555, {}, &failure, &error).empty());
    EXPECT_EQ(failure, TunnelFailure::Launch);
    EXPECT_NE(error.find("fake launch failure"), std::string::npos);
}

// 场景:为 cloudflared 指标服务挑本机空闲端口。
// 期望:返回非 0 端口。
TEST(LineTunnel, PicksFreeLoopbackPort) {
    EXPECT_NE(pick_free_loopback_port(), 0);
}

} // namespace
} // namespace acecode::im::line
