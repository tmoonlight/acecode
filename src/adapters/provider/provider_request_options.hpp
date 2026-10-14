#pragma once

#include "config/saved_models.hpp"

#include <optional>
#include <string>

namespace acecode {

// Provider-neutral request settings copied from one saved model profile.
// Missing values deliberately preserve each provider's legacy behavior.
enum class ReasoningWireProtocol {
    None,
    OpenRouter,
    Anthropic,
    OpenAi,
};

struct ProviderRequestOptions {
    std::string endpoint_mode = "base_url";
    std::string api_protocol = "chat_completions";
    std::optional<int> max_output_tokens;
    std::optional<ModelReasoningOptions> reasoning;
    ReasoningWireProtocol reasoning_protocol = ReasoningWireProtocol::None;
    // 空 = 一个采样字段都不发,请求体与旧版本逐字节一致。
    ModelSamplingOptions sampling;
};

} // namespace acecode
