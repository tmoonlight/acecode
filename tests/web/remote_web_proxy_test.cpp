#include "daemon/platform.hpp"
#include "web/remote_web_proxy.hpp"

#include <gtest/gtest.h>

#include <asio.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

namespace {

using asio::ip::tcp;
namespace fs = std::filesystem;

class EchoSession : public std::enable_shared_from_this<EchoSession> {
public:
    EchoSession(tcp::socket socket, std::function<void(std::string)> peer_seen)
        : socket_(std::move(socket)), peer_seen_(std::move(peer_seen)) {}

    void start() {
        asio::error_code ec;
        peer_seen_(socket_.remote_endpoint(ec).address().to_string());
        read();
    }

private:
    void read() {
        auto self = shared_from_this();
        socket_.async_read_some(
            asio::buffer(buffer_),
            [self](const asio::error_code& error, std::size_t bytes) {
                if (error || bytes == 0) return;
                asio::async_write(
                    self->socket_,
                    asio::buffer(self->buffer_.data(), bytes),
                    [self](const asio::error_code& write_error, std::size_t) {
                        if (!write_error) self->read();
                    });
            });
    }

    tcp::socket socket_;
    std::function<void(std::string)> peer_seen_;
    std::array<unsigned char, 64 * 1024> buffer_{};
};

class EchoServer {
public:
    EchoServer()
        : acceptor_(io_, tcp::endpoint(
              asio::ip::make_address_v4("127.0.0.1"), 0)) {
        begin_accept();
        thread_ = std::thread([this] { io_.run(); });
    }

    ~EchoServer() {
        asio::post(io_, [this] {
            asio::error_code ignored;
            acceptor_.close(ignored);
            io_.stop();
        });
        if (thread_.joinable()) thread_.join();
    }

    int port() const {
        asio::error_code ec;
        return acceptor_.local_endpoint(ec).port();
    }

    std::string peer_address() const {
        std::lock_guard<std::mutex> lock(peer_mu_);
        return peer_address_;
    }

private:
    void begin_accept() {
        auto socket = std::make_shared<tcp::socket>(io_);
        acceptor_.async_accept(
            *socket,
            [this, socket](const asio::error_code& error) {
                if (!error) {
                    auto session = std::make_shared<EchoSession>(
                        std::move(*socket),
                        [this](std::string peer) {
                            std::lock_guard<std::mutex> lock(peer_mu_);
                            peer_address_ = std::move(peer);
                        });
                    session->start();
                }
                if (acceptor_.is_open()) begin_accept();
            });
    }

