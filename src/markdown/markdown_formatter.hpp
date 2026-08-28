#pragma once

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

// Strip AI prompt XML tags (<thinking>, <context>, etc.)
std::string strip_xml_tags(const std::string& content);

// Streaming formatter: caches stable prefix, only re-renders unstable tail.
class StreamingFormatter {
public:
    // Append new delta text and return the full rendered Element.
    ftxui::Element append_delta(const std::string& delta,
                                const FormatOptions& opts = {});
    // Most recently produced Element, reusable for cheap re-render.
    const ftxui::Element& last_element() const;
    // Reset state (new conversation turn).
    void reset();
    // Width/theme changes invalidate the stable prefix; the next append
    // rebuilds from scratch to avoid stale wrapping/colors.
    void set_context(int width, std::uint32_t theme_version);

private:
    std::string full_content_;
    std::string stable_prefix_;
    ftxui::Element cached_stable_;
    ftxui::Element last_element_;
    int width_ = -1;
    std::uint32_t theme_ = 0;
};

} // namespace acecode::markdown
