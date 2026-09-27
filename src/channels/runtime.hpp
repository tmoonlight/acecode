#pragma once

#include "gateway.hpp"
#include "bridge.hpp"
#include "owner_lock.hpp"
#include <memory>

namespace acecode::channels {

class Runtime {
public:
    using SpawnOptions = std::function<platform::SpawnOptions(const std::filesystem::path&)>;
    explicit Runtime(GatewayDeps deps, std::filesystem::path directory = {}, SpawnOptions spawn = {});
    ~Runtime();
    void start();
    void stop();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

std::filesystem::path channel_directory();
// Never starts a host or retries a mutation after an uncertain HTTP outcome.
// Status can inspect saved configuration when daemon/Desktop is not running.
Json request_control(const Json& command, const std::filesystem::path& directory = {});
} // namespace acecode::channels
