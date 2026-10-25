#pragma once

#include "llm/llm_provider.hpp"

#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace acecode {

// Request body caps from grok-build (ApiBackend::default_max_request_bytes):
// the inference proxy limit for Chat Completions / Responses, and the
// Anthropic Messages limit.
inline constexpr std::size_t kOpenAiCompatMaxRequestBytes = 50u * 1024u * 1024u;
inline constexpr std::size_t kAnthropicMaxRequestBytes = 30'000'000u;

struct ImageRequestBudgetLimits {
    std::size_t trigger_bytes = 0;
    std::size_t reclaim_target_bytes = 0;
};

// Trigger 3 MiB below the cap (room for tool definitions and the envelope),
// reclaim to half the cap, so one eviction is followed by many turns that keep
// the prompt cache prefix unchanged.
ImageRequestBudgetLimits image_request_budget_limits(std::size_t max_request_bytes);

struct ImageRequestBudgetOutcome {
    std::size_t body_bytes = 0;
    std::size_t body_bytes_after = 0;
    std::size_t inline_images = 0;
    std::size_t evicted = 0;
};

// Wire size (data URL length) of one `image` content part; 0 = not sent inline.
using ImageWireBytesFn = std::function<std::size_t(const nlohmann::json& image_part)>;

// grok-build's image byte budget (xai-chat-state image_budget.rs) with the
// low-water mark kept across requests: once a request reaches the trigger the
// oldest images become file references until the body is at the reclaim
// target, and the newest removed image is remembered. Later requests remove
// the same images, so the request stays byte-stable until it grows to the
// trigger again. One instance belongs to one provider (one session).
class ImageRequestBudget {
public:
    ImageRequestBudget() = default;
    ImageRequestBudget(const ImageRequestBudget& other);
    ImageRequestBudget& operator=(const ImageRequestBudget& other);

    ImageRequestBudgetOutcome apply(std::vector<ChatMessage>& messages,
                                    const ImageRequestBudgetLimits& limits,
                                    const ImageWireBytesFn& image_wire_bytes);

private:
    mutable std::mutex mu_;
    std::string watermark_;
};

} // namespace acecode
