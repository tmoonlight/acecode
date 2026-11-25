#include "diagram_guidance.hpp"

namespace acecode {
namespace {

// mermaid 能画的类型与会退回源码的写法,「只有 mermaid」与「visual 优先」两种说明共用。
// 类型清单对齐 web/src/lib/mermaidSource.js 的 FAMILY_HEADERS / inspectMermaidSource 与
// TUI 的 mermaid_renderer.cpp:两端都只认这五种。下面列的「会显示成源码」的写法是按较严
// 的一端写的:Web 现在对其中多数改写后照画(见 docs/web-chat/mermaid-rendering.md),
// 提示词仍让模型避开,不用跟着放宽。
constexpr const char* kMermaidTypes =
    "Only these diagram types render on every ACECode surface:\n"
    "- `flowchart TD` or `flowchart LR` (always state the direction)\n"
    "- `sequenceDiagram`\n"
    "- `stateDiagram-v2`\n"
    "- `classDiagram`\n"
    "- `erDiagram`\n\n";

constexpr const char* kMermaidRejectedSyntax =
    "Anything else is shown as raw source: other diagram types (gantt, pie, mindmap, "
    "timeline, journey, gitGraph, quadrant or xy charts, ...), `%%{init}%%` directives, "
    "`---` front matter, `@{ }` shape syntax, HTML tags other than `<br>`, HTML entities, "
    "angle-bracket generics (write `List~String~`), Markdown links, URLs or URL-like "
    "prefixes (`http:`, `file:`, `data:`, `//`), `$$` math, `fa:` icons, and `click`, "
    "`href` or `call` lines. Write labels as plain text, and wrap a label in double quotes "
    "when it contains punctuation such as parentheses, brackets or colons.";

// 只有 mermaid 的说明:TUI,以及关闭「可视化图表」的网页会话。这两种情况的文字与
// visual 优先改版之前逐字节相同,改这里会让这些会话的 prompt cache 前缀整体失效。
std::string mermaid_only_guidance(DiagramSurface surface) {
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
        "something, give them the diagram itself instead of describing it.\n\n";
    out += kMermaidTypes;
    out += kMermaidRejectedSyntax;
    out +=
        "\n\nSkip the diagram for short answers, a single step, or content that reads fine as a "
        "list or table. Keep one diagram under about 30 nodes, put it in your final message, "
        "and still state the key point in prose.";
    return out;
}

// 打开「可视化图表」的网页会话:默认画 visual,mermaid 只留给单纯的流程 / 依赖 / 时序图,
// 复合图形一律 visual(用户 2026-11-26 拍板)。选择规则放在最前面,「画 / 图表 / 示意」这类
// 请求不再先落到 mermaid 段落。变量名、类名、ace.send 与 CDN 清单必须与
// web/src/lib/inlineVisualFrame.js 的外壳一致;细节规则在 inline-visuals 技能里。
std::string visual_first_guidance(const DiagramGuidanceOptions& options) {
    std::string out;
    out +=
        "# Diagrams\n\n"
        "This chat UI draws two kinds of figures: fenced ```visual code blocks, whose SVG or "
        "HTML fragment is rendered live in an isolated frame inside the conversation while "
        "you write it, and fenced ```mermaid code blocks. Reach for a figure when a picture "
        "explains something faster than prose. When the user asks you to draw, diagram, "
        "sketch, chart, illustrate or visualize something, give them the figure itself "
        "instead of describing it.\n\n"
        "Choosing the format:\n"
        "- Default to a ```visual block. Illustrations of how a mechanism works, interactive "
        "explainers (sliders, toggles, step-through controls), charts of real data and "
        "side-by-side comparison cards are all visuals.\n"
        "- Use a ```mermaid block only when the whole figure is a plain flow, dependency or "
        "sequence diagram: boxes and arrows with short labels and nothing else, such as "
        "process steps, call or request flows, state transitions, module dependencies, class "
        "or entity relationships, or messages exchanged between participants.\n"
        "- A composite figure is always a visual, never mermaid, even when part of it is a "
        "flow or a dependency graph. A figure is composite when it combines more than one "
        "kind of content: structure together with numbers or charts, annotations or "
        "callouts, highlighted regions or a legend, several panels or views, a comparison, "
        "or interactive controls.\n\n"
        "Skip the figure for short answers, a single step, or content that reads fine as a "
        "list or table. When you do draw one, put it in your final message.\n\n"
        "## Inline visuals\n\n";
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
        "unpkg.com and esm.sh, and the frame has no other network access. Chart.js 4 and d3 7 "
        "are built in: reference them by their usual CDN URL and they load locally, even "
        "offline. Other libraries need the network, so prefer plain SVG and HTML for simple "
        "figures.\n"
        "- `ace.send(text)` sends `text` as the user's next message; call it from a click "
        "handler to offer follow-up questions.\n"
        "- The user sees the drawing, not the code: do not repeat its content in prose or "
        "explain the markup. A sentence or two of context is enough.\n"
        "- When the user wants HTML or SVG code for their own project, write an ```html or "
        "```svg block instead of a visual.\n\n"
        "## Mermaid\n\n";
    out += kMermaidTypes;
    out += kMermaidRejectedSyntax;
    out +=
        "\n\nKeep one mermaid diagram under about 30 nodes and still state the key point in "
        "prose.";
    return out;
}

} // namespace

bool inline_visuals_guidance_active(DiagramSurface surface, bool inline_visuals_enabled) {
    return surface == DiagramSurface::Web && inline_visuals_enabled;
}

std::string build_diagram_guidance(const DiagramGuidanceOptions& options) {
    if (options.surface == DiagramSurface::None) return {};
    if (inline_visuals_guidance_active(options.surface, options.inline_visuals_enabled)) {
        return visual_first_guidance(options);
    }
    return mermaid_only_guidance(options.surface);
}

} // namespace acecode
