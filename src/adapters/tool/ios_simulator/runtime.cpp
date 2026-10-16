#include "runtime.hpp"

#include "tool/ios_simulator/device_consent.hpp"
#include "tool/ios_simulator/helper_sandbox.hpp"
#include "utils/logger.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace acecode::ios_simulator {
namespace {

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool helper_installed(const std::string& path) {
    std::error_code error;
    return !path.empty() && std::filesystem::exists(path, error);
}

} // namespace

Runtime& Runtime::instance() {
    static Runtime runtime;
    return runtime;
}

Runtime::~Runtime() {
    lifetime_.revoke();
    shutdown();
}

bool Runtime::supported() {
#ifdef __APPLE__
    return true;
#else
    return false;
#endif
}

void Runtime::configure(const RuntimeConfig& config) {
    std::shared_ptr<HelperClient> retired;
    {
        std::lock_guard<std::mutex> lock(mu_);
        const bool xcode_changed = config.xcode_path != config_.xcode_path;
        const bool disabled = !config.enabled;
        config_ = config;
        config_.stream_fps = std::clamp(config_.stream_fps, 5, 60);
        config_.stream_scale = std::clamp(config_.stream_scale, 0.25, 1.0);
        config_.stream_quality = std::clamp(config_.stream_quality, 0.3, 0.95);
        if (xcode_changed) resolution_.reset();
        if ((xcode_changed || disabled) && helper_) retired = std::move(helper_);
    }
    if (retired) {
        retired->stop();
        end_streams("xcode_changed", {});
    }
}

RuntimeConfig Runtime::config() const {
    std::lock_guard<std::mutex> lock(mu_);
    return config_;
}

bool Runtime::enabled() const {
    std::lock_guard<std::mutex> lock(mu_);
    return supported() && config_.enabled;
}

XcodeResolution Runtime::resolution(bool refresh) {
    std::string configured;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (resolution_ && !refresh) return *resolution_;
        configured = config_.xcode_path;
    }
    DiscoveryOptions options;
    options.spotlight = true;
    auto resolved = resolve_xcode(configured, options);
    std::shared_ptr<HelperClient> retired;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (config_.xcode_path != configured) return resolved;  // configuration changed meanwhile
        const bool moved = helper_ && resolved.selected && helper_->developer_dir() != resolved.selected->developer_dir;
        if (moved) retired = std::move(helper_);
        resolution_ = resolved;
    }
    if (retired) {
        retired->stop();
        end_streams("xcode_changed", {});
    }
    return resolved;
}

std::optional<XcodeInstall> Runtime::xcode(std::string* error) {
    auto resolved = resolution(false);
    if (resolved.selected) return resolved.selected;
    if (error) {
        *error = resolved.error;
        if (!resolved.configured_error.empty())
            *error = "The configured Xcode is not usable (" + resolved.configured_error + "). " + resolved.error;
    }
    return std::nullopt;
}

nlohmann::json Runtime::status(bool refresh) {
    const auto resolved = resolution(refresh);
    const auto helper_path = ios_simulator_helper_path();
    nlohmann::json out;
    out["supported"] = supported();
    {
        std::lock_guard<std::mutex> lock(mu_);
        out["enabled"] = config_.enabled;
        out["config"] = {{"xcode_path", config_.xcode_path}, {"stream_fps", config_.stream_fps},
                         {"stream_scale", config_.stream_scale}, {"stream_quality", config_.stream_quality}};
        out["helper"] = {{"installed", helper_installed(helper_path)},
                         {"running", helper_ && helper_->alive()},
                         {"developer_dir", helper_ ? helper_->developer_dir() : std::string()},
                         {"sandboxed", helper_ && helper_->sandboxed()},
                         {"sandbox_note", sandbox_note_}};
        nlohmann::json bindings = nlohmann::json::object();
        for (const auto& [session, udid] : session_devices_) bindings[session] = udid;
        out["sessions"] = bindings;
    }
    out["xcode"] = to_json(resolved);
    out["device_consent"] = list_device_consent();
    return out;
}

std::optional<Simctl> Runtime::simctl(std::string* error) {
    auto install = xcode(error);
    if (!install) return std::nullopt;
    return Simctl(install->developer_dir);
}

