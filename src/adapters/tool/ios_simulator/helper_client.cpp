#include "helper_client.hpp"

#include "utils/logger.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace acecode::ios_simulator {

std::string ios_simulator_helper_path() {
#ifdef __APPLE__
    // Development / test override (e.g. a helper in the build tree).
    if (const char* override_path = std::getenv("ACECODE_IOS_SIM_HELPER"); override_path && *override_path)
        return override_path;
    std::uint32_t length = 0;
    _NSGetExecutablePath(nullptr, &length);
    std::string path(length, '\0');
    if (_NSGetExecutablePath(path.data(), &length) != 0) return {};
    path.resize(std::char_traits<char>::length(path.c_str()));
    std::error_code error;
    auto executable = std::filesystem::canonical(path, error);
    if (error) return {};
    return (executable.parent_path() / "acecode-ios-sim").string();
#else
    return {};
#endif
}

HelperClient::~HelperClient() { stop(); }

bool HelperClient::start(const std::string& helper_path, const std::string& developer_dir, std::string* error) {
    stop();
    platform::SpawnOptions options;
    options.argv = {helper_path, "--developer-dir", developer_dir};
    if (const char* hid = std::getenv("ACECODE_IOS_SIM_HID"); hid && *hid) {
        options.argv.push_back("--hid");
        options.argv.push_back(hid);
    }
    if (!process_.start(options, error)) return false;
    developer_dir_ = developer_dir;
    alive_.store(true);
    reader_ = JoiningThread(&HelperClient::read_loop, this);
    return true;
}

void HelperClient::set_listener(HelperEventListener listener) {
    std::lock_guard<std::mutex> lock(mu_);
    listener_ = std::move(listener);
}

bool HelperClient::write_frame(const nlohmann::json& header) {
    std::string frame;
    try {
        frame = wire::encode(header);
    } catch (const std::exception&) {
        return false;
    }
    std::lock_guard<std::mutex> lock(write_mu_);
    std::string error;
    return process_.write_stdin(frame.data(), frame.size(), &error);
}

HelperResponse HelperClient::request(nlohmann::json request, int timeout_ms, const std::atomic<bool>* abort_flag) {
    HelperResponse failed;
    if (!alive_.load()) {
        failed.error_code = "HELPER_EXITED";
        failed.message = "The iOS Simulator helper is not running.";
        return failed;
    }
    auto pending = std::make_shared<Pending>();
    std::uint64_t id = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        id = next_id_++;
        pending_[id] = pending;
    }
    request["id"] = id;
    if (!write_frame(request)) {
        std::lock_guard<std::mutex> lock(mu_);
        pending_.erase(id);
        failed.error_code = "HELPER_EXITED";
        failed.message = "Could not write to the iOS Simulator helper.";
        return failed;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::unique_lock<std::mutex> lock(mu_);
    while (!pending->done) {
        if (abort_flag && abort_flag->load()) {
            pending_.erase(id);
            posted_.insert(id);  // drop the late reply
            failed.error_code = "CANCELLED";
            failed.message = "Cancelled.";
            return failed;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            pending_.erase(id);
            posted_.insert(id);
            failed.error_code = "HELPER_TIMEOUT";
            failed.message = "The simulator did not answer within " + std::to_string(timeout_ms / 1000) + " seconds.";
            return failed;
        }
        cv_.wait_for(lock, std::chrono::milliseconds(50));
    }
    return pending->response;
}

bool HelperClient::post(nlohmann::json request) {
    if (!alive_.load()) return false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        const auto id = next_id_++;
        posted_.insert(id);
        request["id"] = id;
    }
    return write_frame(request);
}

void HelperClient::fail_all(const std::string& code, const std::string& message) {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& [id, pending] : pending_) {
        pending->done = true;
        pending->response.ok = false;
        pending->response.error_code = code;
        pending->response.message = message;
    }
    pending_.clear();
    cv_.notify_all();
}

void HelperClient::read_loop() {
    wire::Decoder decoder;
    char buffer[65536];
    while (true) {
        const long count = process_.read_stdout(buffer, sizeof(buffer));
        if (count <= 0) break;
        decoder.feed(buffer, static_cast<std::size_t>(count));
        while (auto frame = decoder.next()) {
            auto& header = frame->header;
            if (header.contains("id")) {
                const auto id = header.value("id", std::uint64_t{0});
                std::lock_guard<std::mutex> lock(mu_);
                if (posted_.erase(id)) continue;
                auto found = pending_.find(id);
                if (found == pending_.end()) continue;
                auto& response = found->second->response;
                response.ok = header.value("ok", false);
                if (header.contains("result") && header["result"].is_object()) response.result = header["result"];
                if (header.contains("error") && header["error"].is_object()) {
                    response.error_code = header["error"].value("code", std::string("HELPER_ERROR"));
                    response.message = header["error"].value("message", std::string());
                }
                response.binary = std::move(frame->binary);
                found->second->done = true;
                pending_.erase(found);
                cv_.notify_all();
                continue;
            }
            const auto event = header.value("event", std::string());
            if (event == "log") {
                LOG_INFO("[ios-sim] " + header.value("level", std::string()) + ": " +
                         header.value("message", std::string()));
                continue;
            }
            HelperEventListener listener;
            {
                std::lock_guard<std::mutex> lock(mu_);
                listener = listener_;
            }
            if (listener) listener(header, frame->binary);
        }
        if (!decoder.error().empty()) {
            LOG_WARN("[ios-sim] protocol error: " + decoder.error());
            break;
        }
    }
    alive_.store(false);
    fail_all("HELPER_EXITED", "The iOS Simulator helper exited.");
    HelperEventListener listener;
    {
        std::lock_guard<std::mutex> lock(mu_);
        listener = listener_;
    }
    if (listener) listener({{"event", "helper_exited"}}, {});
}

void HelperClient::stop() {
    if (!process_.started()) return;
    process_.kill_child();
    reader_.join();
    reader_ = JoiningThread();
    process_.terminate();
    alive_.store(false);
}

} // namespace acecode::ios_simulator
