#include "diagram_guidance.hpp"

namespace acecode {
namespace {

// mermaid 部分。类型清单与拒绝写法对齐 web/src/lib/mermaidRenderer.js 的
// FAMILY_HEADERS / inspectMermaidSource 与 TUI 的 mermaid_renderer.cpp:两端都只认
// 这五种;其余写法在 Web 上会退回源码。改这里之前先看那两处。
std::string mermaid_part(DiagramSurface surface) {
    std::string out;
    out += "# Diagrams\n\n";
    if (surface == DiagramSurface::Terminal) {
        out += "This terminal UI draws fenced ```mermaid code blocks as text-art diagrams. ";
    } else {
        out += "This chat UI draws fenced ```mermaid code blocks as diagrams. ";
    }
    out +=
        "Reach for one when a picture shows structure or flow faster than prose: call or "
        "request flows, module dependencies, state machines, class or entity relationships, "
        "multi-step processes. When the user asks you to draw, diagram, sketch or chart "
        "something, give them the diagram itself instead of describing it.\n\n"
        "Only these diagram types render on every ACECode surface:\n"
        "- `flowchart TD` or `flowchart LR` (always state the direction)\n"
        "- `sequenceDiagram`\n"
        "- `stateDiagram-v2`\n"
        "- `classDiagram`\n"
        "- `erDiagram`\n\n"
        "Anything else is shown as raw source: other diagram types (gantt, pie, mindmap, "
        "timeline, journey, gitGraph, quadrant or xy charts, ...), `%%{init}%%` directives, "
        "`---` front matter, `@{ }` shape syntax, HTML tags other than `<br>`, HTML entities, "
        "angle-bracket generics (write `List~String~`), Markdown links, URLs or URL-like "
        "prefixes (`http:`, `file:`, `data:`, `//`), `$$` math, `fa:` icons, and `click`, "
        "`href` or `call` lines. Write labels as plain text, and wrap a label in double quotes "
        "when it contains punctuation such as parentheses, brackets or colons.\n\n"
        "Skip the diagram for short answers, a single step, or content that reads fine as a "
        "list or table. Keep one diagram under about 30 nodes, put it in your final message, "
        "and still state the key point in prose.";
    return out;
}

// visual 部分。变量名、类名、ace.send 与 CDN 清单必须与
// web/src/lib/inlineVisualFrame.js 的外壳一致;细节规则在 inline-visuals 技能里。
std::string visual_part(const DiagramGuidanceOptions& options) {
    std::string out;
    out +=
        "## Inline visuals\n\n"
        "This interface also draws fenced ```visual code blocks. The block holds an SVG or "
        "HTML fragment that is rendered live, in an isolated frame inside the conversation, "
        "while you write it. Use a visual for what mermaid cannot express well: illustrations "
        "of how a mechanism works, interactive explainers (sliders, toggles, step-through "
        "controls), charts of real data, and side-by-side comparison cards. Plain "
        "boxes-and-arrows structure stays in mermaid.\n\n";
    if (options.visual_skill_available) {
        out += "Before writing your first visual in a conversation, load the `";
        out += kInlineVisualsSkillName;
        out += "` skill with the `";
        out += options.skill_view_tool_name.empty() ? std::string("skill_view")
                                                    : options.skill_view_tool_name;
        out += "` tool and follow its layout, color and scripting rules.\n\n";
    }
    out +=
        "Rules:\n"
        "- One figure per ```visual block, written at the top level of the reply (not inside "
        "a list or quote). The content is a fragment: start with `<svg` or an HTML element; "
        "never write `<!DOCTYPE>`, `<html>`, `<head>` or `<body>`.\n"
        "- Color with the frame's theme variables (`var(--fg)`, `var(--fg-2)`, "
        "`var(--surface)`, `var(--border)`, `var(--accent)`) so the figure follows light and "
        "dark mode. Give an `<svg>` a `viewBox`, `width=\"100%\"` and no fixed height.\n"
        "- Order the fragment as `<style>`, then markup, then `<script>`. Scripts run once, "
        "after the block is closed. Never use `position: fixed`.\n"
        "- Scripts, styles and fonts load only from cdnjs.cloudflare.com, cdn.jsdelivr.net, "
        "unpkg.com and esm.sh, and the frame has no other network access. Prefer plain SVG "
        "and HTML, which also work offline.\n"
        "- `ace.send(text)` sends `text` as the user's next message; call it from a click "
        "handler to offer follow-up questions.\n"
        "- The user sees the drawing, not the code: do not repeat its content in prose or "
        "explain the markup. A sentence or two of context is enough.\n"
        "- Do not draw a visual when text, a table or a mermaid diagram already works, or "
        "when the user wants HTML or SVG code for their own project (write an ```html or "
        "```svg block for that).";
    return out;
}

} // namespace

bool inline_visuals_guidance_active(DiagramSurface surface, bool inline_visuals_enabled) {
    return surface == DiagramSurface::Web && inline_visuals_enabled;
}

std::string build_diagram_guidance(const DiagramGuidanceOptions& options) {
    if (options.surface == DiagramSurface::None) return {};
    std::string out = mermaid_part(options.surface);
    if (inline_visuals_guidance_active(options.surface, options.inline_visuals_enabled)) {
        out += "\n\n";
        out += visual_part(options);
    }
    return out;
}

} // namespace acecode