std::vector<SimctlDevice> Runtime::devices(std::string* error) {
    auto tool = simctl(error);
    if (!tool) return {};
    return tool->list(error);
}

std::shared_ptr<HelperClient> Runtime::ensure_helper(std::string* error) {
    std::lock_guard<std::mutex> start(start_mu_);
    auto install = xcode(error);
    if (!install) return nullptr;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (helper_ && helper_->alive() && helper_->developer_dir() == install->developer_dir) return helper_;
    }
    const auto path = ios_simulator_helper_path();
    if (!helper_installed(path)) {
        if (error) *error = "acecode-ios-sim is missing beside the ACECode executable; reinstall ACECode for macOS.";
        return nullptr;
    }
    std::string sandbox_note;
    const bool sandboxed = helper_sandbox_enabled(&sandbox_note);
    auto helper = launch_helper(path, install->developer_dir, sandboxed, error);
    if (helper && sandboxed) {
        // sandbox-exec exits at once when the profile cannot be applied, e.g.
        // when ACECode itself already runs inside a sandbox. The helper still
        // works unconfined; the status reports it and the log says why.
        const auto hello = helper->request({{"op", "hello"}}, 15000);
        if (!hello.ok && hello.error_code == "HELPER_EXITED") {
            LOG_WARN("[ios-sim] sandbox-exec refused the helper profile; running the helper unsandboxed");
            helper->stop();
            sandbox_note = "sandbox-exec could not apply the helper profile";
            helper = launch_helper(path, install->developer_dir, false, error);
        }
    }
    if (!helper) return nullptr;
    std::shared_ptr<HelperClient> retired;
    {
        std::lock_guard<std::mutex> lock(mu_);
        retired = std::move(helper_);
        helper_ = helper;
        sandbox_note_ = helper->sandboxed() ? std::string() : sandbox_note;
    }
    if (retired) retired->stop();
    LOG_INFO("[ios-sim] helper started for " + install->developer_dir +
             (helper->sandboxed() ? " (sandboxed)" : " (unsandboxed: " + sandbox_note + ")"));
    return helper;
}

std::shared_ptr<HelperClient> Runtime::launch_helper(const std::string& path, const std::string& developer_dir,
                                                     bool sandboxed, std::string* error) {
    auto helper = std::make_shared<HelperClient>();
    helper->set_listener([ref = lifetime_.ref(*this)](const nlohmann::json& header, const std::string& binary) {
        ref.with([&header, &binary](Runtime& self) { self.on_helper_event(header, binary); });
    });
    std::string start_error;
    if (!helper->start(path, developer_dir, sandboxed, &start_error)) {
        if (error) *error = "Could not start the iOS Simulator helper: " + start_error;
        return nullptr;
    }
    return helper;
}

HelperResponse Runtime::call(nlohmann::json request, int timeout_ms, const std::atomic<bool>* abort_flag) {
    std::string error;
    auto helper = ensure_helper(&error);
    if (!helper) {
        HelperResponse failed;
        failed.error_code = "XCODE_UNAVAILABLE";
        failed.message = error;
        return failed;
    }
    return helper->request(std::move(request), timeout_ms, abort_flag);
}

bool Runtime::post(nlohmann::json request) {
    std::string error;
    auto helper = ensure_helper(&error);
    return helper && helper->post(std::move(request));
}

std::optional<DeviceInfo> Runtime::describe(const std::string& udid, std::string* error) {
    auto response = call({{"op", "describe"}, {"udid", udid}}, 20000);
    if (!response.ok) {
        if (error) *error = response.message;
        return std::nullopt;
    }
    const auto& raw = response.result;
    DeviceInfo info;
    info.udid = raw.value("udid", udid);
    info.name = raw.value("name", std::string());
    info.state = raw.value("state", std::string());
    info.runtime = raw.value("runtime", std::string());
    info.device_type = raw.value("device_type", std::string());
    info.product_family = raw.value("product_family", std::string());
    info.point_width = raw.value("point_width", 0.0);
    info.point_height = raw.value("point_height", 0.0);
    info.scale = raw.value("scale", 1.0);
    info.rotation = raw.value("rotation", 0);
    info.hid_transport = raw.value("default_hid_transport", std::string());
    info.raw = raw;
    return info;
}

