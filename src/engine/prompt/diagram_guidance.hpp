#pragma once

// 系统提示里的「画图说明」(openspec add-inline-visuals,docs/inline-visuals.md)。
//
// 模型长期不画 mermaid 的根因是从没被告知界面能渲染它;visual 代码块同理。这里按
// 运行面生成一段固定文本,由 ApiRequestBuilder::static_system_prompt 追加在静态系统
// 提示末尾:
//   - mermaid 部分:TUI 与 daemon(Web / Desktop)都有,只列两端都能渲染的五种类型,
//     并与 web/src/lib/mermaidRenderer.js 的 inspectMermaidSource 拒绝规则对齐;
//   - visual 部分:只有 daemon 且「可视化图表」开关打开时才有。
// 文本只由入参决定(不含时间、路径、会话信息),同一组入参逐字节相同,不打穿 prompt
// cache 前缀。

#include <string>

namespace acecode {

// 会话的显示面。宿主写进 SessionPromptConfig:daemon worker = Web,TUI = Terminal,
// headless 不设(None)。子会话与群聊代答由 RequestContextFactory 降为 None。
enum class DiagramSurface {
    None,
    Terminal,
    Web,
};

// 画图规范技能的名字(assets/seed/skills/acecode/inline-visuals/SKILL.md)。
inline constexpr const char* kInlineVisualsSkillName = "inline-visuals";

struct DiagramGuidanceOptions {
    DiagramSurface surface = DiagramSurface::None;
    // config.inline_visuals.enabled;只在 Web 面有意义。
    bool inline_visuals_enabled = false;
    // 本会话技能快照里有 inline-visuals,且工具策略允许 skill_view。
    bool visual_skill_available = false;
    // skill_view 当前的模型侧名字(model_tool_name_for_native("skill_view"))。
    std::string skill_view_tool_name = "skill_view";
};

// visual 部分是否生效。不生效时技能索引隐藏 inline-visuals。
bool inline_visuals_guidance_active(DiagramSurface surface, bool inline_visuals_enabled);

// 返回完整的画图说明;None 面返回空串。非空时以 "# Diagrams" 开头,不带首尾空行。
std::string build_diagram_guidance(const DiagramGuidanceOptions& options);

} // namespace acecode
