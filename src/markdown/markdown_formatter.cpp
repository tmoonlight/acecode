#include "markdown_formatter.hpp"
#include "markdown_lexer.hpp"
#include "mermaid_renderer.hpp"
#include "syntax_highlight.hpp"
#include "tui/text_style.hpp"
#include "tui/theme_palette.hpp"
#include "utils/logger.hpp"
#include <sstream>
#include <algorithm>
#include <cstdlib>

#include <array>
#include <string_view>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/table.hpp>
#include <ftxui/dom/flexbox_config.hpp>
#include <ftxui/screen/string.hpp>

using namespace ftxui;

namespace acecode::markdown {

MarkdownLinkRegion& MarkdownLinkRegionCollector::add(std::string href) {
    regions_.push_back({std::move(href), {}});
    return regions_.back();
}

void MarkdownLinkRegionCollector::clear() {
    regions_.clear();
}

std::optional<std::string> MarkdownLinkRegionCollector::href_at(
    int x,
    int y) const {
    for (const auto& region : regions_) {
        if (!region.box.IsEmpty() && region.box.Contain(x, y)) {
            return region.href;
        }
    }
    return std::nullopt;
}

const std::deque<MarkdownLinkRegion>&
MarkdownLinkRegionCollector::regions() const {
    return regions_;
}

// ---------------------------------------------------------------------------
// XML tag stripping (matches claude-code stripPromptXMLTags)
// ---------------------------------------------------------------------------

std::string strip_xml_tags(const std::string& content) {
    // Strip known AI prompt tags: <thinking>, <context>, etc.
    // Use simple string search instead of regex for reliability on MSVC
    static const std::vector<std::string> tag_names = {
        "thinking", "context", "commit_analysis", "function_analysis", "pr_analysis"
    };
    
    std::string result = content;
    for (const auto& tag : tag_names) {
        std::string open_tag = "<" + tag + ">";
        std::string close_tag = "</" + tag + ">";
        size_t start = 0;
        while ((start = result.find(open_tag, start)) != std::string::npos) {
            size_t end = result.find(close_tag, start + open_tag.size());
            if (end == std::string::npos) break;
            end += close_tag.size();
            // Also consume trailing newline
            if (end < result.size() && result[end] == '\n') end++;
            result.erase(start, end - start);
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// OSC 8 hyperlink support + terminal capability detection
// ---------------------------------------------------------------------------

static bool terminal_supports_hyperlinks() {
    // Check known terminal emulators that support OSC 8
    auto check_env = [](const char* var) -> bool {
        const char* val = std::getenv(var);
        return val != nullptr && val[0] != '\0';
    };

    // Windows Terminal
    if (check_env("WT_SESSION")) return true;

    const char* term_program = std::getenv("TERM_PROGRAM");
    if (term_program) {
        std::string tp(term_program);
        if (tp == "iTerm.app" || tp == "WezTerm" || tp == "vscode") return true;
    }

    const char* term = std::getenv("TERM");
    if (term) {
        std::string t(term);
        if (t.find("xterm") != std::string::npos) return true;
    }

    return false;
}

static std::string make_hyperlink(const std::string& url, const std::string& display) {
    // OSC 8 format: \033]8;;URL\007DISPLAY\033]8;;\007
    // This only works at the terminal level, not in FTXUI Elements.
    // We'll just return the display text since FTXUI handles rendering.
    (void)url;
    return display;
}

// ---------------------------------------------------------------------------
// List numbering helpers (match claude-code exactly)
// ---------------------------------------------------------------------------

static std::string number_to_letter(int n) {
    std::string result;
    while (n > 0) {
        n--;
        result = static_cast<char>('a' + (n % 26)) + result;
        n /= 26;
    }
    return result;
}

static std::string number_to_roman(int n) {
    static const int values[]       = {1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1};
    static const char* numerals[]   = {"m","cm","d","cd","c","xc","l","xl","x","ix","v","iv","i"};
    std::string result;
    for (int i = 0; i < 13; i++) {
        while (n >= values[i]) {
            result += numerals[i];
            n -= values[i];
        }
    }
    return result;
}

static std::string get_list_number(int depth, int number) {
    switch (depth) {
        case 0:
        case 1: return std::to_string(number);
        case 2: return number_to_letter(number);
        case 3: return number_to_roman(number);
        default: return std::to_string(number);
    }
}

// ---------------------------------------------------------------------------
// Styled text building for paragraphs (word-wrapping with styles)
// ---------------------------------------------------------------------------

// Flatten inline tokens into a list of styled runs
static void flatten_inline(const std::vector<Token>& tokens,
                           const TextStyle& inherited,
                           std::vector<StyledRun>& out) {
    for (const auto& tok : tokens) {
        TextStyle style = inherited;

        switch (tok.type) {
        case TokenType::Text:
        case TokenType::Escape:
            out.push_back({tok.text, style});
            break;

        case TokenType::Strong:
            style.bold = true;
            flatten_inline(tok.children, style, out);
            break;

        case TokenType::Em:
            style.italic = true;
            flatten_inline(tok.children, style, out);
            break;

        case TokenType::CodeSpan:
            style.is_code = true;
            out.push_back({tok.text, style});
            break;

        case TokenType::Link:
            style.is_link = true;
            style.href = tok.href;
            // If link text == href or empty, just show the URL
            if (tok.children.empty()) {
                out.push_back({tok.href, style});
            } else {
                flatten_inline(tok.children, style, out);
            }
            break;

        case TokenType::Image:
            // Show alt text or URL
            out.push_back({tok.text.empty() ? tok.href : tok.text, style});
            break;

        case TokenType::Br:
            out.push_back({"\n", style});
            break;

        case TokenType::Html:
        case TokenType::Del:
            // Ignored
            break;

        default:
            if (!tok.text.empty()) {
                out.push_back({tok.text, style});
            }
            if (!tok.children.empty()) {
                flatten_inline(tok.children, style, out);
            }
            break;
        }
    }
}

// Apply TextStyle to an FTXUI Element.
static Element apply_style(const std::string& txt,
                           const TextStyle& style,
                           const FormatOptions& opts) {
    Element e = text(txt);

    const auto& md = acecode::tui::theme().markdown;
    if (style.is_code) {
        e = e | color(md.code_span);
    } else if (style.is_link) {
        e = e | color(md.link) | underlined;
    } else if (style.bold && style.italic) {
        e = e | color(md.bold);
    } else if (style.bold) {
        e = e | color(md.bold);
    } else if (style.italic) {
        e = e | color(md.italic);
    }

    if (style.bold) e = e | bold;
    if (style.italic) e = e | italic;
    if (style.underline) e = e | underlined;
    if (style.dim) e = e | dim;

    if (style.is_link && opts.hyperlinks && opts.link_regions) {
        auto& region = opts.link_regions->add(style.href);
        e = e | reflect(region.box);
    }

    return e;
}

// Check if a character is a space for word-splitting
static bool is_space_char(char c) {
    return c == ' ' || c == '\t';
}

static bool is_space_glyph(const std::string& g) {
    return g == " " || g == "\t";
}

static bool is_narrow_glyph(const std::string& g) {
    return ftxui::string_width(g) == 1;
}

static bool is_opening_cjk_punct(const std::string& g) {
    static constexpr std::array<std::string_view, 8> kOpening = {
        "\xEF\xBC\x88", "\xE3\x80\x8A", "\xE3\x80\x8C", "\xE3\x80\x90",
        "\xE2\x80\x98", "\xE2\x80\x9C", "\xE3\x80\x88", "\xE3\x80\x8E"
    };
    for (const auto& c : kOpening) { if (g == c) return true; }
    return false;
}

static bool is_closing_cjk_punct(const std::string& g) {
    static constexpr std::array<std::string_view, 15> kClosing = {
        "\xEF\xBC\x8C", "\xE3\x80\x82", "\xEF\xBC\x81", "\xEF\xBC\x9F",
        "\xEF\xBC\x9B", "\xEF\xBC\x9A", "\xE3\x80\x81", "\xEF\xBC\x89",
        "\xE3\x80\x8B", "\xE3\x80\x8D", "\xE3\x80\x91", "\xE2\x80\x99",
        "\xE2\x80\x9D", "\xE3\x80\x89", "\xE3\x80\x8F"
    };
    for (const auto& c : kClosing) { if (g == c) return true; }
    return false;
}

// Split styled runs into word-level Elements for flexbox paragraph wrapping.
// CJK-aware: each wide glyph becomes its own token so flexbox can wrap.
static Elements styled_words(const std::vector<StyledRun>& runs,
                             const FormatOptions& opts) {
    Elements words;

    for (const auto& run : runs) {
        if (run.text == "\n") {
            continue;
        }

        auto glyphs = ftxui::Utf8ToGlyphs(run.text);
        std::string ascii_run;
        std::string pending_prefix;  // opening CJK punctuation to attach

        auto flush_ascii = [&]() {
            if (ascii_run.empty()) return;
            std::string token = std::move(ascii_run);
            ascii_run.clear();
            if (!pending_prefix.empty()) {
                token = std::move(pending_prefix) + token;
                pending_prefix.clear();
            }
            words.push_back(apply_style(std::move(token), run.style, opts));
        };

        for (const auto& g : glyphs) {
            if (g.empty()) continue;

            if (is_space_glyph(g)) {
                flush_ascii();
                // Attach trailing space to previous word
                if (!words.empty()) {
                    // Insert a space element for flexbox gap
                    words.push_back(text(" "));
                }
                continue;
            }

            if (is_opening_cjk_punct(g)) {
                flush_ascii();
                pending_prefix += g;
                continue;
            }

            if (is_closing_cjk_punct(g)) {
                flush_ascii();
                if (!words.empty()) {
                    // Append closing punct to previous token by replacing it
                    // We create a new combined element instead
                    // Simpler: emit as own token (flexbox will keep it adjacent)
                    words.push_back(apply_style(g, run.style, opts));
                } else if (!pending_prefix.empty()) {
                    pending_prefix += g;
                } else {
                    words.push_back(apply_style(g, run.style, opts));
                }
                continue;
            }

            if (is_narrow_glyph(g)) {
                ascii_run += g;
                continue;
            }

            // Wide (CJK) glyph — flush ascii, emit as own token
            flush_ascii();
            std::string token = g;
            if (!pending_prefix.empty()) {
                token = std::move(pending_prefix) + token;
                pending_prefix.clear();
            }
            words.push_back(apply_style(std::move(token), run.style, opts));
        }

        flush_ascii();
        if (!pending_prefix.empty()) {
            if (!words.empty()) {
                // Orphan opening punct — attach to previous
                words.push_back(
                    apply_style(std::move(pending_prefix), run.style, opts));
            } else {
                words.push_back(
                    apply_style(std::move(pending_prefix), run.style, opts));
            }
            pending_prefix.clear();
        }
    }

    return words;
}

// Build a paragraph Element from inline tokens with word wrapping.
// Splits at \n boundaries, then uses flexbox per line (like FTXUI paragraph).
static Element styled_paragraph(const std::vector<Token>& inline_tokens,
                                const FormatOptions& opts) {
    std::vector<StyledRun> runs;
    TextStyle base_style;
    flatten_inline(inline_tokens, base_style, runs);

    // Split runs at \n boundaries
    std::vector<std::vector<StyledRun>> line_runs;
    line_runs.push_back({});
    for (const auto& run : runs) {
        if (run.text.find('\n') != std::string::npos) {
            // Split this run at newlines
            std::istringstream stream(run.text);
            std::string segment;
            bool first = true;
            while (std::getline(stream, segment, '\n')) {
                if (!first) {
                    line_runs.push_back({});
                }
                if (!segment.empty()) {
                    line_runs.back().push_back({segment, run.style});
                }
                first = false;
            }
        } else {
            line_runs.back().push_back(run);
        }
    }

    // Build each line as a flexbox of styled words
    Elements line_elements;
    static const auto config = FlexboxConfig().SetGap(0, 0);

    for (const auto& lr : line_runs) {
        auto words = styled_words(lr, opts);
        if (words.empty()) {
            line_elements.push_back(text(""));
        } else {
            line_elements.push_back(flexbox(std::move(words), config));
        }
    }

    if (line_elements.empty()) return text("");
    if (line_elements.size() == 1) return std::move(line_elements[0]);
    return vbox(std::move(line_elements));
}

// ---------------------------------------------------------------------------
// Block token formatting (recursive)
// ---------------------------------------------------------------------------

struct FormatContext {
    FormatOptions opts;
    int list_depth = 0;
    int ordered_number = -1;  // -1 = unordered
    const Token* parent = nullptr;
};

static Element format_block_token(const Token& token, const FormatContext& ctx);

static Color mermaid_role_color(MermaidRole role) {
    const auto& palette = acecode::tui::theme();
    switch (role) {
        case MermaidRole::Border: return palette.ui.border;
        case MermaidRole::NodeText: return palette.markdown.block_code_text;
        case MermaidRole::Edge: return palette.ui.text_dim;
        case MermaidRole::EdgeLabel: return palette.markdown.italic;
        case MermaidRole::Title: return palette.markdown.heading;
        case MermaidRole::Hint: return palette.ui.text_secondary;
    }
    return palette.markdown.block_code_text;
}

static Element format_mermaid_art(const MermaidArt& art) {
    Elements rows;
    rows.reserve(art.lines.size());
    for (const auto& line : art.lines) {
        Elements spans;
        spans.reserve(line.spans.size());
        for (const auto& span : line.spans) {
            Element element = text(span.text) | color(mermaid_role_color(span.role));
            if (span.italic) element = element | italic;
            spans.push_back(std::move(element));
        }
        rows.push_back(spans.empty() ? text("") : hbox(std::move(spans)));
    }
    return vbox({
        hbox({text("  "), vbox(std::move(rows))}),
        text("")
    });
}

// Format a list of block tokens
static Element format_blocks(const std::vector<Token>& tokens, const FormatContext& ctx) {
    Elements elems;
    for (const auto& tok : tokens) {
        elems.push_back(format_block_token(tok, ctx));
    }
    if (elems.empty()) return text("");
    return vbox(std::move(elems));
}

static Element format_block_token(const Token& token, const FormatContext& ctx) {
    switch (token.type) {

    // -- Heading --
    case TokenType::Heading: {
        Element content = styled_paragraph(token.children, ctx.opts);
        switch (token.depth) {
            case 1:
                content = content | bold | italic | underlined | color(acecode::tui::theme().markdown.heading);
                break;
            case 2:
                content = content | bold | color(acecode::tui::theme().markdown.heading);
                break;
            default:
                content = content | bold | color(acecode::tui::theme().ui.text_primary);
                break;
        }
        return vbox({content, text("")});
    }

    // -- Paragraph --
    case TokenType::Paragraph: {
        return styled_paragraph(token.children, ctx.opts);
    }

    // -- Code block --
    case TokenType::Code: {
        if (token.lang == "mermaid" &&
            token.text.find_first_not_of(" \t\r\n") != std::string::npos) {
            try {
                const int width = std::max(8, ctx.opts.terminal_width - 2);
                if (auto art = render_mermaid_terminal(token.text, width)) {
                    return format_mermaid_art(*art);
                }
            } catch (...) {
                // Untrusted diagram input must never break message rendering.
                // Continue through the ordinary fenced-code presentation.
            }
        }

        Elements code_elements;

        // Language label
        if (!token.lang.empty()) {
            code_elements.push_back(
                text(" " + token.lang + " ") |
                    acecode::tui::readable_secondary()
            );
        }

        if (ctx.opts.syntax_highlight && supports_language(token.lang)) {
            auto highlighted_lines = highlight_code(token.text, token.lang);
            for (auto& line : highlighted_lines) {
                code_elements.push_back(hbox({text("  "), std::move(line)}));
            }
        } else {
            // No highlighting — dim monochrome
            std::istringstream stream(token.text);
            std::string line;
            while (std::getline(stream, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                code_elements.push_back(hbox({text("  "), text(line) | color(acecode::tui::theme().markdown.block_code_text)}));
            }
        }

        // Wrap in a border-left indicator
        auto code_block = vbox(std::move(code_elements));
        return vbox({
            hbox({text("  ") | color(acecode::tui::theme().ui.text_dim), code_block}),
            text("")
        });
    }

    // -- Blockquote --
    case TokenType::Blockquote: {
        // Recursively format blockquote content
        FormatContext bq_ctx = ctx;
        Element inner = format_blocks(token.children, bq_ctx);
        // Prefix with dim vertical bar (like claude-code's BLOCKQUOTE_BAR)
        return hbox({
            text(" | ") | dim | color(acecode::tui::theme().markdown.block_quote),
            inner | italic
        });
    }

    // -- Horizontal rule --
    case TokenType::Hr: {
        return text("---") | dim;
    }

    // -- Space (empty line) --
    case TokenType::Space: {
        return text("");
    }

    // -- List --
    case TokenType::List: {
        Elements items;
        int num = token.start;
        for (const auto& item : token.children) {
            FormatContext item_ctx = ctx;
            item_ctx.list_depth = ctx.list_depth;
            item_ctx.ordered_number = token.ordered ? num : -1;
            item_ctx.parent = &token;
            items.push_back(format_block_token(item, item_ctx));
            num++;
        }
        return vbox(std::move(items));
    }

    // -- List item --
    case TokenType::ListItem: {
        // Build prefix: indentation + marker
        std::string indent(ctx.list_depth * 2, ' ');
        std::string marker;
        if (ctx.ordered_number >= 0) {
            marker = get_list_number(ctx.list_depth, ctx.ordered_number) + ". ";
        } else {
            marker = "- ";
        }

        // Format item content
        Elements content_parts;
        for (const auto& child : token.children) {
            if (child.type == TokenType::Text) {
                // Inline text — render as styled paragraph
                content_parts.push_back(styled_paragraph(
                    child.children.empty()
                        ? parse_inline(child.text)
                        : child.children,
                    ctx.opts));
            } else if (child.type == TokenType::List) {
                // Nested list
                FormatContext nested = ctx;
                nested.list_depth = ctx.list_depth + 1;
                content_parts.push_back(format_block_token(child, nested));
            } else {
                FormatContext nested = ctx;
                nested.list_depth = ctx.list_depth + 1;
                content_parts.push_back(format_block_token(child, nested));
            }
        }

        Element content = content_parts.empty() ? text("") :
                          (content_parts.size() == 1 ? std::move(content_parts[0]) :
                           vbox(std::move(content_parts)));

        return hbox({
            text(indent + marker) | color(acecode::tui::theme().markdown.list_marker),
            content
        });
    }

    // -- Table --
    case TokenType::Table: {
        if (token.header_cells.empty()) return text("");

        size_t num_cols = token.header_cells.size();

        // Build FTXUI Table data: first row is header, rest are body rows
        std::vector<std::vector<Element>> table_data;

        // Header row
        std::vector<Element> header_row;
        for (const auto& cell : token.header_cells) {
            header_row.push_back(styled_paragraph(cell, ctx.opts));
        }
        table_data.push_back(std::move(header_row));

        // Body rows
        for (const auto& row : token.body_rows) {
            std::vector<Element> elem_row;
            for (size_t c = 0; c < num_cols; c++) {
                if (c < row.size()) {
                    elem_row.push_back(styled_paragraph(row[c], ctx.opts));
                } else {
                    elem_row.push_back(text(""));
                }
            }
            table_data.push_back(std::move(elem_row));
        }

        auto table = Table(std::move(table_data));

        // Style the table
        table.SelectAll().Border(LIGHT);
        table.SelectRow(0).Border(DOUBLE);
        table.SelectRow(0).Decorate(bold);
        table.SelectRow(0).Separator(LIGHT);

        // Apply alignment decorators
        for (size_t c = 0; c < num_cols && c < token.align.size(); c++) {
            if (token.align[c] == "center") {
                table.SelectColumn(static_cast<int>(c)).DecorateCells(center);
            } else if (token.align[c] == "right") {
                table.SelectColumn(static_cast<int>(c)).DecorateCells(align_right);
            }
        }

        return vbox({std::move(table).Render(), text("")});
    }

    // -- Inline text (shouldn't appear at block level, but handle gracefully) --
    case TokenType::Text: {
        if (!token.children.empty()) {
            return styled_paragraph(token.children, ctx.opts);
        }
        auto inline_tokens = parse_inline(token.text);
        return styled_paragraph(inline_tokens, ctx.opts);
    }

    // -- Other inline types at block level (shouldn't normally happen) --
    case TokenType::Strong:
    case TokenType::Em:
    case TokenType::CodeSpan:
    case TokenType::Link:
    case TokenType::Image:
    case TokenType::Br:
    case TokenType::Escape:
    case TokenType::Html:
    case TokenType::Del:
        return text("");
    }

    return text("");
}

// ---------------------------------------------------------------------------
// Main entry point
// ---------------------------------------------------------------------------

// Render a list of block-level tokens into an FTXUI Element tree. Behavior is
// identical to the block-formatting half of format_markdown, so callers can
// reuse a token stream without re-running normalize/strip_xml/lex.
Element render_token_blocks(const std::vector<Token>& tokens,
                            const FormatOptions& opts) {
    FormatContext ctx;
    ctx.opts = opts;
    return format_blocks(tokens, ctx);
}

Element format_markdown(const std::string& raw_text, const FormatOptions& opts) {
    // Step 1: Strip XML tags
    std::string content = opts.strip_xml ? strip_xml_tags(raw_text) : raw_text;

    // Step 2: Normalize line endings
    std::string normalized;
    normalized.reserve(content.size());
    for (size_t i = 0; i < content.size(); i++) {
        if (content[i] == '\r' && i + 1 < content.size() && content[i + 1] == '\n') {
            continue;
        }
        normalized += content[i];
    }

    // Step 3: Lex + render block tokens
    return render_token_blocks(lex(normalized), opts);
}

// ---------------------------------------------------------------------------
// Streaming formatter
// ---------------------------------------------------------------------------

// Freeze-safety and code-fence detection are shared with the incremental
// LexerState (declared in markdown_lexer.hpp), so the stable-boundary rules
// cannot drift between the resumable lexer and the streaming formatter.

Element StreamingFormatter::append_delta(const std::string& delta,
                                         const FormatOptions& opts) {
    // Build Elements for the stable tokens not yet in stable_elements_. After
    // a reset()/replay this covers all stable tokens; for a normal append it
    // covers only the newly frozen ones (new_stable_count()).
    auto build_new_stable = [&]() {
        const std::size_t new_count = lexer_.new_stable_count();
        if (new_count == 0) return;
        const auto& stable = lexer_.stable_tokens();
        const std::size_t begin = stable.size() - new_count;
        for (std::size_t k = begin; k < stable.size(); ++k) {
            stable_elements_.push_back(
                render_token_blocks({stable[k]}, opts));
        }
    };

    // A width change invalidates the per-token Element cache. set_context owns
    // the normal invalidation path (it is called with the same width used here
    // before the first append); this check is a defensive fallback so a width
    // mismatch can never mix stale and fresh wrapping. The accumulated
    // full_content_ is replayed through the lexer, so a mid-stream width
    // change (a resize or Ctrl+O changes streaming_render_width without
    // calling set_context) rebuilds every previously streamed block at the
    // new width instead of dropping it from the view.
    if (width_ != opts.terminal_width) {
        lexer_.reset();
        stable_elements_.clear();
        width_ = opts.terminal_width;
        // Replay the content accumulated before this delta so the stable
        // region and stable_elements_ return to their pre-change state under
        // the new width; the new delta is appended below.
        if (!full_content_.empty()) {
            lexer_.append(full_content_);
            build_new_stable();
        }
    }

    lexer_.append(delta);
    full_content_ += delta;
    build_new_stable();
    auto tail = lexer_.tail_tokens();
    Element tail_elem = tail.empty() ? emptyElement()
                                     : render_token_blocks(tail, opts);
    // Copy (not move) stable_elements_ into the vbox: the per-token cache must
    // survive across append_delta calls so future deltas only append newly
    // frozen tokens. Elements are shared_ptrs, so the copy is shallow.
    Element stable_elem = stable_elements_.empty()
        ? emptyElement() : vbox(stable_elements_);
    last_element_ = (tail.empty())
        ? stable_elem : vbox({stable_elem, tail_elem});
    return last_element_;
}

const Element& StreamingFormatter::last_element() const {
    return last_element_;
}

void StreamingFormatter::set_context(int width, std::uint32_t theme_version) {
    if (width != width_ || theme_version != theme_) {
        lexer_.reset();
        stable_elements_.clear();
        last_element_ = text("");
    }
    width_ = width;
    theme_ = theme_version;
}

void StreamingFormatter::reset() {
    lexer_.reset();
    stable_elements_.clear();
    full_content_.clear();
    last_element_ = text("");
}

} // namespace acecode::markdown
