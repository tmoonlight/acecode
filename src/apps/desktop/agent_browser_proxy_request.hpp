#pragma once

#include "agent_browser_capture.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace acecode::desktop {

inline std::string browser_proxy_request_error(const nlohmann::json& request,
                                               const std::string& auth_token,
                                               const char* method_error) {
    if (!request.is_object()) return "proxy request must be an object";
    if (!request.contains("auth_token") || !request["auth_token"].is_string() ||
        request["auth_token"].get_ref<const std::string&>() != auth_token) {
        return "Agent Browser proxy authentication failed";
    }
    const std::string operation = request.value("operation", "cdp");
    if (operation != "cdp" && operation != "create_page" && operation != "claim_page" &&
        operation != "close_page" && operation != "select_page" && operation != "drain_store_tasks") {
        return "Agent Browser proxy operation is invalid";
    }
    if (operation == "cdp" && (!request.contains("method") || !request["method"].is_string() ||
        request["method"].get_ref<const std::string&>().empty() ||
        request["method"].get_ref<const std::string&>().size() > 256)) {
        return method_error;
    }
    return {};
}

inline std::chrono::steady_clock::time_point browser_proxy_deadline(const nlohmann::json& request) {
    const int requested_timeout = request.contains("timeout_ms") && request["timeout_ms"].is_number_integer()
        ? request["timeout_ms"].get<int>() : 15000;
    const int timeout_ms = (std::max)(100, (std::min)(120000, requested_timeout));
    return std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
}

class PendingProxyCall {
public:
    explicit PendingProxyCall(std::function<void(nlohmann::json)> completion = {})
        : completion_(std::move(completion)) {}

    void finish(nlohmann::json response) {
        std::function<void(nlohmann::json)> completion;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (completed_) return;
            completed_ = true;
            completion = std::move(completion_);
            if (!completion) response_ = std::move(response);
        }
        ready_.notify_all();
        if (completion) completion(std::move(response));
    }

    bool completed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return completed_;
    }

    nlohmann::json wait(const nlohmann::json& request, const std::atomic<bool>& stopping,
                        std::chrono::steady_clock::time_point deadline,
                        const char* timeout_error) {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!completed_ && !stopping.load() && std::chrono::steady_clock::now() < deadline) {
            ready_.wait_for(lock, std::chrono::milliseconds(50));
        }
        if (completed_) return response_;
        bool full_page = false;
        if (request.value("method", "") == "Page.captureScreenshot" &&
            browser_capture::request_mode(request.value("params", nlohmann::json::object()), full_page).empty() &&
            full_page) {
            completed_ = true;
            response_ = {{"ok", false}, {"page_id", request.value("page_id", "")},
                {"error", stopping.load() ? "capture_stopped" : "capture_timeout"},
                {"recoverable", true}};
            return response_;
        }
        return {{"ok", false}, {"page_id", request.value("page_id", "")},
            {"error", stopping.load() ? "Agent Browser stopped" : timeout_error}};
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    bool completed_ = false;
    nlohmann::json response_;
    std::function<void(nlohmann::json)> completion_;
};

inline void finish_proxy_call(const std::shared_ptr<PendingProxyCall>& pending,
                               nlohmann::json response) {
    pending->finish(std::move(response));
}

}
