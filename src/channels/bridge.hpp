#pragma once

#include "state.hpp"
#include "platform/process/piped_process.hpp"
#include <chrono>
#include <functional>
#include <memory>

namespace acecode::channels {

// JSON lines are bounded independently of OS pipe buffering. Events are queued
// for the runtime thread, never dispatched on the response reader.
class Bridge {
public:
    Bridge();
    ~Bridge();
    void start(const lsp::LspSpawnOptions& options);
    void stop();
    bool running() const;
    Json request(const std::string& method, const Json& params,
                 std::chrono::milliseconds timeout = std::chrono::seconds(30));
    std::vector<Json> take_events();
    std::string error() const;
    static constexpr std::size_t kMaxFrameBytes = 256 * 1024;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

std::filesystem::path find_whatsapp_bridge();
std::filesystem::path prepare_whatsapp_bridge(const std::filesystem::path& state_directory);
bool whatsapp_dependencies_ready(const std::filesystem::path& bridge_directory);
lsp::LspSpawnOptions whatsapp_bridge_options(const std::filesystem::path& state_directory);
} // namespace acecode::channels
