#pragma once

#include "config/config.hpp"
#include "prompt/diagram_guidance.hpp"
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
    // 画图说明的显示面(daemon worker = Web,TUI = Terminal,headless 留 None)与
    // 「可视化图表」开关快照;缺省 = 不加画图说明。见 prompt/diagram_guidance.hpp。
    DiagramSurface diagram_surface = DiagramSurface::None;
    std::optional<InlineVisualsConfig> inline_visuals;
};
using PromptConfigProvider = std::function<SessionPromptConfig()>;

} // namespace acecode
