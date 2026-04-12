#pragma once

#include "../provider/llm_provider.hpp"
#include <string>
#include <mutex>

namespace acecode {

class TokenTracker {
public:
    // Record usage from a server response
    void record(const TokenUsage& usage);

    // Estimate and record tokens from character count (fallback when no server usage)
    void record_estimate(int char_count);

    // Get cumulative session stats
    int prompt_tokens() const;
    int completion_tokens() const;
    int total_tokens() const;

    // Reset all counters
    void reset();

    // Format token count for display (e.g., "1.2k", "45.3k")
    static std::string format_tokens(int count);

    // Format status bar string: "1.2k/128k  ~$0.03"
    std::string format_status(int context_window) const;

    // Get estimated cost in USD
    double estimated_cost() const;

private:
    mutable std::mutex mu_;
    int prompt_tokens_ = 0;
    int completion_tokens_ = 0;
    int total_tokens_ = 0;

    // Default pricing per 1M tokens (reasonable defaults for common models)
    static constexpr double kInputPricePerMillion = 3.0;   // $3/1M input tokens
    static constexpr double kOutputPricePerMillion = 15.0;  // $15/1M output tokens
};

} // namespace acecode
