#include "availability.hpp"
#include "runtime.hpp"
#include "helper_process_posix.hpp"

#include <chrono>
#include <thread>
#ifdef __APPLE__
#include <cerrno>
#include <unistd.h>
#endif

namespace acecode::computer_use {
namespace {
using json = nlohmann::json;
#ifdef __APPLE__
json probe(const std::string& permission) {
    json result{{"supported", supported()}, {"minimum_macos", "14.0"}, {"helper_available", false},
        {"accessibility", "unknown"}, {"screen_recording", "unknown"}, {"ready", false}};
    if (!supported()) { result["error"] = "COMPUTER_USE_PLATFORM_UNSUPPORTED"; return result; }
    const auto path = computer_use_helper_path();
    result["helper_path"] = path;
    if (path.empty() || access(path.c_str(), X_OK) != 0) { result["error"] = "COMPUTER_USE_HELPER_MISSING"; return result; }
    PosixHelperProcess worker;
    const auto arguments = permission.empty() ? std::vector<std::string>{"--permissions"} :
        std::vector<std::string>{"--request-permission", permission};
    if (!worker.start(path, arguments)) { result["error"] = "COMPUTER_USE_START_ERROR"; return result; }
    result["helper_available"] = true;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(permission.empty() ? 4 : 20);
    std::string response;
    while (std::chrono::steady_clock::now() < deadline) {
        char buffer[4096];
        const auto count = worker.read(buffer, sizeof(buffer));
        if (count > 0) {
            response.append(buffer, static_cast<std::size_t>(count));
            if (response.size() > 16384) { result["error"] = "COMPUTER_USE_PROTOCOL_ERROR"; return result; }
            const auto end = response.find('\n');
            if (end != std::string::npos) {
                const auto message = json::parse(response.substr(0, end), nullptr, false);
                if (!message.is_object() || !message.contains("protocol_version") || message["protocol_version"] != 1 ||
                    !message.contains("success") || !message["success"].is_boolean() ||
                    !message.contains("output") || !message["output"].is_object() || end + 1 != response.size()) {
                    result["error"] = "COMPUTER_USE_PROTOCOL_ERROR"; return result;
                }
                if (!message["success"].get<bool>()) {
                    const auto error = message["output"].find("error");
                    result["error"] = error != message["output"].end() && error->is_string() ? *error : json("COMPUTER_USE_PERMISSION_ERROR");
                    return result;
                }
                const auto& output = message["output"];
                for (const char* key : {"accessibility", "screen_recording"}) {
                    if (!output.contains(key) || !output[key].is_string() ||
                        (output[key] != "granted" && output[key] != "required")) {
                        result["error"] = "COMPUTER_USE_PROTOCOL_ERROR"; return result;
                    }
                    result[key] = output[key];
                }
                result["ready"] = result["accessibility"] == "granted" && result["screen_recording"] == "granted";
                return result;
            }
        } else if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            result["helper_available"] = false; result["error"] = "COMPUTER_USE_DISCONNECTED"; return result;
        } else std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    result["error"] = "COMPUTER_USE_PERMISSION_TIMEOUT";
    return result;
}
#endif
}
json availability() {
#ifdef __APPLE__
    return probe({});
#else
    return {{"supported", supported()}, {"ready", supported()},
        {"accessibility", "not_required"}, {"screen_recording", "not_required"}};
#endif
}
json request_permission(const std::string& permission) {
    if (permission != "accessibility" && permission != "screen_recording")
        return {{"ready", false}, {"error", "COMPUTER_USE_INVALID_PERMISSION"}};
#ifdef __APPLE__
    return probe(permission);
#else
    return {{"ready", false}, {"error", "COMPUTER_USE_PERMISSION_UNSUPPORTED"}};
#endif
}
}
