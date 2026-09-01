#pragma once

#include "markdown_types.hpp"
#include <deque>
#include <optional>
#include <string>
#include <vector>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

namespace acecode::markdown {

struct MarkdownLinkRegion {
    std::string href;
    ftxui::Box box;
};

// Per-frame storage for rendered Markdown link fragments. std::deque keeps
// region addresses stable while reflect() holds references during layout.
class MarkdownLinkRegionCollector {
public:
    MarkdownLinkRegion& add(std::string href);
    void clear();

    std::optional<std::string> href_at(int x, int y) const;
    const std::deque<MarkdownLinkRegion>& regions() const;

private:
    std::deque<MarkdownLinkRegion> regions_;
};

// Main entry: convert raw Markdown text to an FTXUI Element tree.
ftxui::Element format_markdown(const std::string& raw_text,
                               const FormatOptions& opts = {});

// Render a list of block-level tokens into an FTXUI Element tree. The
// reusable "tokens -> Element" half of format_markdown; the L3 incremental
// path calls this directly to re-render a token stream without re-normalizing
// raw text.
ftxui::Element render_token_blocks(const std::vector<Token>& tokens,
                                   const FormatOptions& opts);

// Strip AI prompt XML tags (<thinking>, <context>, etc.)
std::string strip_xml_tags(const std::string& content);

// Correctness-first compatibility formatter. It accumulates deltas but routes
// every result through format_markdown(full_content_, opts), preserving the
// complete parser and XML-filtering semantics. It is intentionally not wired
// into the production TUI; future incremental work must first prove semantic
// equivalence for every streamed prefix.
class StreamingFormatter {
public:
    // Append new delta text and return the full rendered Element.
    ftxui::Element append_delta(const std::string& delta,
                                const FormatOptions& opts = {});
    // Reset accumulated source for a new conversation turn.
    void reset();

private:
    std::string full_content_;
};

} // namespace acecode::markdown
