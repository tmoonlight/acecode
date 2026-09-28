#pragma once

#include <atomic>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace acecode::web {

inline constexpr const char* kRemoteWebProxyBind = "0.0.0.0";
inline constexpr const char* kRemoteWebProxyIpv6Bind = "::";
inline constexpr const char* kRemoteWebProxyTarget = "127.0.0.1";
inline constexpr const char* kRemoteWebProxyUpstreamSource = "127.0.0.2";

struct RemoteWebProxyStatus {
    bool configured = false;
    bool running = false;
    bool starting = false;
    bool ipv6 = false;
    int port = 0;
    std::int64_t pid = 0;
    std::string error;
};

// Interface injected into WebServer so route tests can use a deterministic
// fake without launching another executable.
class RemoteWebProxyController {
public:
    virtual ~RemoteWebProxyController() = default;
    virtual RemoteWebProxyStatus status() = 0;
    virtual RemoteWebProxyStatus start(
        int configured_port,
        int target_port) = 0;
    virtual RemoteWebProxyStatus stop() = 0;
};

// The byte-transparent proxy runtime used both by the hidden child command and
// native integration tests. start() only binds/listens; run() blocks.
class RemoteWebTcpProxy {
public:
    RemoteWebTcpProxy();
    ~RemoteWebTcpProxy();

    RemoteWebTcpProxy(const RemoteWebTcpProxy&) = delete;
    RemoteWebTcpProxy& operator=(const RemoteWebTcpProxy&) = delete;

    bool start(int listen_port, int target_port, std::string* error = nullptr);
    void run();
    void stop();

    int port() const;
    bool ipv6_enabled() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Owns the child process launched by a daemon. configured_port=0 chooses a
// deterministic adjacent port first and falls back to an OS-selected port.
class ManagedRemoteWebProxyController final : public RemoteWebProxyController {
public:
    ManagedRemoteWebProxyController(
        std::string executable_path,
        std::string runtime_dir,
        std::int64_t parent_pid);
    ~ManagedRemoteWebProxyController() override;

    ManagedRemoteWebProxyController(
        const ManagedRemoteWebProxyController&) = delete;
    ManagedRemoteWebProxyController& operator=(
        const ManagedRemoteWebProxyController&) = delete;

    RemoteWebProxyStatus status() override;
    RemoteWebProxyStatus start(
        int configured_port,
        int target_port) override;
    RemoteWebProxyStatus stop() override;

private:
    RemoteWebProxyStatus status_locked();
    RemoteWebProxyStatus start_candidate_locked(
        int listen_port,
        int target_port);
    bool stop_locked();

    std::mutex mu_;
    std::string executable_path_;
    std::string runtime_dir_;
    std::int64_t parent_pid_ = 0;
    RemoteWebProxyStatus status_;
};

// Internal top-level command. tokens excludes the executable and
// `--remote-web-proxy` marker.
int run_remote_web_proxy_command(
    const std::vector<std::string>& tokens,
    std::ostream& out,
    std::ostream& err);

} // namespace acecode::web
