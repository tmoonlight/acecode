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
// documents cost O(incremental) per frame instead of a full rebuild. The
// streamed text is also accumulated in full_content_ so a mid-stream width
// change can replay everything through the lexer and rebuild the stable
// region at the new width instead of dropping the streamed view.
class StreamingFormatter {
public:
    // Append new delta text and return the full rendered Element.
    ftxui::Element append_delta(const std::string& delta,
                                const FormatOptions& opts = {});
    // Most recently produced Element, reusable for cheap re-render.
    const ftxui::Element& last_element() const;
    // Reset state (new conversation turn). Clears the accumulated
    // full_content_ along with the lexer and the stable-Element cache.
    void reset();
    // Width/theme changes invalidate the cached stable render; the next append
    // rebuilds it at the new width (see append_delta). The accumulated
    // full_content_ is retained, so no previously streamed text is lost.
    // Callers pass the same width that append_delta will receive (see the
    // on_delta path in main.cpp).
    void set_context(int width, std::uint32_t theme_version);

private:
    LexerState lexer_;
    std::vector<ftxui::Element> stable_elements_;
    // Cached vbox of stable_elements_, rebuilt only when new stable tokens are
    // added (R14: avoids O(#stable) per-frame vector copy). Empty until the
    // first stable token is frozen.
    ftxui::Element stable_vbox_;
    // Append-only buffer of all text streamed since the last reset(); cleared
    // only by reset(). Used to replay accumulated content when the width
    // changes mid-stream (see append_delta).
    std::string full_content_;
    ftxui::Element last_element_;
    int width_ = -1;
    std::uint32_t theme_ = 0;
};

} // namespace acecode::markdown
