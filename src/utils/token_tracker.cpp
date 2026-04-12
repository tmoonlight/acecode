#include "token_tracker.hpp"
#include <sstream>
#include <iomanip>
#include <cmath>

namespace acecode {

void TokenTracker::record(const TokenUsage& usage) {
    std::lock_guard<std::mutex> lk(mu_);
    prompt_tokens_ += usage.prompt_tokens;
    completion_tokens_ += usage.completion_tokens;
    total_tokens_ += usage.total_tokens;
}

void TokenTracker::record_estimate(int char_count) {
    std::lock_guard<std::mutex> lk(mu_);
    int estimated = char_count / 4;
    total_tokens_ += estimated;
}

int TokenTracker::prompt_tokens() const {
    std::lock_guard<std::mutex> lk(mu_);
    return prompt_tokens_;
}

int TokenTracker::completion_tokens() const {
    std::lock_guard<std::mutex> lk(mu_);
    return completion_tokens_;
}

int TokenTracker::total_tokens() const {
    std::lock_guard<std::mutex> lk(mu_);
    return total_tokens_;
}

void TokenTracker::reset() {
    std::lock_guard<std::mutex> lk(mu_);
    prompt_tokens_ = 0;
    completion_tokens_ = 0;
    total_tokens_ = 0;
}

std::string TokenTracker::format_tokens(int count) {
    if (count < 1000) {
        return std::to_string(count);
    }
    std::ostringstream oss;
    if (count < 100000) {
        oss << std::fixed << std::setprecision(1) << (count / 1000.0) << "k";
    } else {
        oss << (count / 1000) << "k";
    }
    return oss.str();
}

std::string TokenTracker::format_status(int context_window) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::string tokens_str = format_tokens(total_tokens_) + "/" + format_tokens(context_window);
    double cost = (prompt_tokens_ * kInputPricePerMillion + completion_tokens_ * kOutputPricePerMillion) / 1000000.0;
    std::ostringstream oss;
    oss << tokens_str << "  ~$" << std::fixed << std::setprecision(2) << cost;
    return oss.str();
}

double TokenTracker::estimated_cost() const {
    std::lock_guard<std::mutex> lk(mu_);
    return (prompt_tokens_ * kInputPricePerMillion + completion_tokens_ * kOutputPricePerMillion) / 1000000.0;
}

} // namespace acecode
