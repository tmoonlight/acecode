#include "remote_web_proxy.hpp"

#include "../daemon/platform.hpp"
#include "../utils/atomic_file.hpp"
#include "../utils/encoding.hpp"
#include "../utils/utf8_path.hpp"
#include "../utils/uuid.hpp"

#include <asio.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace acecode::web {
namespace {

using asio::ip::tcp;
using json = nlohmann::json;
namespace fs = std::filesystem;

constexpr std::size_t kProxyBufferBytes = 64 * 1024;
constexpr std::size_t kProxyMaxConnections = 512;
constexpr auto kProxyReadyTimeout = std::chrono::seconds(4);
constexpr auto kProxyReadyPoll = std::chrono::milliseconds(25);
constexpr auto kParentPoll = std::chrono::milliseconds(500);
constexpr auto kStopTimeout = std::chrono::seconds(2);

std::string one_line_error(std::string value) {
    for (char& ch : value) {
        if (ch == '\r' || ch == '\n' || ch == '\t') ch = ' ';
    }
    constexpr std::size_t kMaxErrorBytes = 512;
    if (value.size() > kMaxErrorBytes) value.resize(kMaxErrorBytes);
    return value;
}

bool parse_int_arg(
    const std::string& value,
    int minimum,
    int maximum,
    int& out) {
    try {
        std::size_t parsed = 0;
        const long long number = std::stoll(value, &parsed, 10);
        if (parsed != value.size() || number < minimum || number > maximum) {
            return false;
        }
        out = static_cast<int>(number);
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_pid_arg(const std::string& value, std::int64_t& out) {
    try {
        std::size_t parsed = 0;
        const long long number = std::stoll(value, &parsed, 10);
        if (parsed != value.size() || number <= 0) return false;
        out = static_cast<std::int64_t>(number);
        return true;
    } catch (...) {
        return false;
    }
}

std::optional<std::string> value_after(
    const std::string& token,
    const char* prefix) {
    const std::string marker(prefix);
    if (token.rfind(marker, 0) != 0) return std::nullopt;
    return token.substr(marker.size());
}

struct ProxyCommandOptions {
    int listen_port = -1;
    int target_port = 0;
    std::int64_t parent_pid = 0;
    std::string ready_file;
};

bool parse_proxy_command(
    const std::vector<std::string>& tokens,
    ProxyCommandOptions& options,
    std::string& error) {
    for (const auto& token : tokens) {
        if (auto value = value_after(token, "--listen-port=")) {
            if (!parse_int_arg(*value, 0, 65535, options.listen_port)) {
                error = "invalid --listen-port";
                return false;
            }
        } else if (auto value = value_after(token, "--target-port=")) {
            if (!parse_int_arg(*value, 1, 65535, options.target_port)) {
                error = "invalid --target-port";
                return false;
            }
        } else if (auto value = value_after(token, "--parent-pid=")) {
            if (!parse_pid_arg(*value, options.parent_pid)) {
                error = "invalid --parent-pid";
                return false;
            }
        } else if (auto value = value_after(token, "--ready-file=")) {
            options.ready_file = *value;
        } else {
            error = "unknown proxy argument: " + token;
            return false;
        }
    }
    if (options.listen_port < 0 || options.target_port <= 0 ||
        options.parent_pid <= 0 || options.ready_file.empty()) {
        error = "proxy requires listen-port, target-port, parent-pid, and ready-file";
        return false;
    }
    return true;
}

bool write_child_state(
    const std::string& path,
    bool ready,
    int port,
    bool ipv6,
    const std::string& error = {}) {
    json state{
        {"ready", ready},
        {"pid", acecode::daemon::current_pid()},
        {"port", port},
        {"ipv6", ipv6},
    };
    if (!error.empty()) state["error"] = one_line_error(error);
    return acecode::atomic_write_file(path, state.dump(), true);
}

class ProxyTunnel : public std::enable_shared_from_this<ProxyTunnel> {
public:
    ProxyTunnel(
        tcp::socket client,
        asio::io_context& io,
        int target_port,
        std::function<void()> on_close)
        : client_(std::move(client)),
          upstream_(io),
          target_port_(target_port),
          on_close_(std::move(on_close)) {}

    ~ProxyTunnel() { finish(); }

    void start() {
        asio::error_code ec;
        upstream_.open(tcp::v4(), ec);
        if (ec) return finish();
        upstream_.bind(
            tcp::endpoint(
                asio::ip::make_address_v4(kRemoteWebProxyUpstreamSource), 0),
            ec);
        if (ec) return finish();

        auto self = shared_from_this();
        upstream_.async_connect(
            tcp::endpoint(
                asio::ip::make_address_v4(kRemoteWebProxyTarget),
                static_cast<unsigned short>(target_port_)),
            [self](const asio::error_code& connect_error) {
                if (connect_error) return self->finish();
                self->pump_client_to_upstream();
                self->pump_upstream_to_client();
            });
    }

private:
    void pump_client_to_upstream() {
        auto self = shared_from_this();
        client_.async_read_some(
            asio::buffer(client_buffer_),
            [self](const asio::error_code& error, std::size_t bytes) {
                if (error || bytes == 0) return self->finish();
                asio::async_write(
                    self->upstream_,
                    asio::buffer(self->client_buffer_.data(), bytes),
                    [self](const asio::error_code& write_error, std::size_t) {
                        if (write_error) return self->finish();
                        self->pump_client_to_upstream();
                    });
            });
    }

    void pump_upstream_to_client() {
        auto self = shared_from_this();
        upstream_.async_read_some(
            asio::buffer(upstream_buffer_),
            [self](const asio::error_code& error, std::size_t bytes) {
                if (error || bytes == 0) return self->finish();
                asio::async_write(
                    self->client_,
                    asio::buffer(self->upstream_buffer_.data(), bytes),
                    [self](const asio::error_code& write_error, std::size_t) {
                        if (write_error) return self->finish();
                        self->pump_upstream_to_client();
                    });
            });
    }

    void finish() {
        if (closed_.exchange(true)) return;
        asio::error_code ignored;
        client_.shutdown(tcp::socket::shutdown_both, ignored);
        client_.close(ignored);
        upstream_.shutdown(tcp::socket::shutdown_both, ignored);
        upstream_.close(ignored);
        if (on_close_) on_close_();
    }

    tcp::socket client_;
    tcp::socket upstream_;
    int target_port_ = 0;
    std::function<void()> on_close_;
    std::array<unsigned char, kProxyBufferBytes> client_buffer_{};
    std::array<unsigned char, kProxyBufferBytes> upstream_buffer_{};
    std::atomic<bool> closed_{false};
};

std::vector<std::string> proxy_argv(
    const std::string& executable_path,
    int listen_port,
    int target_port,
    std::int64_t parent_pid,
    const std::string& ready_file) {
    return {
        executable_path,
        "--remote-web-proxy",
        "--listen-port=" + std::to_string(listen_port),
        "--target-port=" + std::to_string(target_port),
        "--parent-pid=" + std::to_string(parent_pid),
        "--ready-file=" + ready_file,
    };
}

#ifdef _WIN32

std::wstring quote_windows_arg(const std::wstring& value) {
    if (!value.empty() && value.find_first_of(L" \t\"") == std::wstring::npos) {
        return value;
    }
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (wchar_t ch : value) {
        if (ch == L'\\') {
            ++slashes;
            continue;
        }
        if (ch == L'\"') {
            result.append(slashes * 2 + 1, L'\\');
            result.push_back(ch);
            slashes = 0;
            continue;
        }
        result.append(slashes, L'\\');
        slashes = 0;
        result.push_back(ch);
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

std::int64_t spawn_proxy_child(
    const std::vector<std::string>& argv,
    std::string& error) {
    if (argv.empty()) {
        error = "proxy executable path is empty";
        return 0;
    }
    std::vector<std::wstring> wide;
    wide.reserve(argv.size());
    for (const auto& arg : argv) {
        std::wstring converted = acecode::utf8_to_wide(arg);
        if (converted.empty() && !arg.empty()) {
            error = "proxy argument cannot be converted to UTF-16";
            return 0;
        }
        wide.push_back(std::move(converted));
    }
    std::wstring command;
    for (std::size_t i = 0; i < wide.size(); ++i) {
        if (i) command.push_back(L' ');
        command += quote_windows_arg(wide[i]);
    }

    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    HANDLE nul = ::CreateFileW(
        L"NUL", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
        OPEN_EXISTING, 0, nullptr);
    if (nul == INVALID_HANDLE_VALUE) nul = nullptr;

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW |
        (nul ? STARTF_USESTDHANDLES : 0);
    startup.wShowWindow = SW_HIDE;
    if (nul) {
        startup.hStdInput = nul;
        startup.hStdOutput = nul;
        startup.hStdError = nul;
    }
    PROCESS_INFORMATION process{};
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    const BOOL started = ::CreateProcessW(
        wide.front().c_str(), mutable_command.data(), nullptr, nullptr,
        nul ? TRUE : FALSE,
        CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP,
        nullptr, nullptr, &startup, &process);
    if (nul) ::CloseHandle(nul);
    if (!started) {
        error = "CreateProcessW failed (error " +
            std::to_string(static_cast<unsigned long>(::GetLastError())) + ")";
        return 0;
    }
    const std::int64_t pid = process.dwProcessId;
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    return pid;
}

bool reap_if_exited(std::int64_t pid) {
    return !acecode::daemon::is_pid_alive(pid);
}

bool terminate_proxy_child(std::int64_t pid) {
    return pid <= 0 || acecode::daemon::terminate_pid(pid, 1500);
}

#else

std::int64_t spawn_proxy_child(
    const std::vector<std::string>& argv,
    std::string& error) {
    if (argv.empty()) {
        error = "proxy executable path is empty";
        return 0;
    }
    const pid_t pid = ::fork();
    if (pid < 0) {
        error = "fork failed (errno " + std::to_string(errno) + ")";
        return 0;
    }
    if (pid == 0) {
        int null_fd = ::open("/dev/null", O_RDWR);
        if (null_fd >= 0) {
            ::dup2(null_fd, STDIN_FILENO);
            ::dup2(null_fd, STDOUT_FILENO);
            ::dup2(null_fd, STDERR_FILENO);
            if (null_fd > STDERR_FILENO) ::close(null_fd);
        }
        std::vector<std::string> storage = argv;
        std::vector<char*> pointers;
        pointers.reserve(storage.size() + 1);
        for (auto& item : storage) pointers.push_back(item.data());
        pointers.push_back(nullptr);
        ::execv(storage.front().c_str(), pointers.data());
        ::_exit(127);
    }
    return static_cast<std::int64_t>(pid);
}

bool reap_if_exited(std::int64_t pid_value) {
    if (pid_value <= 0) return true;
    int status = 0;
    const pid_t pid = static_cast<pid_t>(pid_value);
    const pid_t result = ::waitpid(pid, &status, WNOHANG);
    return result == pid || (result < 0 && errno == ECHILD);
}

bool terminate_proxy_child(std::int64_t pid_value) {
    if (pid_value <= 0) return true;
    const pid_t pid = static_cast<pid_t>(pid_value);
    if (::kill(pid, SIGTERM) != 0 && errno == ESRCH) return true;
    const auto deadline = std::chrono::steady_clock::now() + kStopTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (reap_if_exited(pid_value)) return true;
        std::this_thread::sleep_for(kProxyReadyPoll);
    }
    if (::kill(pid, SIGKILL) != 0 && errno == ESRCH) return true;
    int status = 0;
    pid_t result = 0;
    do {
        result = ::waitpid(pid, &status, 0);
    } while (result < 0 && errno == EINTR);
    if (result == pid || (result < 0 && errno == ECHILD)) {
        return !acecode::daemon::is_pid_alive(pid_value);
    }
    return false;
}

#endif

std::optional<RemoteWebProxyStatus> read_child_state(
    const std::string& path,
    std::string& parse_error) {
    std::ifstream input(acecode::path_from_utf8(path), std::ios::binary);
    if (!input.is_open()) return std::nullopt;
    std::ostringstream content;
    content << input.rdbuf();
    try {
        const json state = json::parse(content.str());
        RemoteWebProxyStatus result;
        result.running = state.value("ready", false);
        result.port = state.value("port", 0);
        result.pid = state.value("pid", std::int64_t{0});
        result.ipv6 = state.value("ipv6", false);
        result.error = one_line_error(state.value("error", std::string{}));
        return result;
    } catch (const std::exception& exception) {
        parse_error = std::string("invalid proxy readiness state: ") +
            exception.what();
        return std::nullopt;
    }
}

} // namespace

struct RemoteWebTcpProxy::Impl {
    asio::io_context io;
    std::unique_ptr<tcp::acceptor> ipv4_acceptor;
    std::unique_ptr<tcp::acceptor> ipv6_acceptor;
    // A tunnel can be released while io_context destroys a cancelled handler.
    // Keep the counter independently owned so its close callback never reaches
    // into an Impl whose member destruction has already begun.
    std::shared_ptr<std::atomic<std::size_t>> active_connections =
        std::make_shared<std::atomic<std::size_t>>(0);
    int target_port = 0;
    int listen_port = 0;
    bool ipv6 = false;
    std::atomic<bool> stopped{false};

    bool open_ipv4(int requested_port, std::string& error) {
        asio::error_code ec;
        ipv4_acceptor = std::make_unique<tcp::acceptor>(io);
        ipv4_acceptor->open(tcp::v4(), ec);
        if (!ec) {
            ipv4_acceptor->set_option(asio::socket_base::reuse_address(false), ec);
        }
        if (!ec) {
            ipv4_acceptor->bind(
                tcp::endpoint(tcp::v4(), static_cast<unsigned short>(requested_port)),
                ec);
        }
        if (!ec) ipv4_acceptor->listen(asio::socket_base::max_listen_connections, ec);
        if (ec) {
            error = "cannot bind remote Web proxy IPv4 listener: " + ec.message();
            ipv4_acceptor.reset();
            return false;
        }
        listen_port = ipv4_acceptor->local_endpoint(ec).port();
        if (ec || listen_port <= 0) {
            error = "cannot read remote Web proxy listener port";
            ipv4_acceptor.reset();
            return false;
        }
        return true;
    }

    void open_ipv6_best_effort() {
        asio::error_code ec;
        auto acceptor = std::make_unique<tcp::acceptor>(io);
        acceptor->open(tcp::v6(), ec);
        if (!ec) acceptor->set_option(asio::ip::v6_only(true), ec);
        if (!ec) acceptor->set_option(asio::socket_base::reuse_address(false), ec);
        if (!ec) {
            acceptor->bind(
                tcp::endpoint(tcp::v6(), static_cast<unsigned short>(listen_port)),
                ec);
        }
        if (!ec) acceptor->listen(asio::socket_base::max_listen_connections, ec);
        if (ec) return;
        ipv6 = true;
        ipv6_acceptor = std::move(acceptor);
    }

    void begin_accept(tcp::acceptor& acceptor) {
        auto socket = std::make_shared<tcp::socket>(io);
        acceptor.async_accept(
            *socket,
            [this, &acceptor, socket](const asio::error_code& error) {
                if (!error && !stopped.load()) {
                    const std::size_t current =
                        active_connections->fetch_add(1);
                    if (current >= kProxyMaxConnections) {
                        active_connections->fetch_sub(1);
                        asio::error_code ignored;
                        socket->close(ignored);
                    } else {
                        const auto counter = active_connections;
                        auto tunnel = std::make_shared<ProxyTunnel>(
                            std::move(*socket), io, target_port,
                            [counter] { counter->fetch_sub(1); });
                        tunnel->start();
                    }
                }
                if (acceptor.is_open() && !stopped.load()) begin_accept(acceptor);
            });
    }
};

RemoteWebTcpProxy::RemoteWebTcpProxy()
    : impl_(std::make_unique<Impl>()) {}

RemoteWebTcpProxy::~RemoteWebTcpProxy() { stop(); }

bool RemoteWebTcpProxy::start(
    int listen_port,
    int target_port,
    std::string* error) {
    if (listen_port < 0 || listen_port > 65535 ||
        target_port <= 0 || target_port > 65535) {
        if (error) *error = "proxy port is out of range";
        return false;
    }
    if (listen_port != 0 && listen_port == target_port) {
        if (error) *error = "proxy listener port must differ from daemon port";
        return false;
    }
    std::string local_error;
    impl_->target_port = target_port;
    if (!impl_->open_ipv4(listen_port, local_error)) {
        if (error) *error = std::move(local_error);
        return false;
    }
    impl_->open_ipv6_best_effort();
    impl_->begin_accept(*impl_->ipv4_acceptor);
    if (impl_->ipv6_acceptor) impl_->begin_accept(*impl_->ipv6_acceptor);
    return true;
}

void RemoteWebTcpProxy::run() { impl_->io.run(); }

void RemoteWebTcpProxy::stop() {
    if (!impl_ || impl_->stopped.exchange(true)) return;
    auto* impl = impl_.get();
    asio::post(impl->io, [impl] {
        asio::error_code ignored;
        if (impl->ipv4_acceptor) impl->ipv4_acceptor->close(ignored);
        if (impl->ipv6_acceptor) impl->ipv6_acceptor->close(ignored);
        impl->io.stop();
    });
}

int RemoteWebTcpProxy::port() const {
    return impl_ ? impl_->listen_port : 0;
}

bool RemoteWebTcpProxy::ipv6_enabled() const {
    return impl_ && impl_->ipv6;
}

ManagedRemoteWebProxyController::ManagedRemoteWebProxyController(
    std::string executable_path,
    std::string runtime_dir,
    std::int64_t parent_pid)
    : executable_path_(std::move(executable_path)),
      runtime_dir_(std::move(runtime_dir)),
      parent_pid_(parent_pid) {}

ManagedRemoteWebProxyController::~ManagedRemoteWebProxyController() {
    std::lock_guard<std::mutex> lock(mu_);
    stop_locked();
}

RemoteWebProxyStatus ManagedRemoteWebProxyController::status_locked() {
    if (status_.pid > 0 && reap_if_exited(status_.pid)) {
        status_.running = false;
        status_.starting = false;
        status_.pid = 0;
        status_.port = 0;
        status_.ipv6 = false;
        if (status_.error.empty()) {
            status_.error = "remote Web proxy process exited unexpectedly";
        }
    }
    return status_;
}

RemoteWebProxyStatus ManagedRemoteWebProxyController::status() {
    std::lock_guard<std::mutex> lock(mu_);
    return status_locked();
}

RemoteWebProxyStatus ManagedRemoteWebProxyController::start(
    int configured_port,
    int target_port) {
    std::lock_guard<std::mutex> lock(mu_);
    auto current = status_locked();
    if (current.running &&
        (configured_port == 0 || current.port == configured_port)) {
        status_.configured = true;
        return status_;
    }
    if (!stop_locked()) {
        status_.configured = true;
        return status_;
    }
    status_.configured = true;

    if (configured_port < 0 || configured_port > 65535 ||
        target_port <= 0 || target_port > 65535 ||
        configured_port == target_port || executable_path_.empty() ||
        runtime_dir_.empty() || parent_pid_ <= 0) {
        status_.error = "invalid remote Web proxy process configuration";
        return status_;
    }

    if (configured_port != 0) {
        return start_candidate_locked(configured_port, target_port);
    }

    const int adjacent = target_port < 65535 ? target_port + 1 : target_port - 1;
    auto adjacent_status = start_candidate_locked(adjacent, target_port);
    if (adjacent_status.running) return adjacent_status;
    return start_candidate_locked(0, target_port);
}

RemoteWebProxyStatus ManagedRemoteWebProxyController::start_candidate_locked(
    int listen_port,
    int target_port) {
    if (!stop_locked()) {
        status_.configured = true;
        return status_;
    }
    status_.configured = true;
    status_.starting = true;
    status_.error.clear();

    const fs::path ready_path =
        acecode::path_from_utf8(runtime_dir_) /
        ("remote-web-proxy-" + acecode::generate_uuid_v7() + ".json");
    const std::string ready_file = acecode::path_to_utf8(ready_path);
    std::error_code filesystem_error;
    fs::create_directories(ready_path.parent_path(), filesystem_error);
    if (filesystem_error) {
        status_.starting = false;
        status_.error = "cannot create remote Web proxy runtime directory";
        return status_;
    }
    fs::remove(ready_path, filesystem_error);
    fs::remove(ready_path.string() + ".tmp", filesystem_error);

    std::string spawn_error;
    status_.pid = spawn_proxy_child(
        proxy_argv(
            executable_path_, listen_port, target_port, parent_pid_, ready_file),
        spawn_error);
    if (status_.pid <= 0) {
        status_.starting = false;
        status_.error = one_line_error(spawn_error.empty()
            ? "failed to start remote Web proxy process"
            : spawn_error);
        return status_;
    }

    const auto deadline = std::chrono::steady_clock::now() + kProxyReadyTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
        std::string state_error;
        if (auto child = read_child_state(ready_file, state_error)) {
            fs::remove(ready_path, filesystem_error);
            if (child->pid > 0 && child->pid != status_.pid) {
                status_.error = "remote Web proxy readiness PID mismatch";
                break;
            }
            status_.starting = false;
            status_.running = child->running;
            status_.port = child->port;
            status_.ipv6 = child->ipv6;
            status_.error = child->error;
            if (status_.running && status_.port > 0) return status_;
            if (status_.error.empty()) {
                status_.error = "remote Web proxy failed before becoming ready";
            }
            break;
        }
        if (!state_error.empty()) {
            status_.error = one_line_error(state_error);
            break;
        }
        if (reap_if_exited(status_.pid)) {
            status_.pid = 0;
            status_.error = "remote Web proxy process exited before readiness";
            break;
        }
        std::this_thread::sleep_for(kProxyReadyPoll);
    }

    if (status_.error.empty()) {
        status_.error = "timed out waiting for remote Web proxy readiness";
    }
    fs::remove(ready_path, filesystem_error);
    fs::remove(ready_path.string() + ".tmp", filesystem_error);
    const std::int64_t failed_pid = status_.pid;
    if (failed_pid > 0 && !terminate_proxy_child(failed_pid)) {
        status_.pid = failed_pid;
        status_.running = acecode::daemon::is_pid_alive(failed_pid);
        status_.starting = false;
        status_.port = 0;
        status_.ipv6 = false;
        status_.error += "; failed to stop proxy process";
        return status_;
    }
    status_.pid = 0;
    status_.port = 0;
    status_.ipv6 = false;
    status_.running = false;
    status_.starting = false;
    return status_;
}

RemoteWebProxyStatus ManagedRemoteWebProxyController::stop() {
    std::lock_guard<std::mutex> lock(mu_);
    stop_locked();
    status_.configured = false;
    return status_;
}

bool ManagedRemoteWebProxyController::stop_locked() {
    const std::int64_t pid = status_.pid;
    if (pid > 0 && !terminate_proxy_child(pid) &&
        acecode::daemon::is_pid_alive(pid)) {
        status_.running = true;
        status_.starting = false;
        status_.error = "failed to stop remote Web proxy process";
        return false;
    }
    status_.running = false;
    status_.starting = false;
    status_.ipv6 = false;
    status_.port = 0;
    status_.pid = 0;
    status_.error.clear();
    return true;
}

int run_remote_web_proxy_command(
    const std::vector<std::string>& tokens,
    std::ostream& out,
    std::ostream& err) {
    ProxyCommandOptions options;
    std::string parse_error;
    if (!parse_proxy_command(tokens, options, parse_error)) {
        err << "acecode --remote-web-proxy: " << parse_error << '\n';
        return 64;
    }
    if (!acecode::daemon::is_pid_alive(options.parent_pid)) {
        write_child_state(
            options.ready_file, false, 0, false,
            "owning daemon process is not running");
        return 2;
    }

    RemoteWebTcpProxy proxy;
    std::string start_error;
    if (!proxy.start(options.listen_port, options.target_port, &start_error)) {
        write_child_state(
            options.ready_file, false, 0, false, start_error);
        return 3;
    }
    if (!write_child_state(
            options.ready_file, true, proxy.port(), proxy.ipv6_enabled())) {
        err << "acecode --remote-web-proxy: cannot publish readiness\n";
        return 4;
    }
    out << "remote Web proxy listening on " << proxy.port() << '\n';

    std::atomic<bool> monitor_stop{false};
    std::thread parent_monitor([&] {
        while (!monitor_stop.load()) {
            if (!acecode::daemon::is_pid_alive(options.parent_pid)) {
                proxy.stop();
                return;
            }
            std::this_thread::sleep_for(kParentPoll);
        }
    });
    proxy.run();
    monitor_stop.store(true);
    if (parent_monitor.joinable()) parent_monitor.join();
    return 0;
}

} // namespace acecode::web
