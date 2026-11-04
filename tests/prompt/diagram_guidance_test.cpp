// 覆盖 src/engine/prompt/diagram_guidance.cpp(openspec add-inline-visuals 的「画图说明」):
//   1. 三种显示面:None 不出任何文字;Terminal 只有 mermaid 部分;Web 视开关加 visual 部分
//   2. mermaid 部分只列两端都能渲染的五种类型,并写明会退回源码的写法
//   3. visual 部分:技能可用时点名用 skill_view 的模型侧名字加载 inline-visuals,
//      技能不可用时只给核心约束
//   4. 同一组输入逐字节相同(放在静态系统提示里,不能打穿 prompt cache)

#include <gtest/gtest.h>

#include "prompt/diagram_guidance.hpp"

#include <string>

using acecode::DiagramGuidanceOptions;
using acecode::DiagramSurface;
using acecode::build_diagram_guidance;
using acecode::inline_visuals_guidance_active;

namespace {

bool contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

DiagramGuidanceOptions web(bool enabled, bool skill) {
    DiagramGuidanceOptions options;
    options.surface = DiagramSurface::Web;
    options.inline_visuals_enabled = enabled;
    options.visual_skill_available = skill;
    options.skill_view_tool_name = "skill_view";
    return options;
}

} // namespace

// 场景:headless(`acecode -p`)、子代理、群聊代答会话的显示面都是 None。
// 期望:不输出任何文字(拼接方据此不加空行),开关打开也一样。
TEST(DiagramGuidance, NoneSurfaceProducesNothing) {
    DiagramGuidanceOptions options;
    options.surface = DiagramSurface::None;
    options.inline_visuals_enabled = true;
    options.visual_skill_available = true;
    EXPECT_TRUE(build_diagram_guidance(options).empty());
    EXPECT_FALSE(inline_visuals_guidance_active(DiagramSurface::None, true));
}

// 场景:TUI 会话(终端只能画文字版 mermaid,不渲染 visual)。
// 期望:有 mermaid 说明,并说明终端里是文字图;没有任何 visual 字样 —— 否则 TUI 里的
// 模型会去写 visual 代码块,用户只看到一行占位提示。开关打开也不影响。
TEST(DiagramGuidance, TerminalGetsMermaidOnly) {
    DiagramGuidanceOptions options;
    options.surface = DiagramSurface::Terminal;
    options.inline_visuals_enabled = true;
    options.visual_skill_available = true;
    const auto text = build_diagram_guidance(options);
    EXPECT_EQ(text.rfind("# Diagrams\n\n", 0), 0u);
    EXPECT_TRUE(contains(text, "```mermaid"));
    EXPECT_TRUE(contains(text, "text-art"));
    EXPECT_FALSE(contains(text, "visual"));
    EXPECT_FALSE(contains(text, "inline-visuals"));
    EXPECT_FALSE(inline_visuals_guidance_active(DiagramSurface::Terminal, true));
}

// 场景:模型不知道 Web 的 mermaid 安全检查会拒绝哪些写法,经常写出 gantt、带 click
// 或 %%{init}%% 的图,结果用户只看到源码。
// 期望:mermaid 部分列出五种类型(flowchart 必须写方向),并写明会退回源码的写法,
// 包括 classDiagram 的尖括号泛型要改用 ~T~。
TEST(DiagramGuidance, MermaidPartListsSupportedTypesAndRejectedSyntax) {
    const auto text = build_diagram_guidance(web(false, false));
    for (const char* type : {"`flowchart TD`", "`flowchart LR`", "`sequenceDiagram`",
                             "`stateDiagram-v2`", "`classDiagram`", "`erDiagram`"}) {
        EXPECT_TRUE(contains(text, type)) << type;
    }
    EXPECT_TRUE(contains(text, "always state the direction"));
    for (const char* rejected : {"gantt", "`%%{init}%%`", "front matter", "`<br>`",
                                 "`List~String~`", "`click`", "`$$`", "`//`"}) {
        EXPECT_TRUE(contains(text, rejected)) << rejected;
    }
    EXPECT_TRUE(contains(text, "Skip the diagram"));
}

