#pragma once

#include "../config/saved_models.hpp"

#include <optional>
#include <string>

namespace acecode {

// Provider-neutral request settings copied from one saved model profile.
// Missing values deliberately preserve each provider's legacy behavior.
enum class ReasoningWireProtocol {
    None,
    OpenRouter,
    Anthropic,
};

struct ProviderRequestOptions {
    std::string endpoint_mode = "base_url";
    std::optional<int> max_output_tokens;
    // nullopt means legacy behavior: forward the supplied tool definitions.
    std::optional<bool> tools_enabled;
    std::optional<ModelReasoningOptions> reasoning;
    ReasoningWireProtocol reasoning_protocol = ReasoningWireProtocol::None;
};

} // namespace acecode
