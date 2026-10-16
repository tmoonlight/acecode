#pragma once

// Daemon-side connection to one acecode-ios-sim process. Requests are framed
// JSON (see wire.hpp); a reader thread routes responses to waiting callers
// and video frames / stream events to the installed listener.

#include "platform/process/piped_process.hpp"
#include "tool/ios_simulator/wire.hpp"
#include "utils/joining_thread.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>

namespace acecode::ios_simulator {

struct HelperResponse {
    bool ok = false;
    nlohmann::json result = nlohmann::json::object();
    std::string binary;
    std::string error_code;
    std::string message;
};

// Unsolicited helper messages ("frame", "stream_ended", "log"). Called on the
// reader thread; keep it short and never call back into request().
using HelperEventListener = std::function<void(const nlohmann::json& header, const std::string& binary)>;

class HelperClient {
public:
    HelperClient() = default;
    ~HelperClient();
    HelperClient(const HelperClient&) = delete;
    HelperClient& operator=(const HelperClient&) = delete;

    // Spawns the helper for the given Xcode developer directory.
    bool start(const std::string& helper_path, const std::string& developer_dir, std::string* error);
    bool alive() const { return alive_.load(); }
    const std::string& developer_dir() const { return developer_dir_; }
    void set_listener(HelperEventListener listener);

    // Sends {op, ...} and waits for the reply. Fails with HELPER_TIMEOUT,
    // HELPER_EXITED or CANCELLED without killing the helper.
    HelperResponse request(nlohmann::json request, int timeout_ms, const std::atomic<bool>* abort_flag = nullptr);
    // Fire-and-forget (the reply is discarded); for latency-sensitive input.
    bool post(nlohmann::json request);

    void stop();

private:
    struct Pending {
        bool done = false;
        HelperResponse response;
    };

    bool write_frame(const nlohmann::json& header);
    void read_loop();
    void fail_all(const std::string& code, const std::string& message);

    std::string developer_dir_;
    platform::PipedProcess process_;
    std::mutex write_mu_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::map<std::uint64_t, std::shared_ptr<Pending>> pending_;
    std::uint64_t next_id_ = 1;
    std::set<std::uint64_t> posted_;  // fire-and-forget ids whose replies are dropped
    HelperEventListener listener_;
    std::atomic<bool> alive_{false};
    // Last member: joined before the state above is destroyed.
    JoiningThread reader_;
};

// Path of acecode-ios-sim beside the running executable.
std::string ios_simulator_helper_path();

} // namespace acecode::ios_simulator
