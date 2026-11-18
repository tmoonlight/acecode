#pragma once

#include "agent_browser_proxy_request.hpp"

#include <vector>

namespace acecode::desktop {

class AnnotationRequests {
public:
    using Clock = std::chrono::steady_clock;
    AnnotationRequests() = default;
    AnnotationRequests(const AnnotationRequests&) = delete;
    AnnotationRequests& operator=(const AnnotationRequests&) = delete;
    ~AnnotationRequests() { stop(); }

    std::shared_ptr<PendingProxyCall> add(std::function<void(nlohmann::json)> completion,
                                        Clock::time_point now = Clock::now()) {
        expire(now);
        if (stopped_ || entries_.size() >= 32) {
            completion(failure(stopped_ ? "Annotation page closed" : "Too many annotation requests"));
            return {};
        }
        auto pending = std::make_shared<PendingProxyCall>(std::move(completion));
        entries_.push_back({pending, now + std::chrono::seconds(15)});
        return pending;
    }

    void expire(Clock::time_point now = Clock::now()) {
        std::vector<std::shared_ptr<PendingProxyCall>> expired;
        entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [&](const auto& entry) {
            if (entry.pending->completed()) return true;
            if (entry.deadline > now) return false;
            expired.push_back(entry.pending);
            return true;
        }), entries_.end());
        for (const auto& pending : expired) pending->finish(failure("Annotation request timed out"));
    }

    void cancel(const std::string& reason) {
        auto entries = std::move(entries_);
        entries_.clear();
        for (const auto& entry : entries) entry.pending->finish(failure(reason));
    }

    void stop() { stopped_ = true; cancel("Annotation page closed"); }
    void restart() { stopped_ = false; }

private:
    struct Entry {
        std::shared_ptr<PendingProxyCall> pending;
        Clock::time_point deadline;
    };
    static nlohmann::json failure(const std::string& reason) {
        return {{"ok", false}, {"error", reason}, {"recoverable", true}};
    }
    std::vector<Entry> entries_;
    bool stopped_ = false;
};

}