// 场景:网页会话,「可视化图表」开关为默认的开启,画图规范技能可用。
// 期望:mermaid 与 visual 两部分都有;visual 部分点名先用 skill_view 加载
// inline-visuals,并写明顶层、片段、主题变量、脚本在闭合后执行、CDN 与 ace.send。
TEST(DiagramGuidance, WebWithSwitchOnAddsVisualPartAndSkillLoad) {
    const auto text = build_diagram_guidance(web(true, true));
    EXPECT_TRUE(contains(text, "# Diagrams"));
    EXPECT_TRUE(contains(text, "## Inline visuals"));
    EXPECT_TRUE(contains(text, "```visual"));
    EXPECT_TRUE(contains(text, "load the `inline-visuals` skill with the `skill_view` tool"));
    for (const char* rule : {"top level", "`<html>`", "`var(--fg)`", "after the block is closed",
                             "cdnjs.cloudflare.com", "esm.sh", "`ace.send(text)`",
                             "`position: fixed`", "Chart.js 4 and d3 7 are built in"}) {
        EXPECT_TRUE(contains(text, rule)) << rule;
    }
    // mermaid 部分在前,visual 部分在后,中间空一行。
    EXPECT_LT(text.find("```mermaid"), text.find("## Inline visuals"));
    EXPECT_TRUE(contains(text, "in prose.\n\n## Inline visuals"));
    EXPECT_TRUE(inline_visuals_guidance_active(DiagramSurface::Web, true));
}

// 场景:用户在设置 > 常规关闭「可视化图表」。
// 期望:只剩 mermaid 部分,与 Terminal 以外的文字相同起点;不再出现 visual 部分。
TEST(DiagramGuidance, WebWithSwitchOffKeepsMermaidOnly) {
    const auto text = build_diagram_guidance(web(false, true));
    EXPECT_TRUE(contains(text, "```mermaid"));
    EXPECT_FALSE(contains(text, "## Inline visuals"));
    EXPECT_FALSE(contains(text, "inline-visuals"));
    EXPECT_FALSE(inline_visuals_guidance_active(DiagramSurface::Web, false));
}

// 场景:用户在技能设置里停用了 inline-visuals(或专家策略收走了 skill_view)。
// 期望:visual 核心约束仍在,但不再要求加载一个加载不到的技能。
TEST(DiagramGuidance, UnavailableSkillDropsOnlyTheLoadInstruction) {
    const auto with_skill = build_diagram_guidance(web(true, true));
    const auto without = build_diagram_guidance(web(true, false));
    EXPECT_TRUE(contains(without, "## Inline visuals"));
    EXPECT_TRUE(contains(without, "`ace.send(text)`"));
    EXPECT_FALSE(contains(without, "inline-visuals"));
    EXPECT_FALSE(contains(without, "skill_view"));
    EXPECT_LT(without.size(), with_skill.size());
}

// 场景:「工具重写」把 skill_view 映射成别的模型侧名字(例如 load_skill)。
// 期望:说明里引用映射后的名字,不出现原生名 —— 模型照着原生名调用会失败。
TEST(DiagramGuidance, UsesModelFacingSkillViewName) {
    auto options = web(true, true);
    options.skill_view_tool_name = "load_skill";
    const auto text = build_diagram_guidance(options);
    EXPECT_TRUE(contains(text, "with the `load_skill` tool"));
    EXPECT_FALSE(contains(text, "skill_view"));
}

// 场景:同一会话里连续请求、或不同会话 / 不同时间用同一组输入组装系统提示。
// 期望:逐字节相同;不同输入(开关)才得到不同文本。
TEST(DiagramGuidance, SameInputsAreByteIdentical) {
    EXPECT_EQ(build_diagram_guidance(web(true, true)), build_diagram_guidance(web(true, true)));
    EXPECT_EQ(build_diagram_guidance(web(false, true)), build_diagram_guidance(web(false, false)));
    EXPECT_NE(build_diagram_guidance(web(true, true)), build_diagram_guidance(web(false, true)));
}