void Runtime::bind_session(const std::string& session_id, const std::string& udid) {
    if (session_id.empty() || udid.empty()) return;
    std::lock_guard<std::mutex> lock(mu_);
    session_devices_[session_id] = udid;
}

std::string Runtime::session_device(const std::string& session_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto found = session_devices_.find(session_id);
    return found == session_devices_.end() ? std::string() : found->second;
}

void Runtime::release_session(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mu_);
    session_devices_.erase(session_id);
}

std::vector<std::string> Runtime::sessions_for(const std::string& udid) const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<std::string> sessions;
    for (const auto& [session, device] : session_devices_) {
        if (device == udid) sessions.push_back(session);
    }
    return sessions;
}

std::optional<SimctlDevice> Runtime::choose_device(const std::string& session_id, const std::string& requested,
                                                   std::string* error) {
    // Fast path (no simctl round trip): the session's booted device, or an
    // explicit UDID, answered by the helper directly.
    const bool udid_like = requested.size() == 36 && std::count(requested.begin(), requested.end(), '-') == 4;
    const auto direct = udid_like ? requested : (requested.empty() ? session_device(session_id) : std::string());
    if (!direct.empty()) {
        std::string ignored;
        if (auto info = describe(direct, &ignored); info && info->state == "booted") {
            SimctlDevice device;
            device.udid = info->udid;
            device.name = info->name;
            device.state = "Booted";
            device.runtime_name = info->runtime;
            device.runtime_identifier = info->raw.value("runtime_identifier", std::string());
            device.runtime_version = info->raw.value("runtime_version", std::string());
            device.device_type_identifier = info->device_type;
            return device;
        }
    }
    std::string list_error;
    auto all = devices(&list_error);
    if (all.empty()) {
        if (error) *error = list_error.empty() ? "No simulators are available. Create one in Xcode." : list_error;
        return std::nullopt;
    }
    if (!requested.empty()) {
        const auto wanted = lower(requested);
        std::vector<SimctlDevice> partial;
        for (const auto& device : all) {
            if (lower(device.udid) == wanted || lower(device.name) == wanted) return device;
            if (lower(device.name).find(wanted) != std::string::npos) partial.push_back(device);
        }
        // Prefer booted partial matches, then the newest runtime (list order).
        std::stable_partition(partial.begin(), partial.end(),
                              [](const SimctlDevice& device) { return is_booted_state(device.state); });
        if (!partial.empty()) return partial.front();
        if (error) *error = "No simulator matches \"" + requested + "\". Use action list_devices.";
        return std::nullopt;
    }
    const auto bound = session_device(session_id);
    if (!bound.empty()) {
        for (const auto& device : all) {
            if (device.udid == bound && is_booted_state(device.state)) return device;
        }
    }
    std::optional<SimctlDevice> any_booted;
    for (const auto& device : all) {
        if (!is_booted_state(device.state) || !device.available) continue;
        if (!any_booted) any_booted = device;
        const auto users = sessions_for(device.udid);
        const bool others = std::any_of(users.begin(), users.end(),
                                        [&session_id](const std::string& user) { return user != session_id; });
        if (!others) return device;
    }
    if (any_booted) return any_booted;
    if (error) *error = "No simulator is booted. Boot one with action boot (device name or UDID), "
                        "or run the app from Xcode first.";
    return std::nullopt;
}

std::uint64_t Runtime::subscribe(const std::string& udid, FrameSink sink, std::string* error) {
    std::uint64_t id = 0;
    std::optional<StreamFrame> replay;
    bool need_start = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        id = next_subscription_++;
        auto& stream = streams_[udid];
        stream.sinks[id] = sink;
        subscriptions_[id] = udid;
        need_start = !stream.active;
        if (stream.active) replay = stream.last;
    }
    if (replay && !replay->ended) sink(*replay);
    if (!need_start) return id;
    std::string helper_error;
    auto helper = ensure_helper(&helper_error);  // takes start_mu_ itself
    std::lock_guard<std::mutex> start(start_mu_);
    RuntimeConfig settings;
    std::uint64_t helper_stream = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto& stream = streams_[udid];
        if (stream.active) return id;  // another subscriber started it meanwhile
        settings = config_;
        helper_stream = next_helper_stream_++;
        stream.helper_stream = helper_stream;
    }
    HelperResponse response;
    if (helper) {
        response = helper->request({{"op", "start_stream"}, {"udid", udid}, {"stream", helper_stream},
                                    {"fps", settings.stream_fps}, {"scale", settings.stream_scale},
                                    {"quality", settings.stream_quality}}, 20000);
    } else {
        response.message = helper_error;
    }
    std::lock_guard<std::mutex> lock(mu_);
    auto& stream = streams_[udid];
    if (!response.ok) {
        stream.sinks.erase(id);
        subscriptions_.erase(id);
        if (error) *error = response.message.empty() ? "Could not start the simulator stream." : response.message;
        return 0;
    }
    if (stream.helper_stream == helper_stream) stream.active = true;
    return id;
}

