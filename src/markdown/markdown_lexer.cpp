#include "markdown_lexer.hpp"
#include <sstream>
#include <algorithm>
#include <cassert>

namespace acecode::markdown {

// ---------------------------------------------------------------------------
// Helper utilities
// ---------------------------------------------------------------------------

static inline std::string rtrim(const std::string& s) {
    auto end = s.find_last_not_of(" \t\r\n");
    return (end == std::string::npos) ? "" : s.substr(0, end + 1);
}

static inline std::string ltrim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    return (start == std::string::npos) ? "" : s.substr(start);
}

static inline std::string trim(const std::string& s) {
    return ltrim(rtrim(s));
}

// Count leading spaces/tabs (tab = 4 spaces equivalent)
static int count_indent(const std::string& line) {
    int indent = 0;
    for (char c : line) {
        if (c == ' ') indent++;
        else if (c == '\t') indent += 4;
        else break;
    }
    return indent;
}

// Remove up to `n` spaces of indentation from a line
static std::string remove_indent(const std::string& line, int n) {
    int removed = 0;
    size_t i = 0;
    while (i < line.size() && removed < n) {
        if (line[i] == ' ') { removed++; i++; }
        else if (line[i] == '\t') { removed += 4; i++; }
        else break;
    }
    return line.substr(i);
}

// Split text into lines
static std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        // Strip trailing \r
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

// Check if a line is a thematic break (---, ***, ___)
static bool is_thematic_break(const std::string& line) {
    auto t = trim(line);
    if (t.size() < 3) return false;
    char ch = t[0];
    if (ch != '-' && ch != '*' && ch != '_') return false;
    for (char c : t) {
        if (c != ch && c != ' ') return false;
    }
    // Count the actual break characters
    int count = 0;
    for (char c : t) {
        if (c == ch) count++;
    }
    return count >= 3;
}

// Check if a line is an ATX heading, return depth (0 if not)
static int atx_heading_depth(const std::string& line) {
    auto t = ltrim(line);
    int depth = 0;
    size_t i = 0;
    while (i < t.size() && t[i] == '#' && depth < 6) {
        depth++;
        i++;
    }
    if (depth == 0) return 0;
    // Must be followed by space or end of line
    if (i < t.size() && t[i] != ' ' && t[i] != '\t') return 0;
    return depth;
}

// Extract heading text after the # characters
static std::string heading_text(const std::string& line) {
    auto t = ltrim(line);
    size_t i = 0;
    while (i < t.size() && t[i] == '#') i++;
    while (i < t.size() && (t[i] == ' ' || t[i] == '\t')) i++;
    auto result = t.substr(i);
    // Remove trailing # and whitespace
    auto end = result.find_last_not_of("# \t");
    if (end != std::string::npos) {
        result = result.substr(0, end + 1);
    }
    return result;
}

// Check if a line starts a fenced code block (``` or ~~~)
static bool is_code_fence(const std::string& line, char& fence_char, int& fence_count) {
    auto t = ltrim(line);
    if (t.size() < 3) return false;
    char ch = t[0];
    if (ch != '`' && ch != '~') return false;
    int count = 0;
    size_t i = 0;
    while (i < t.size() && t[i] == ch) { count++; i++; }
    if (count < 3) return false;
    // For backtick fences, the info string must not contain backticks
    if (ch == '`') {
        for (size_t j = i; j < t.size(); j++) {
            if (t[j] == '`') return false;
        }
    }
    fence_char = ch;
    fence_count = count;
    return true;
}

