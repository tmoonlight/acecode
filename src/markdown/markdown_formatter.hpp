#pragma once

#include "markdown_lexer.hpp"
#include "markdown_types.hpp"
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
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

// Streaming formatter: ingests delta text through a resumable LexerState and
// builds one Element per stable token. Only newly frozen tokens (from
// LexerState::new_stable_count()) and the unstable tail are re-rendered per
// append; previously built stable Elements are reused as-is, so long
// documents cost O(incremental) per frame instead of a full rebuild.
class StreamingFormatter {
public:
    // Append new delta text and return the full rendered Element.
    ftxui::Element append_delta(const std::string& delta,
                                const FormatOptions& opts = {});
    // Most recently produced Element, reusable for cheap re-render.
    const ftxui::Element& last_element() const;
    // Reset state (new conversation turn).
    void reset();
    // Width/theme changes invalidate the token cache; the next append rebuilds
    // from scratch to avoid stale wrapping/colors. Callers pass the same width
    // that append_delta will receive (see the on_delta path in main.cpp).
    void set_context(int width, std::uint32_t theme_version);

private:
    LexerState lexer_;
    std::vector<ftxui::Element> stable_elements_;
    ftxui::Element last_element_;
    int width_ = -1;
    std::uint32_t theme_ = 0;
};

} // namespace acecode::markdown
