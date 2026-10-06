#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "im/line/line_tunnel.hpp"

#include "im/http.hpp"
#include "platform/process/piped_process.hpp"
#include "platform/process/which.hpp"
#include "utils/encoding.hpp"
#include "utils/joining_thread.hpp"
#include "utils/logger.hpp"
#include "utils/utf8_path.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <thread>

namespace acecode::im::line {
namespace {

using Clock = std::chrono::steady_clock;

// 真实的 cloudflared 子进程:stdio 管道由 PipedProcess 管理(stderr 丢弃),
// 读线程持续读空 stdout,避免子进程写满管道后卡住。
class ChildTunnelProcess final : public TunnelProcess {
public:
    ~ChildTunnelProcess() override { stop(); }

    bool start(const std::vector<std::string>& argv, std::string* error) {
        platform::SpawnOptions spawn;
        spawn.argv = argv;
        if (!process_.start(spawn, error)) return false;
        reader_ = acecode::JoiningThread(&ChildTunnelProcess::drain, this);
        return true;
    }

    bool alive() override { return process_.started() && !process_.wait_exit(0); }

    void stop() override {
        if (!process_.started()) return;
        // 先杀进程让读线程从阻塞读里返回,等它退出后再关句柄。
        process_.kill_child();
        if (reader_.joinable()) reader_.join();
        process_.terminate();
    }

private:
    void drain() {
        char buffer[4096];
        while (process_.read_stdout(buffer, sizeof(buffer)) > 0) {
        }
    }

    platform::PipedProcess process_;
    acecode::JoiningThread reader_;
};

std::unique_ptr<TunnelProcess> launch_child(const std::vector<std::string>& argv, std::string* error) {
    auto process = std::make_unique<ChildTunnelProcess>();
    if (!process->start(argv, error)) return nullptr;
    return process;
}

HttpResponse metrics_get(std::uint16_t port, const std::string& path) {
    HttpRequest request;
    request.url = "http://127.0.0.1:" + std::to_string(port) + path;
    request.timeout = std::chrono::seconds(3);
    request.use_proxy = false;
    return http_send(request);
}

bool wait_cancellable(std::chrono::milliseconds duration, const std::function<bool()>& cancel) {
    const auto deadline = Clock::now() + duration;
    while (Clock::now() < deadline) {
        if (cancel && cancel()) return false;
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        std::this_thread::sleep_for((std::min)(left, std::chrono::milliseconds(50)));
    }
    return !(cancel && cancel());
}

bool hostname_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
           c == ':';
}

} // namespace

std::vector<std::string> cloudflared_install_candidates(const std::function<std::string(const char*)>& env) {
    std::vector<std::string> out;
    auto add = [&out](const std::string& base, const char* tail) {
        if (!base.empty()) out.push_back(base + tail);
    };
    add(env("LOCALAPPDATA"), "\\Microsoft\\WinGet\\Links\\cloudflared.exe");
    add(env("ProgramFiles(x86)"), "\\cloudflared\\cloudflared.exe");
    add(env("ProgramFiles"), "\\cloudflared\\cloudflared.exe");
    return out;
}

std::string locate_cloudflared() {
    if (const auto found = platform::which("cloudflared")) return *found;
#ifdef _WIN32
    const auto env = [](const char* name) { return getenv_utf8(name); };
    const std::vector<std::string> candidates = cloudflared_install_candidates(env);
#else
    const std::vector<std::string> candidates{"/opt/homebrew/bin/cloudflared", "/usr/local/bin/cloudflared",
                                              "/usr/bin/cloudflared"};
#endif
    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(path_from_utf8(candidate), ec)) return candidate;
    }
    return {};
}

std::vector<std::string> cloudflared_argv(const std::string& executable, std::uint16_t target_port,
                                          std::uint16_t metrics_port, const TunnelOptions& options) {
    std::vector<std::string> argv{executable, "tunnel", "--no-autoupdate"};
    if (!options.config_path.empty()) {
        argv.push_back("--config");
        argv.push_back(options.config_path);
    }
    // 指标服务只能在 127.0.0.1(它带 /debug/pprof 与 /config,绝不能暴露)。
    argv.push_back("--metrics");
    argv.push_back("127.0.0.1:" + std::to_string(metrics_port));
    if (!options.protocol.empty()) {
        argv.push_back("--protocol");
        argv.push_back(options.protocol);
    }
    if (!options.log_file.empty()) {
        argv.push_back("--logfile");
        argv.push_back(options.log_file);
    }
    // 用 127.0.0.1 而不是 localhost,避免解析成 ::1 而回调端口只监听 IPv4。
    argv.push_back("--url");
    argv.push_back("http://127.0.0.1:" + std::to_string(target_port));
    return argv;
}

