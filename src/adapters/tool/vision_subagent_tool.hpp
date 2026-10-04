#pragma once

#include "config/config.hpp"
#include "llm/llm_provider.hpp"
#include "tool_executor.hpp"

#include <cstddef>
#include <functional>
#include <memory>

namespace acecode {

struct VisionSubagentToolOptions {
    using ProviderFactory =
        std::function<std::shared_ptr<LlmProvider>(const ModelProfile&)>;
    using IndexChooser = std::function<std::size_t(std::size_t)>;

    // With cancellation enabled, factory and its captures may outlive the
    // invocation. Injected factories must own all captured dependencies.
    ProviderFactory provider_factory;
    IndexChooser choose_index;
};

ToolImpl create_vision_analyze_tool(
    const AppConfig& config,
    VisionSubagentToolOptions options = {});

} // namespace acecode
