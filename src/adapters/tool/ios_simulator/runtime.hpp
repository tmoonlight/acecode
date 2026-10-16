#pragma once

// Process-wide iOS Simulator runtime shared by the model tools and the web
// panel: the selected Xcode, the acecode-ios-sim helper, session-to-device
// bindings and fan-out of live frames.

#include "tool/ios_simulator/helper_client.hpp"
#include "tool/ios_simulator/simctl.hpp"
#include "tool/ios_simulator/xcode_locator.hpp"
#include "utils/lifetime_token.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace acecode::ios_simulator {

struct RuntimeConfig {
    bool enabled = true;
    std::string xcode_path;
    int stream_fps = 30;
    double stream_scale = 0.5;
    double stream_quality = 0.7;
};

struct DeviceInfo {
    std::string udid;
    std::string name;
    std::string state;
    std::string runtime;
    std::string device_type;
    std::string product_family;
    double point_width = 0;   // interface orientation
    double point_height = 0;
    double scale = 1;
    int rotation = 0;
    std::string hid_transport;
    nlohmann::json raw;
};

struct StreamFrame {
    std::string udid;
    std::uint64_t seq = 0;
    int width = 0;
    int height = 0;
    int rotation = 0;
    std::shared_ptr<const std::string> jpeg;
    bool ended = false;
    std::string reason;
};

using FrameSink = std::function<void(const StreamFrame& frame)>;

class Runtime {
public:
    static Runtime& instance();
    ~Runtime();

    static bool supported();
    void configure(const RuntimeConfig& config);
    RuntimeConfig config() const;
    bool enabled() const;

    // Cached Xcode resolution; refresh reruns discovery including Spotlight.
    XcodeResolution resolution(bool refresh = false);
    std::optional<XcodeInstall> xcode(std::string* error);
    nlohmann::json status(bool refresh);

    std::optional<Simctl> simctl(std::string* error);
    std::vector<SimctlDevice> devices(std::string* error);

    // Helper round trip; starts (or restarts) the helper on demand.
    HelperResponse call(nlohmann::json request, int timeout_ms, const std::atomic<bool>* abort_flag = nullptr);
    bool post(nlohmann::json request);
    std::optional<DeviceInfo> describe(const std::string& udid, std::string* error);

    void bind_session(const std::string& session_id, const std::string& udid);
    std::string session_device(const std::string& session_id) const;
    void release_session(const std::string& session_id);
    std::vector<std::string> sessions_for(const std::string& udid) const;
    // requested: UDID or device name (case-insensitive). Without one: the
    // session's device, then a booted device no other session uses, then any
    // booted device.
    std::optional<SimctlDevice> choose_device(const std::string& session_id, const std::string& requested,
                                              std::string* error);

    std::uint64_t subscribe(const std::string& udid, FrameSink sink, std::string* error);
    void unsubscribe(std::uint64_t subscription);
    std::optional<StreamFrame> last_frame(const std::string& udid) const;

    void shutdown();

private:
    Runtime() = default;

    struct Stream {
        std::uint64_t helper_stream = 0;
        bool active = false;
        std::map<std::uint64_t, FrameSink> sinks;
        std::optional<StreamFrame> last;
    };

    std::shared_ptr<HelperClient> ensure_helper(std::string* error);
    void on_helper_event(const nlohmann::json& header, const std::string& binary);
    void end_streams(const std::string& reason, const std::string& only_udid);

    mutable std::mutex mu_;
    RuntimeConfig config_;
    std::optional<XcodeResolution> resolution_;
    std::shared_ptr<HelperClient> helper_;
    std::map<std::string, std::string> session_devices_;
    std::map<std::string, Stream> streams_;              // by udid
    std::map<std::uint64_t, std::string> subscriptions_; // subscription -> udid
    std::uint64_t next_subscription_ = 1;
    std::uint64_t next_helper_stream_ = 1;
    std::mutex start_mu_;  // serializes helper start / stream start
    LifetimeToken lifetime_;  // last: revoked before members above are destroyed
};

nlohmann::json to_json(const DeviceInfo& info);

} // namespace acecode::ios_simulator
