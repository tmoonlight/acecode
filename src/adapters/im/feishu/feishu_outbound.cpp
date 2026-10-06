#include "im/feishu/feishu_protocol.hpp"

#include "im/feishu/feishu_text_util.hpp"
#include "im/text_chunk.hpp"

#include <algorithm>

namespace acecode::im::feishu {

using namespace detail;

namespace {

// ---------------------------------------------------------------- Markdown 判定

bool line_is_heading(std::string_view line) {
    std::size_t i = 0;
    while (i < line.size() && line[i] == '#') ++i;
    return i >= 1 && i <= 6 && i < line.size() && (line[i] == ' ' || line[i] == '\t');
}

std::string_view ltrim_view(std::string_view line) {
    std::size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    return line.substr(i);
}

bool line_is_list(std::string_view line) {
    const auto rest = ltrim_view(line);
    if (rest.size() >= 2 && (rest[0] == '-' || rest[0] == '*') && (rest[1] == ' ' || rest[1] == '\t')) return true;
    std::size_t i = 0;
    while (i < rest.size() && is_digit(rest[i])) ++i;
    return i >= 1 && i + 1 < rest.size() && rest[i] == '.' && (rest[i + 1] == ' ' || rest[i + 1] == '\t');
}

bool line_is_rule(std::string_view line) {
    const auto text = trim(line);
    return text.size() >= 3 && text.find_first_not_of('-') == std::string::npos;
}

bool line_is_quote(std::string_view line) {
    return !line.empty() && line[0] == '>' && (line.size() == 1 || line[1] == ' ' || line[1] == '\t');
}

bool line_is_table_separator(std::string_view line) {
    if (line.size() < 2 || line[0] != '|') return false;
    bool dash = false;
    for (const char c : line) {
        if (c == '-') dash = true;
        else if (c != ':' && c != '|' && c != ' ' && c != '\t') return false;
    }
    return dash && trim(line).back() == '|';
}

// 同一行内成对出现的标记(如 **x**):开标记后的第一个字符不能还是标记字符,
// 两个标记之间至少 min_inner 个字符。
bool paired_inline(std::string_view line, std::string_view marker, std::size_t min_inner) {
    std::size_t pos = 0;
    while ((pos = line.find(marker, pos)) != std::string_view::npos) {
        const auto inner = pos + marker.size();
        if (inner < line.size() && line[inner] != marker[0] &&
            line.find(marker, inner + min_inner) != std::string_view::npos)
            return true;
        pos = inner;
    }
    return false;
}

bool has_inline_markdown(std::string_view line) {
    // `code`
    for (std::size_t pos = line.find('`'); pos != std::string_view::npos; pos = line.find('`', pos + 1)) {
        const auto end = line.find('`', pos + 1);
        if (end != std::string_view::npos && end > pos + 1) return true;
    }
    if (paired_inline(line, "**", 2)) return true;   // **bold**(内部至少 2 个字符)
    if (paired_inline(line, "~~", 2)) return true;   // ~~strike~~
    {
        const auto open = line.find("<u>");
        if (open != std::string_view::npos && line.find("</u>", open + 4) != std::string_view::npos) return true;
    }
    // *italic*:单星号之间至少一个非星号字符
    for (std::size_t pos = line.find('*'); pos != std::string_view::npos; pos = line.find('*', pos + 1)) {
        std::size_t k = pos + 1;
        while (k < line.size() && line[k] != '*') ++k;
        if (k < line.size() && k > pos + 1) return true;
    }
    // [text](url)
    for (std::size_t pos = line.find('['); pos != std::string_view::npos; pos = line.find('[', pos + 1)) {
        const auto close = line.find("](", pos + 1);
        if (close == std::string_view::npos || close == pos + 1) continue;
        if (line.substr(pos + 1, close - pos - 1).find(']') != std::string_view::npos) continue;
        const auto end = line.find(')', close + 2);
        if (end != std::string_view::npos && end > close + 2) return true;
    }
    return false;
}

std::vector<std::string_view> split_lines_view(std::string_view text) {
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto end = text.find('\n', start);
        auto line = text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        lines.push_back(line);
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return lines;
}

nlohmann::json md_row(const std::string& text) {
    return nlohmann::json::array({nlohmann::json{{"tag", "md"}, {"text", text}}});
}

std::size_t content_bytes(const std::string& chunk, bool markdown) {
    return dump_json(markdown ? build_post_content(chunk) : build_text_content(chunk)).size();
}

void split_into(const std::string& text, bool markdown, std::size_t max_chars, std::size_t max_bytes,
                std::vector<std::string>& out) {
    for (auto& chunk : chunk_text(text, max_chars, false)) {
        if (max_chars > 64 && content_bytes(chunk, markdown) > max_bytes) {
            split_into(chunk, markdown, max_chars / 2, max_bytes, out);
        } else {
            out.push_back(std::move(chunk));
        }
    }
}

} // namespace

bool looks_like_markdown(std::string_view text) {
    if (text.find("```") != std::string_view::npos) return true;
    const auto lines = split_lines_view(text);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const auto line = lines[i];
        if (line_is_heading(line) || line_is_list(line) || line_is_rule(line) || line_is_quote(line)) return true;
        if (line.size() >= 2 && line[0] == '|' && trim(line).back() == '|' && i + 1 < lines.size() &&
            line_is_table_separator(lines[i + 1]))
            return true;
        if (has_inline_markdown(line)) return true;
    }
    return false;
}

nlohmann::json build_post_content(const std::string& markdown) {
    nlohmann::json rows = nlohmann::json::array();
    if (markdown.find("```") == std::string::npos) {
        rows.push_back(md_row(markdown));
    } else {
        std::vector<std::string> current;
        bool in_code = false;
        const auto flush = [&current, &rows] {
            if (current.empty()) return;
            std::string segment;
            for (std::size_t i = 0; i < current.size(); ++i) {
                if (i) segment.push_back('\n');
                segment += current[i];
            }
            if (!trim(segment).empty()) rows.push_back(md_row(segment));
            current.clear();
        };
        for (const auto line : split_lines_view(markdown)) {
            const auto stripped = trim(line);
            const bool fence = in_code ? stripped == "```"
                                       : starts_with(stripped, "```") &&
                                             stripped.find('`', 3) == std::string::npos;
            if (fence) {
                if (!in_code) flush();
                current.emplace_back(line);
                in_code = !in_code;
                if (!in_code) flush();
                continue;
            }
            current.emplace_back(line);
        }
        flush();
        if (rows.empty()) rows.push_back(md_row(markdown));
    }
    return {{"zh_cn", {{"content", rows}}}};
}

nlohmann::json build_text_content(const std::string& text) { return {{"text", text}}; }

std::vector<std::string> split_outbound(const std::string& text, bool markdown, std::size_t max_chars,
                                        std::size_t max_bytes) {
    std::vector<std::string> out;
    split_into(text, markdown, max_chars, max_bytes, out);
    return out;
}

} // namespace acecode::im::feishu