// Extract language from code fence opening line
static std::string code_fence_lang(const std::string& line) {
    auto t = ltrim(line);
    size_t i = 0;
    while (i < t.size() && (t[i] == '`' || t[i] == '~')) i++;
    auto info = trim(t.substr(i));
    // Take first word as language
    auto space = info.find_first_of(" \t");
    if (space != std::string::npos) {
        info = info.substr(0, space);
    }
    // Normalize to lowercase
    std::transform(info.begin(), info.end(), info.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return info;
}

// Check if a line is a blockquote start
static bool is_blockquote(const std::string& line) {
    auto t = ltrim(line);
    return !t.empty() && t[0] == '>';
}

// Strip the blockquote prefix (>) from a line
static std::string strip_blockquote(const std::string& line) {
    auto t = ltrim(line);
    if (t.empty() || t[0] != '>') return line;
    t = t.substr(1);
    if (!t.empty() && t[0] == ' ') t = t.substr(1);
    return t;
}

// Check if a line starts an unordered list item, returns marker width (0 if not)
static int is_unordered_list_item(const std::string& line) {
    int indent = count_indent(line);
    auto stripped = ltrim(line);
    if (stripped.empty()) return 0;
    char marker = stripped[0];
    if (marker != '-' && marker != '*' && marker != '+') return 0;
    if (stripped.size() < 2 || stripped[1] != ' ') return 0;
    return indent + 2;
}

// Check if a line starts an ordered list item, returns marker width (0 if not)
static int is_ordered_list_item(const std::string& line, int& start_num) {
    int indent = count_indent(line);
    auto stripped = ltrim(line);
    if (stripped.empty()) return 0;
    size_t i = 0;
    while (i < stripped.size() && stripped[i] >= '0' && stripped[i] <= '9') i++;
    if (i == 0 || i > 9) return 0; // No digits or too many
    if (i >= stripped.size()) return 0;
    if (stripped[i] != '.' && stripped[i] != ')') return 0;
    if (i + 1 >= stripped.size() || stripped[i + 1] != ' ') return 0;
    start_num = std::stoi(stripped.substr(0, i));
    return indent + static_cast<int>(i) + 2;
}

// Check if a line could be a GFM table separator (|---|---|)
static bool is_table_separator(const std::string& line) {
    auto t = trim(line);
    if (t.empty() || t[0] != '|') return false;
    if (t.back() != '|') return false;
    
    // Manual parse: expect |---| pattern (with optional colons for alignment)
    bool has_dash_cell = false;
    size_t i = 1; // skip first |
    while (i < t.size()) {
        // Skip whitespace
        while (i < t.size() && (t[i] == ' ' || t[i] == '\t')) i++;
        if (i >= t.size()) break;
        if (t[i] == '|') { i++; continue; }
        
        // Expect optional :, then dashes, then optional :, then |
        if (t[i] == ':') i++;
        size_t dash_start = i;
        while (i < t.size() && t[i] == '-') i++;
        if (i == dash_start) return false; // no dashes found
        if (i < t.size() && t[i] == ':') i++;
        // Skip whitespace before |
        while (i < t.size() && (t[i] == ' ' || t[i] == '\t')) i++;
        if (i < t.size() && t[i] == '|') {
            has_dash_cell = true;
            i++;
        } else if (i >= t.size()) {
            has_dash_cell = true;
        } else {
            return false;
        }
    }
    return has_dash_cell;
}

// Check if a line looks like a table row (starts and ends with |)
static bool is_table_row(const std::string& line) {
    auto t = trim(line);
    return t.size() >= 3 && t.front() == '|' && t.back() == '|';
}

// Split a table row into cells (splitting on | but respecting escapes)
static std::vector<std::string> split_table_cells(const std::string& line) {
    auto t = trim(line);
    // Strip leading and trailing |
    if (!t.empty() && t.front() == '|') t = t.substr(1);
    if (!t.empty() && t.back() == '|') t.pop_back();

    std::vector<std::string> cells;
    std::string current;
    bool escaped = false;
    bool in_code = false;

    for (size_t i = 0; i < t.size(); i++) {
        if (escaped) {
            current += t[i];
            escaped = false;
            continue;
        }
        if (t[i] == '\\') {
            escaped = true;
            current += t[i];
            continue;
        }
        if (t[i] == '`') {
            in_code = !in_code;
            current += t[i];
            continue;
        }
        if (t[i] == '|' && !in_code) {
            cells.push_back(trim(current));
            current.clear();
            continue;
        }
        current += t[i];
    }
    cells.push_back(trim(current));
    return cells;
}

// Parse alignment from table separator cells
static std::vector<std::string> parse_table_alignments(const std::string& sep_line) {
    auto cells = split_table_cells(sep_line);
    std::vector<std::string> aligns;
    for (const auto& cell : cells) {
        auto t = trim(cell);
        if (t.empty()) {
            aligns.push_back("");
            continue;
        }
        bool left_colon = t.front() == ':';
        bool right_colon = t.back() == ':';
        if (left_colon && right_colon) aligns.push_back("center");
        else if (right_colon) aligns.push_back("right");
        else if (left_colon) aligns.push_back("left");
        else aligns.push_back("");
    }
    return aligns;
}

// ---------------------------------------------------------------------------
// Inline Parser
// ---------------------------------------------------------------------------

std::vector<Token> parse_inline(const std::string& text) {
    std::vector<Token> tokens;
    size_t pos = 0;
    std::string pending_text;

    auto flush_text = [&]() {
        if (!pending_text.empty()) {
            Token t;
            t.type = TokenType::Text;
            t.text = std::move(pending_text);
            t.raw = t.text;
            tokens.push_back(std::move(t));
            pending_text.clear();
        }
    };

    while (pos < text.size()) {
        // Escape: \char
        if (text[pos] == '\\' && pos + 1 < text.size()) {
            char next = text[pos + 1];
            // Only escape punctuation characters
            if (next == '\\' || next == '`' || next == '*' || next == '_' ||
                next == '{' || next == '}' || next == '[' || next == ']' ||
                next == '(' || next == ')' || next == '#' || next == '+' ||
                next == '-' || next == '.' || next == '!' || next == '|' ||
                next == '~') {
                flush_text();
                Token t;
                t.type = TokenType::Escape;
                t.text = std::string(1, next);
                t.raw = text.substr(pos, 2);
                tokens.push_back(std::move(t));
                pos += 2;
                continue;
            }
        }

        // Inline code: `code` or ``code``
        if (text[pos] == '`') {
            // Count opening backticks
            size_t bt_start = pos;
            int bt_count = 0;
            while (pos < text.size() && text[pos] == '`') { bt_count++; pos++; }

            // Find matching closing backticks
            size_t search = pos;
            bool found = false;
            while (search < text.size()) {
                if (text[search] == '`') {
                    int close_count = 0;
                    size_t close_start = search;
                    while (search < text.size() && text[search] == '`') {
                        close_count++;
                        search++;
                    }
                    if (close_count == bt_count) {
                        // Found matching close
                        flush_text();
                        Token t;
                        t.type = TokenType::CodeSpan;
                        std::string code_text = text.substr(pos, close_start - pos);
                        // Strip single leading/trailing space if both present
                        if (code_text.size() >= 2 && code_text.front() == ' ' && code_text.back() == ' ') {
                            code_text = code_text.substr(1, code_text.size() - 2);
                        }
                        t.text = std::move(code_text);
                        t.raw = text.substr(bt_start, search - bt_start);
                        tokens.push_back(std::move(t));
                        pos = search;
                        found = true;
                        break;
                    }
                } else {
                    search++;
                }
            }
            if (found) continue;
            // No matching close — treat backticks as literal
            pending_text += text.substr(bt_start, pos - bt_start);
            continue;
        }

        // Strong + Em: ***text*** or ___text___
        if ((text[pos] == '*' || text[pos] == '_') &&
            pos + 2 < text.size() && text[pos + 1] == text[pos] && text[pos + 2] == text[pos]) {
            char marker = text[pos];
            size_t end = text.find(std::string(3, marker), pos + 3);
            if (end != std::string::npos) {
                flush_text();
                std::string inner = text.substr(pos + 3, end - pos - 3);
                Token strong_tok;
                strong_tok.type = TokenType::Strong;
                strong_tok.raw = text.substr(pos, end + 3 - pos);

                Token em_tok;
                em_tok.type = TokenType::Em;
                em_tok.children = parse_inline(inner);
                strong_tok.children.push_back(std::move(em_tok));

                tokens.push_back(std::move(strong_tok));
                pos = end + 3;
                continue;
            }
        }

        // Strong: **text** or __text__
        if ((text[pos] == '*' || text[pos] == '_') &&
            pos + 1 < text.size() && text[pos + 1] == text[pos]) {
            char marker = text[pos];
            size_t end = text.find(std::string(2, marker), pos + 2);
            if (end != std::string::npos) {
                flush_text();
                std::string inner = text.substr(pos + 2, end - pos - 2);
                Token t;
                t.type = TokenType::Strong;
                t.raw = text.substr(pos, end + 2 - pos);
                t.children = parse_inline(inner);
                tokens.push_back(std::move(t));
                pos = end + 2;
                continue;
            }
        }

        // Em: *text* or _text_
        if (text[pos] == '*' || text[pos] == '_') {
            char marker = text[pos];
            // For underscore, check word boundary (don't match mid-word underscores)
            if (marker == '_' && pos > 0 && std::isalnum(static_cast<unsigned char>(text[pos - 1]))) {
                pending_text += text[pos];
                pos++;
                continue;
            }
            size_t end = pos + 1;
            while (end < text.size()) {
                if (text[end] == marker) {
                    // For underscore, check that next char is not alphanumeric
                    if (marker == '_' && end + 1 < text.size() &&
                        std::isalnum(static_cast<unsigned char>(text[end + 1]))) {
                        end++;
                        continue;
                    }
                    break;
                }
                if (text[end] == '\\' && end + 1 < text.size()) {
                    end += 2;
                } else {
                    end++;
                }
            }
            if (end < text.size() && end > pos + 1) {
                flush_text();
                std::string inner = text.substr(pos + 1, end - pos - 1);
                Token t;
                t.type = TokenType::Em;
                t.raw = text.substr(pos, end + 1 - pos);
                t.children = parse_inline(inner);
                tokens.push_back(std::move(t));
                pos = end + 1;
                continue;
            }
            pending_text += text[pos];
            pos++;
            continue;
        }

        // Image: ![alt](url)
        if (text[pos] == '!' && pos + 1 < text.size() && text[pos + 1] == '[') {
            size_t bracket_close = text.find(']', pos + 2);
            if (bracket_close != std::string::npos && bracket_close + 1 < text.size() && text[bracket_close + 1] == '(') {
                size_t paren_close = text.find(')', bracket_close + 2);
                if (paren_close != std::string::npos) {
                    flush_text();
                    Token t;
                    t.type = TokenType::Image;
                    t.text = text.substr(pos + 2, bracket_close - pos - 2);
                    t.href = text.substr(bracket_close + 2, paren_close - bracket_close - 2);
                    t.raw = text.substr(pos, paren_close + 1 - pos);
                    tokens.push_back(std::move(t));
                    pos = paren_close + 1;
                    continue;
                }
            }
        }

        // Link: [text](url)
        if (text[pos] == '[') {
            // Find matching ] accounting for nesting
            int bracket_depth = 1;
            size_t i = pos + 1;
            while (i < text.size() && bracket_depth > 0) {
                if (text[i] == '\\' && i + 1 < text.size()) { i += 2; continue; }
                if (text[i] == '[') bracket_depth++;
                else if (text[i] == ']') bracket_depth--;
                if (bracket_depth > 0) i++;
            }
            if (bracket_depth == 0 && i + 1 < text.size() && text[i + 1] == '(') {
                size_t paren_close = text.find(')', i + 2);
                if (paren_close != std::string::npos) {
                    flush_text();
                    std::string link_text = text.substr(pos + 1, i - pos - 1);
                    Token t;
                    t.type = TokenType::Link;
                    t.href = text.substr(i + 2, paren_close - i - 2);
                    t.raw = text.substr(pos, paren_close + 1 - pos);
                    t.children = parse_inline(link_text);
                    tokens.push_back(std::move(t));
                    pos = paren_close + 1;
                    continue;
                }
            }
        }

        // HTML tag: <tag>...</tag> or self-closing <tag/>
        if (text[pos] == '<') {
            size_t end = text.find('>', pos + 1);
            if (end != std::string::npos) {
                std::string tag = text.substr(pos, end + 1 - pos);
                // Simple check: starts with < and ends with >
                if (tag.size() >= 3) {
                    flush_text();
                    Token t;
                    t.type = TokenType::Html;
                    t.raw = tag;
                    t.text = tag;
                    tokens.push_back(std::move(t));
                    pos = end + 1;
                    continue;
                }
            }
        }

        // Line break: two trailing spaces + newline, or explicit \n
        if (text[pos] == '\n') {
            // Check for two trailing spaces before \n
            if (pending_text.size() >= 2 &&
                pending_text[pending_text.size() - 1] == ' ' &&
                pending_text[pending_text.size() - 2] == ' ') {
                pending_text.resize(pending_text.size() - 2);
                flush_text();
                Token t;
                t.type = TokenType::Br;
                tokens.push_back(std::move(t));
            } else {
                // Regular newline within paragraph — treat as space
                if (!pending_text.empty() && pending_text.back() != ' ') {
                    pending_text += ' ';
                }
            }
            pos++;
            continue;
        }

        // Plain text
        pending_text += text[pos];
        pos++;
    }

    flush_text();
    return tokens;
}


// ---------------------------------------------------------------------------
// Block-level Lexer
// ---------------------------------------------------------------------------

// Forward declaration for recursive list parsing
static void parse_list(const std::vector<std::string>& lines, size_t& idx,
                       int base_indent, std::vector<Token>& tokens);

std::vector<Token> lex(const std::string& content) {
    auto lines = split_lines(content);
    std::vector<Token> tokens;
    size_t idx = 0;

    auto flush_paragraph = [&](std::string& para_text) {
        if (para_text.empty()) return;
        // Trim trailing whitespace/newlines
        while (!para_text.empty() && (para_text.back() == '\n' || para_text.back() == '\r' ||
               para_text.back() == ' ')) {
            para_text.pop_back();
        }
        if (para_text.empty()) return;
        Token t;
        t.type = TokenType::Paragraph;
        t.raw = para_text;
        t.children = parse_inline(para_text);
        tokens.push_back(std::move(t));
        para_text.clear();
    };

    std::string paragraph_accum;

    while (idx < lines.size()) {
        const auto& line = lines[idx];

        // Empty line
        if (trim(line).empty()) {
            flush_paragraph(paragraph_accum);
            Token t;
            t.type = TokenType::Space;
            tokens.push_back(std::move(t));
            idx++;
            continue;
        }

        // ATX Heading
        int hdepth = atx_heading_depth(line);
        if (hdepth > 0) {
            flush_paragraph(paragraph_accum);
            Token t;
            t.type = TokenType::Heading;
            t.depth = hdepth;
            std::string htext = heading_text(line);
            t.raw = line;
            t.text = htext;
            t.children = parse_inline(htext);
            tokens.push_back(std::move(t));
            idx++;
            continue;
        }

        // Fenced code block
        char fence_char = 0;
        int fence_count = 0;
        if (is_code_fence(line, fence_char, fence_count)) {
            std::string lang = code_fence_lang(line);

            // For bare fences (no language), verify a matching close exists.
            // This prevents a stray outer-close fence (from AI-generated nested
            // fences like ```markdown ... ```python ... ``` ```) from swallowing
            // all remaining content as an unclosed code block.
            if (lang.empty()) {
                bool has_close = false;
                for (size_t look = idx + 1; look < lines.size(); look++) {
                    char lc = 0; int ln = 0;
                    if (is_code_fence(lines[look], lc, ln) &&
                        lc == fence_char && ln >= fence_count) {
                        auto lt = trim(lines[look]);
                        bool lclean = true;
                        for (char c : lt) {
                            if (c != fence_char) { lclean = false; break; }
                        }
                        if (lclean) { has_close = true; break; }
                    }
                }
                if (!has_close) {
                    // No matching close — skip this bare fence line
                    idx++;
                    continue;
                }
            }

            flush_paragraph(paragraph_accum);
            std::string code_text;
            idx++;
            bool closed = false;
            while (idx < lines.size()) {
                const auto& cl = lines[idx];
                // Check for closing fence
                char close_char = 0;
                int close_count = 0;
                if (is_code_fence(cl, close_char, close_count) &&
                    close_char == fence_char && close_count >= fence_count &&
                    trim(cl.substr(ltrim(cl).find(close_char) == 0 ? 0 : 0)) == std::string(close_count, close_char)) {
                    // Verify it's a clean closing fence (no info string)
                    auto trimmed = trim(cl);
                    bool clean = true;
                    for (char c : trimmed) {
                        if (c != fence_char) { clean = false; break; }
                    }
                    if (clean && close_count >= fence_count) {
                        closed = true;
                        idx++;
                        break;
                    }
                }
                if (!code_text.empty()) code_text += '\n';
                code_text += cl;
                idx++;
            }
            Token t;
            t.type = TokenType::Code;
            t.lang = lang;
            t.text = code_text;
            tokens.push_back(std::move(t));
            continue;
        }

        // Thematic break (must check before list since --- could be list marker)
        if (is_thematic_break(line) && !is_unordered_list_item(line)) {
            flush_paragraph(paragraph_accum);
            Token t;
            t.type = TokenType::Hr;
            t.raw = line;
            tokens.push_back(std::move(t));
            idx++;
            continue;
        }

        // GFM Table: need current line as potential header + next as separator
        if (is_table_row(line) && idx + 1 < lines.size() && is_table_separator(lines[idx + 1])) {
            flush_paragraph(paragraph_accum);
            auto header_cells_str = split_table_cells(line);
            auto aligns = parse_table_alignments(lines[idx + 1]);
            idx += 2;

            Token t;
            t.type = TokenType::Table;
            t.align = aligns;

            // Parse header cells as inline tokens
            for (const auto& cell : header_cells_str) {
                t.header_cells.push_back(parse_inline(cell));
            }

            // Parse body rows
            while (idx < lines.size() && is_table_row(lines[idx])) {
                auto cells = split_table_cells(lines[idx]);
                std::vector<std::vector<Token>> row;
                for (const auto& cell : cells) {
                    row.push_back(parse_inline(cell));
                }
                // Pad/truncate to match header columns
                while (row.size() < header_cells_str.size()) {
                    row.push_back({});
                }
                if (row.size() > header_cells_str.size()) {
                    row.resize(header_cells_str.size());
                }
                t.body_rows.push_back(std::move(row));
                idx++;
            }
            tokens.push_back(std::move(t));
            continue;
        }

        // Blockquote
        if (is_blockquote(line)) {
            flush_paragraph(paragraph_accum);
            std::string bq_content;
            while (idx < lines.size() && (is_blockquote(lines[idx]) || (!trim(lines[idx]).empty() && !atx_heading_depth(lines[idx])))) {
                if (is_blockquote(lines[idx])) {
                    if (!bq_content.empty()) bq_content += '\n';
                    bq_content += strip_blockquote(lines[idx]);
                } else {
                    // Lazy continuation
                    if (!bq_content.empty()) bq_content += '\n';
                    bq_content += lines[idx];
                }
                idx++;
                // Stop if next line is empty
                if (idx < lines.size() && trim(lines[idx]).empty()) break;
            }
            Token t;
            t.type = TokenType::Blockquote;
            t.raw = bq_content;
            // Recursively lex the blockquote content
            t.children = lex(bq_content);
            tokens.push_back(std::move(t));
            continue;
        }

        // Unordered list
        if (is_unordered_list_item(line)) {
            flush_paragraph(paragraph_accum);
            int base_indent = count_indent(line);
            parse_list(lines, idx, base_indent, tokens);
            continue;
        }

        // Ordered list
        {
            int start_num = 0;
            if (is_ordered_list_item(line, start_num)) {
                flush_paragraph(paragraph_accum);
                int base_indent = count_indent(line);
                parse_list(lines, idx, base_indent, tokens);
                continue;
            }
        }

        // Default: paragraph line accumulation
        if (!paragraph_accum.empty()) paragraph_accum += '\n';
        paragraph_accum += line;
        idx++;
    }

    // Flush any remaining paragraph
    std::string remaining = std::move(paragraph_accum);
    if (!remaining.empty()) {
        while (!remaining.empty() && (remaining.back() == '\n' || remaining.back() == '\r' ||
               remaining.back() == ' ')) {
            remaining.pop_back();
        }
        if (!remaining.empty()) {
            Token t;
            t.type = TokenType::Paragraph;
            t.raw = remaining;
            t.children = parse_inline(remaining);
            tokens.push_back(std::move(t));
        }
    }

    return tokens;
}

// ---------------------------------------------------------------------------
// List parsing (handles nested lists)
// ---------------------------------------------------------------------------

static void parse_list(const std::vector<std::string>& lines, size_t& idx,
                       int base_indent, std::vector<Token>& tokens) {
    Token list_tok;
    list_tok.type = TokenType::List;

    // Determine if ordered or unordered from first item
    const auto& first_line = lines[idx];
    int dummy;
    bool is_ordered = (is_ordered_list_item(first_line, dummy) > 0);
    list_tok.ordered = is_ordered;
    if (is_ordered) {
        list_tok.start = dummy;
    }

    while (idx < lines.size()) {
        const auto& line = lines[idx];
        int cur_indent = count_indent(line);

        // Check if this line is a list item at our level
        int marker_width = 0;
        int start_num = 0;
        bool is_our_item = false;

        if (is_ordered) {
            marker_width = is_ordered_list_item(line, start_num);
            is_our_item = (marker_width > 0 && cur_indent == base_indent);
        } else {
            marker_width = is_unordered_list_item(line);
            is_our_item = (marker_width > 0 && cur_indent == base_indent);
        }

        if (is_our_item) {
            Token item;
            item.type = TokenType::ListItem;

            // Extract item text (after marker)
            auto stripped = ltrim(line);
            size_t skip = 0;
            if (is_ordered) {
                while (skip < stripped.size() && stripped[skip] >= '0' && stripped[skip] <= '9') skip++;
                skip++; // skip . or )
                if (skip < stripped.size() && stripped[skip] == ' ') skip++;
            } else {
                skip = 2; // skip "- " or "* " or "+ "
            }
            std::string item_text = stripped.substr(skip);

            // Collect continuation lines
            idx++;
            while (idx < lines.size()) {
                const auto& next_line = lines[idx];
                if (trim(next_line).empty()) {
                    // Empty line — check if next non-empty line continues the item
                    if (idx + 1 < lines.size() && count_indent(lines[idx + 1]) > base_indent) {
                        item_text += '\n';
                        idx++;
                        continue;
                    }
                    break;
                }
                int next_indent = count_indent(next_line);
                // If same indent and is a new list item, stop
                if (next_indent == base_indent) {
                    int ns = 0;
                    if (is_ordered && is_ordered_list_item(next_line, ns) > 0) break;
                    if (!is_ordered && is_unordered_list_item(next_line) > 0) break;
                }
                // If less indent, stop
                if (next_indent < base_indent) break;

                // Check for nested list
                int ns2 = 0;
                if (next_indent > base_indent &&
                    (is_unordered_list_item(next_line) > 0 || is_ordered_list_item(next_line, ns2) > 0)) {
                    // Parse nested list — add as sub-content
                    std::string nested_content;
                    while (idx < lines.size()) {
                        if (trim(lines[idx]).empty()) {
                            if (idx + 1 < lines.size() && count_indent(lines[idx + 1]) > base_indent) {
                                nested_content += '\n';
                                idx++;
                                continue;
                            }
                            break;
                        }
                        if (count_indent(lines[idx]) <= base_indent) break;
                        if (!nested_content.empty()) nested_content += '\n';
                        nested_content += lines[idx];
                        idx++;
                    }
                    // Lex nested content as sub-blocks
                    auto nested_tokens = lex(nested_content);
                    // Create a text token for the item text so far
                    if (!item_text.empty()) {
                        Token text_tok;
                        text_tok.type = TokenType::Text;
                        text_tok.text = item_text;
                        text_tok.children = parse_inline(item_text);
                        item.children.push_back(std::move(text_tok));
                        item_text.clear();
                    }
                    for (auto& nt : nested_tokens) {
                        item.children.push_back(std::move(nt));
                    }
                    continue;
                }

                // Continuation line
                item_text += '\n' + remove_indent(next_line, base_indent + 2);
                idx++;
            }

            // Finalize item
            if (!item_text.empty()) {
                Token text_tok;
                text_tok.type = TokenType::Text;
                text_tok.text = item_text;
                text_tok.children = parse_inline(item_text);
                item.children.push_back(std::move(text_tok));
            }
            list_tok.children.push_back(std::move(item));
        } else {
            // Not a list item at our level — done with this list
            break;
        }
    }

    tokens.push_back(std::move(list_tok));
}

} // namespace acecode::markdown
