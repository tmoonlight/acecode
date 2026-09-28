#pragma once

#include "config/config.hpp"
#include <functional>
#include <optional>

namespace acecode {

// Missing configuration keeps the legacy unconfigured-service behavior.
// A provider returns one value, never pointers into a mutable AppConfig.
struct SessionPromptConfig {
    std::optional<MemoryConfig> memory;
    std::optional<ProjectInstructionsConfig> project_instructions;
    std::optional<CustomInstructionsConfig> custom_instructions;
    std::optional<GitContextConfig> git_context;
};
using PromptConfigProvider = std::function<SessionPromptConfig()>;

} // namespace acecode