std::optional<std::string> parse_quicktunnel_hostname(const std::string& body) {
    try {
        const auto json = nlohmann::json::parse(body);
        if (!json.is_object() || !json.contains("hostname") || !json["hostname"].is_string()) return std::nullopt;
        const auto host = json["hostname"].get<std::string>();
        if (host.empty() || host.size() > 253) return std::nullopt;
        if (!std::all_of(host.begin(), host.end(), hostname_char)) return std::nullopt;
        return host;
    } catch (...) {
        return std::nullopt;
    }
}

std::uint16_t pick_free_loopback_port() {
#ifdef _WIN32
    WSADATA data;
    if (::WSAStartup(MAKEWORD(2, 2), &data) != 0) return 0;
    std::uint16_t port = 0;
    const SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s != INVALID_SOCKET) {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        int length = sizeof(address);
        if (::bind(s, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
            ::getsockname(s, reinterpret_cast<sockaddr*>(&address), &length) == 0)
            port = ntohs(address.sin_port);
        ::closesocket(s);
    }
    ::WSACleanup();
    return port;
#else
    std::uint16_t port = 0;
    const int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s >= 0) {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        socklen_t length = sizeof(address);
        if (::bind(s, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
            ::getsockname(s, reinterpret_cast<sockaddr*>(&address), &length) == 0)
            port = ntohs(address.sin_port);
        ::close(s);
    }
    return port;
#endif
}

CloudflareTunnel::CloudflareTunnel(TunnelOptions options) : options_(std::move(options)) {}

CloudflareTunnel::~CloudflareTunnel() { stop(); }

std::string CloudflareTunnel::executable() {
    if (!options_.cloudflared_path.empty()) {
        std::error_code ec;
        return std::filesystem::is_regular_file(path_from_utf8(options_.cloudflared_path), ec)
                   ? options_.cloudflared_path
                   : std::string{};
    }
    return options_.locate ? options_.locate() : locate_cloudflared();
}

std::string CloudflareTunnel::start(std::uint16_t target_port, const std::function<bool()>& cancel,
                                    TunnelFailure* failure, std::string* error) {
    stop();
    auto fail = [failure, error](TunnelFailure kind, const std::string& reason) {
        if (failure) *failure = kind;
        if (error) *error = reason;
        return std::string{};
    };
    if (failure) *failure = TunnelFailure::None;
    const auto exe = executable();
    if (exe.empty()) return fail(TunnelFailure::Missing, kCloudflaredInstallHint);
    metrics_port_ = options_.metrics_port ? options_.metrics_port : pick_free_loopback_port();
    if (metrics_port_ == 0) return fail(TunnelFailure::Launch, "无法为 cloudflared 分配本机端口");

    std::string launch_error;
    const auto argv = cloudflared_argv(exe, target_port, metrics_port_, options_);
    process_ = options_.launch ? options_.launch(argv, &launch_error) : launch_child(argv, &launch_error);
    if (!process_) return fail(TunnelFailure::Launch, "无法启动 cloudflared:" + launch_error);
    LOG_INFO("[channels/line] cloudflared started, metrics on 127.0.0.1:" + std::to_string(metrics_port_));

    const auto deadline = Clock::now() + options_.ready_timeout;
    std::string hostname;
    while (true) {
        if (cancel && cancel()) {
            stop();
            return fail(TunnelFailure::Cancelled, "已取消");
        }
        if (!process_->alive()) {
            stop();
            LOG_WARN("[channels/line] cloudflared exited before the quick tunnel was ready");
            return fail(TunnelFailure::Exited,
                        "cloudflared 已退出(可能连不上 Cloudflare,或 trycloudflare.com 暂时限流)");
        }
        if (hostname.empty()) {
            const auto response = metrics_get(metrics_port_, "/quicktunnel");
            if (response.status == 200) {
                if (const auto host = parse_quicktunnel_hostname(response.body)) hostname = *host;
            }
        }
        if (!hostname.empty() && metrics_get(metrics_port_, "/ready").status == 200) break;
        if (Clock::now() >= deadline) {
            stop();
            LOG_WARN(std::string("[channels/line] cloudflared not ready in time (hostname ") +
                     (hostname.empty() ? "missing" : "assigned") + ")");
            return fail(TunnelFailure::Timeout, "等待 Cloudflare 隧道就绪超时");
        }
        if (!wait_cancellable(options_.poll_interval, cancel)) {
            stop();
            return fail(TunnelFailure::Cancelled, "已取消");
        }
    }
    public_url_ = options_.url_scheme + "://" + hostname;
    LOG_INFO("[channels/line] quick tunnel ready: " + public_url_);
    return public_url_;
}

bool CloudflareTunnel::alive() { return process_ && process_->alive(); }

bool CloudflareTunnel::ready() {
    if (!process_ || metrics_port_ == 0) return false;
    return metrics_get(metrics_port_, "/ready").status == 200;
}

void CloudflareTunnel::stop() {
    if (process_) {
        process_->stop();
        process_.reset();
        LOG_INFO("[channels/line] cloudflared stopped");
    }
    public_url_.clear();
}

} // namespace acecode::im::line