void Runtime::unsubscribe(std::uint64_t subscription) {
    std::uint64_t stop_stream = 0;
    std::shared_ptr<HelperClient> helper;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto found = subscriptions_.find(subscription);
        if (found == subscriptions_.end()) return;
        auto stream = streams_.find(found->second);
        subscriptions_.erase(found);
        if (stream == streams_.end()) return;
        stream->second.sinks.erase(subscription);
        if (stream->second.sinks.empty()) {
            if (stream->second.active) stop_stream = stream->second.helper_stream;
            streams_.erase(stream);
        }
        helper = helper_;
    }
    if (stop_stream && helper) helper->post({{"op", "stop_stream"}, {"stream", stop_stream}});
}

std::optional<StreamFrame> Runtime::last_frame(const std::string& udid) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto found = streams_.find(udid);
    if (found == streams_.end()) return std::nullopt;
    return found->second.last;
}

void Runtime::end_streams(const std::string& reason, const std::string& only_udid) {
    std::vector<std::pair<FrameSink, StreamFrame>> notify;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& [udid, stream] : streams_) {
            if (!only_udid.empty() && udid != only_udid) continue;
            if (!stream.active) continue;
            stream.active = false;
            StreamFrame ended;
            ended.udid = udid;
            ended.ended = true;
            ended.reason = reason;
            stream.last = ended;
            for (const auto& [id, sink] : stream.sinks) notify.emplace_back(sink, ended);
        }
    }
    for (auto& [sink, frame] : notify) sink(frame);
}

void Runtime::on_helper_event(const nlohmann::json& header, const std::string& binary) {
    const auto event = header.value("event", std::string());
    if (event == "helper_exited") {
        end_streams("helper_exited", {});
        return;
    }
    const auto udid = header.value("udid", std::string());
    const auto stream_id = header.value("stream", std::uint64_t{0});
    if (event == "stream_ended") {
        std::string target;
        {
            std::lock_guard<std::mutex> lock(mu_);
            for (const auto& [device, stream] : streams_) {
                if (stream.helper_stream == stream_id) target = device;
            }
        }
        if (!target.empty()) end_streams(header.value("reason", std::string("ended")), target);
        return;
    }
    if (event != "frame") return;
    StreamFrame frame;
    frame.udid = udid;
    frame.seq = header.value("seq", std::uint64_t{0});
    frame.width = header.value("width", 0);
    frame.height = header.value("height", 0);
    frame.rotation = header.value("rotation", 0);
    frame.jpeg = std::make_shared<const std::string>(binary);
    std::vector<FrameSink> sinks;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto found = streams_.find(udid);
        if (found == streams_.end() || found->second.helper_stream != stream_id) return;
        found->second.last = frame;
        for (const auto& [id, sink] : found->second.sinks) sinks.push_back(sink);
    }
    for (const auto& sink : sinks) sink(frame);
}

void Runtime::shutdown() {
    std::shared_ptr<HelperClient> helper;
    {
        std::lock_guard<std::mutex> lock(mu_);
        helper = std::move(helper_);
    }
    if (helper) helper->stop();
}

nlohmann::json to_json(const DeviceInfo& info) {
    return {{"udid", info.udid}, {"name", info.name}, {"state", info.state}, {"runtime", info.runtime},
            {"device_type", info.device_type}, {"product_family", info.product_family},
            {"point_width", info.point_width}, {"point_height", info.point_height}, {"scale", info.scale},
            {"rotation", info.rotation}, {"hid_transport", info.hid_transport}};
}

} // namespace acecode::ios_simulator