    asio::io_context io_;
    tcp::acceptor acceptor_;
    std::thread thread_;
    mutable std::mutex peer_mu_;
    std::string peer_address_;
};

class ProxyTempDir {
public:
    ProxyTempDir() {
        path_ = fs::temp_directory_path() /
            ("acecode-remote-proxy-test-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path_);
    }

    ~ProxyTempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    std::string path() const { return path_.string(); }

private:
    fs::path path_;
};

void write_all(tcp::socket& socket, const std::string& value) {
    asio::write(socket, asio::buffer(value));
}

std::string read_exact(tcp::socket& socket, std::size_t bytes) {
    std::string result(bytes, '\0');
    asio::read(socket, asio::buffer(result));
    return result;
}

void assert_round_trip(int proxy_port, EchoServer& echo) {
    asio::io_context io;
    tcp::socket client(io);
    client.connect(tcp::endpoint(
        asio::ip::make_address_v4("127.0.0.1"),
        static_cast<unsigned short>(proxy_port)));

    const std::string http_like =
        "GET /ws HTTP/1.1\r\n"
        "Host: acecode.test\r\n"
        "Connection: Upgrade\r\n"
        "Upgrade: websocket\r\n\r\n";
    write_all(client, http_like);
    EXPECT_EQ(read_exact(client, http_like.size()), http_like);

    const std::string websocket_like("\x82\x06stream", 8);
    write_all(client, websocket_like);
    EXPECT_EQ(read_exact(client, websocket_like.size()), websocket_like);

    for (int attempt = 0; attempt < 40 && echo.peer_address().empty(); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(echo.peer_address(), "127.0.0.2");
}

} // namespace

TEST(RemoteWebTcpProxy, TransparentlyForwardsPersistentBidirectionalBytes) {
    EchoServer echo;
    acecode::web::RemoteWebTcpProxy proxy;
    std::string error;
    ASSERT_TRUE(proxy.start(0, echo.port(), &error)) << error;
    ASSERT_GT(proxy.port(), 0);
    std::thread proxy_thread([&] { proxy.run(); });

    assert_round_trip(proxy.port(), echo);

    proxy.stop();
    if (proxy_thread.joinable()) proxy_thread.join();
}

TEST(RemoteWebProxyCommand, RejectsIncompleteArguments) {
    std::ostringstream out;
    std::ostringstream error;
    EXPECT_EQ(
        acecode::web::run_remote_web_proxy_command(
            {"--listen-port=0"}, out, error),
        64);
    EXPECT_NE(error.str().find("requires"), std::string::npos);
}

TEST(ManagedRemoteWebProxy, StartsRealChildAndStopsWithoutOrphaningIt) {
    EchoServer echo;
    ProxyTempDir runtime;
    acecode::web::ManagedRemoteWebProxyController controller(
        ACECODE_REMOTE_WEB_PROXY_CHILD_PATH,
        runtime.path(),
        acecode::daemon::current_pid());

    const auto started = controller.start(0, echo.port());
    ASSERT_TRUE(started.running) << started.error;
    ASSERT_GT(started.pid, 0);
    ASSERT_GT(started.port, 0);
    EXPECT_TRUE(acecode::daemon::is_pid_alive(started.pid));

    assert_round_trip(started.port, echo);

    const auto stopped = controller.stop();
    EXPECT_FALSE(stopped.running);
    EXPECT_EQ(stopped.pid, 0);
}

TEST(ManagedRemoteWebProxy, FixedPortConflictFailsWithoutFallback) {
    EchoServer echo;
    ProxyTempDir runtime;
    asio::io_context io;
    tcp::acceptor occupied(io, tcp::endpoint(tcp::v4(), 0));
    const int occupied_port = occupied.local_endpoint().port();

    acecode::web::ManagedRemoteWebProxyController controller(
        ACECODE_REMOTE_WEB_PROXY_CHILD_PATH,
        runtime.path(),
        acecode::daemon::current_pid());
    const auto status = controller.start(occupied_port, echo.port());
    EXPECT_FALSE(status.running);
    EXPECT_EQ(status.port, 0);
    EXPECT_FALSE(status.error.empty());
}

TEST(ManagedRemoteWebProxy, AutomaticPortFallsBackWhenAdjacentPortIsBusy) {
    EchoServer echo;
    if (echo.port() >= 65535) GTEST_SKIP() << "no adjacent port available";
    ProxyTempDir runtime;
    asio::io_context io;
    tcp::acceptor occupied(io);
    asio::error_code bind_error;
    occupied.open(tcp::v4(), bind_error);
    if (!bind_error) {
        occupied.bind(
            tcp::endpoint(
                tcp::v4(),
                static_cast<unsigned short>(echo.port() + 1)),
            bind_error);
    }
    if (bind_error) GTEST_SKIP() << bind_error.message();
    occupied.listen(asio::socket_base::max_listen_connections, bind_error);
    if (bind_error) GTEST_SKIP() << bind_error.message();

    acecode::web::ManagedRemoteWebProxyController controller(
        ACECODE_REMOTE_WEB_PROXY_CHILD_PATH,
        runtime.path(),
        acecode::daemon::current_pid());
    const auto status = controller.start(0, echo.port());
    ASSERT_TRUE(status.running) << status.error;
    EXPECT_GT(status.port, 0);
    EXPECT_NE(status.port, echo.port());
    EXPECT_NE(status.port, echo.port() + 1);
    assert_round_trip(status.port, echo);
}

TEST(RemoteWebProxyCommand, ExitsAfterOwningProcessDisappears) {
    EchoServer echo;
    ProxyTempDir runtime;
    const fs::path state_path =
        fs::path(runtime.path()) / "parent-death-state.json";
    const auto launched = acecode::daemon::spawn_detached({
        ACECODE_REMOTE_WEB_PROXY_CHILD_PATH,
        "--parent-death-probe",
        "--target-port=" + std::to_string(echo.port()),
        "--runtime-dir=" + runtime.path(),
        "--state-file=" + state_path.string(),
    });
    ASSERT_NE(launched, 0);

    nlohmann::json state;
    const auto state_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(6);
    while (std::chrono::steady_clock::now() < state_deadline) {
        std::ifstream input(state_path, std::ios::binary);
        if (input.is_open()) {
            state = nlohmann::json::parse(input, nullptr, false);
            if (state.is_object()) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    ASSERT_TRUE(state.is_object());
    ASSERT_TRUE(state.value("running", false))
        << state.value("error", std::string{});
    const auto proxy_pid = state.value("pid", std::int64_t{0});
    ASSERT_GT(proxy_pid, 0);

    const auto exit_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < exit_deadline &&
           acecode::daemon::is_pid_alive(proxy_pid)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    EXPECT_FALSE(acecode::daemon::is_pid_alive(proxy_pid));
}
