#pragma once

#include "state.hpp"
#include "session/session_client.hpp"
#include <functional>
#include <memory>

namespace acecode::channels {

struct GatewayDeps {
    SessionClient& sessions;
    // Transport requests are bounded and must throw on timeout or failure.
    std::function<Json(const std::string&, const Json&)> request;
    std::function<std::vector<Json>(const std::string&)> permissions;
    std::function<Json(const std::string&)> transcript;
    std::function<std::string(const std::string&)> session_cwd;
};

class Gateway {
public:
    Gateway(State& state, GatewayDeps deps);
    ~Gateway();
    Gateway(const Gateway&) = delete;
    Gateway& operator=(const Gateway&) = delete;

    Json receive(const Json& message);
    Json control(const Json& command);
    void restore(const std::string& account);
    void shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace acecode::channels
