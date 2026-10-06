#include "markdown.hpp"

#include <cctype>
#include <vector>

namespace acecode::im {
namespace {

enum class BlockKind { Text, Blank, Code, Heading, Quote, Bullet, Ordered, Table, Rule };

struct Block {
    BlockKind kind = BlockKind::Text;
    std::string text;               // 标题/列表/段落行的正文;代码块内容
    std::string lang;               // 代码块语言
    std::string marker;             // 有序列表编号或列表缩进
    std::vector<std::string> lines; // 引用与表格的多行
};

bool is_alnum(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; }

std::string trim(std::string_view text) {
    std::size_t b = 0, e = text.size();
    while (b < e && (text[b] == ' ' || text[b] == '\t')) ++b;
    while (e > b && (text[e - 1] == ' ' || text[e - 1] == '\t' || text[e - 1] == '\r')) --e;
    return std::string(text.substr(b, e - b));
}

std::size_t leading_spaces(std::string_view line) {
    std::size_t i = 0;
    while (i < line.size() && line[i] == ' ') ++i;
    return i;
}

bool fence_open(std::string_view line, std::string* lang) {
    const auto indent = leading_spaces(line);
    if (indent > 3 || line.substr(indent, 3) != "```") return false;
    if (lang) {
        *lang = trim(line.substr(indent + 3));
        const auto space = lang->find(' ');
        if (space != std::string::npos) lang->resize(space);
    }
    return true;
}

bool is_rule(std::string_view line) {
    const auto text = trim(line);
    if (text.size() < 3) return false;
    const char c = text[0];
    if (c != '-' && c != '*' && c != '_') return false;
    std::size_t count = 0;
    for (const char ch : text) {
        if (ch == c) ++count;
        else if (ch != ' ') return false;
    }
    return count >= 3;
}

std::vector<std::string> split_lines(std::string_view text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto end = text.find('\n', start);
        std::string line(text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return lines;
}

std::vector<Block> parse_blocks(std::string_view markdown) {
    std::vector<Block> blocks;
    const auto lines = split_lines(markdown);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        std::string lang;
        if (fence_open(line, &lang)) {
            Block block{BlockKind::Code};
            block.lang = lang;
            std::string content;
            std::size_t j = i + 1;
            for (; j < lines.size() && !fence_open(lines[j], nullptr); ++j) {
                if (!content.empty() || j > i + 1) content += '\n';
                content += lines[j];
            }
            block.text = content;
            blocks.push_back(std::move(block));
            i = j;  // 跳过闭合围栏;未闭合时吃到文末
            continue;
        }
        const auto text = trim(line);
        if (text.empty()) {
            blocks.push_back(Block{BlockKind::Blank});
            continue;
        }
        if (is_rule(line)) {
            blocks.push_back(Block{BlockKind::Rule});
            continue;
        }
        std::size_t hashes = 0;
        while (hashes < text.size() && hashes < 7 && text[hashes] == '#') ++hashes;
        if (hashes >= 1 && hashes <= 6 && hashes < text.size() && text[hashes] == ' ') {
            Block block{BlockKind::Heading};
            block.text = trim(std::string_view(text).substr(hashes + 1));
            blocks.push_back(std::move(block));
            continue;
        }
        if (text[0] == '>') {
            Block block{BlockKind::Quote};
            for (; i < lines.size(); ++i) {
                const auto quoted = trim(lines[i]);
                if (quoted.empty() || quoted[0] != '>') break;
                std::string body = quoted.substr(1);
                if (!body.empty() && body[0] == ' ') body.erase(0, 1);
                block.lines.push_back(body);
            }
            --i;
            blocks.push_back(std::move(block));
            continue;
        }
        if (text[0] == '|') {
            Block block{BlockKind::Table};
            for (; i < lines.size(); ++i) {
                const auto row = trim(lines[i]);
                if (row.empty() || row[0] != '|') break;
                block.lines.push_back(row);
            }
            --i;
            blocks.push_back(std::move(block));
            continue;
        }
        const auto indent = leading_spaces(line);
        if ((text[0] == '-' || text[0] == '*' || text[0] == '+') && text.size() > 1 && text[1] == ' ') {
            Block block{BlockKind::Bullet};
            block.marker = std::string((indent / 2) * 2, ' ');
            block.text = trim(std::string_view(text).substr(2));
            blocks.push_back(std::move(block));
            continue;
        }
        std::size_t digits = 0;
        while (digits < text.size() && digits < 9 && std::isdigit(static_cast<unsigned char>(text[digits]))) ++digits;
        if (digits > 0 && digits + 1 < text.size() && (text[digits] == '.' || text[digits] == ')') &&
            text[digits + 1] == ' ') {
            Block block{BlockKind::Ordered};
            block.marker = std::string((indent / 2) * 2, ' ') + text.substr(0, digits) + ".";
            block.text = trim(std::string_view(text).substr(digits + 2));
            blocks.push_back(std::move(block));
            continue;
        }
        Block block{BlockKind::Text};
        block.text = std::string(line);
        blocks.push_back(std::move(block));
    }
    return blocks;
}

bool safe_link(std::string_view url) {
    for (const char* prefix : {"http://", "https://", "mailto:", "tg://"}) {
        if (url.rfind(prefix, 0) == 0) return url.find_first_of(" \n\r\t") == std::string_view::npos;
    }
    return false;
}

// 行内渲染。html=false 时输出去标记的纯文本。depth 防止病态嵌套导致深递归。
std::string render_inline(std::string_view s, bool html, int depth);

// 在 s[from..] 中找与开头标记相同的闭合标记;要求内容非空且两端不是空格。
std::size_t find_closing(std::string_view s, std::size_t from, std::string_view marker) {
    std::size_t pos = from;
    while (true) {
        pos = s.find(marker, pos);
        if (pos == std::string_view::npos) return pos;
        if (pos > from && s[pos - 1] != ' ' && s[pos - 1] != '\\') return pos;
        pos += 1;
    }
}

std::string render_inline(std::string_view s, bool html, int depth) {
    std::string out;
    std::size_t i = 0;
    auto emit_text = [&](char c) {
        if (!html) {
            out.push_back(c);
            return;
        }
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else out.push_back(c);
    };
    while (i < s.size()) {
        const char c = s[i];
        // 反斜杠转义:\* \_ \` 等按字面输出
        if (c == '\\' && i + 1 < s.size() && std::string_view("*_`~[]()#>|\\").find(s[i + 1]) != std::string_view::npos) {
            emit_text(s[i + 1]);
            i += 2;
            continue;
        }
        if (c == '`') {
            std::size_t ticks = 0;
            while (i + ticks < s.size() && s[i + ticks] == '`') ++ticks;
            const std::string marker(ticks, '`');
            const auto close = s.find(marker, i + ticks);
            if (close != std::string_view::npos) {
                std::string code(s.substr(i + ticks, close - i - ticks));
                if (code.size() >= 2 && code.front() == ' ' && code.back() == ' ') code = code.substr(1, code.size() - 2);
                if (html) out += "<code>" + html_escape(code) + "</code>";
                else out += code;
                i = close + ticks;
                continue;
            }
            for (std::size_t k = 0; k < ticks; ++k) emit_text('`');
            i += ticks;
            continue;
        }
        if (depth < 8 && c == '[') {
            const auto close_text = s.find(']', i + 1);
            if (close_text != std::string_view::npos && close_text + 1 < s.size() && s[close_text + 1] == '(') {
                const auto close_url = s.find(')', close_text + 2);
                if (close_url != std::string_view::npos) {
                    const auto label = s.substr(i + 1, close_text - i - 1);
                    const auto url = s.substr(close_text + 2, close_url - close_text - 2);
                    if (!label.empty() && safe_link(url)) {
                        if (html) {
                            out += "<a href=\"" + html_escape(url, true) + "\">" +
                                   render_inline(label, html, depth + 1) + "</a>";
                        } else {
                            const auto plain = render_inline(label, html, depth + 1);
                            out += plain == url ? plain : plain + " (" + std::string(url) + ")";
                        }
                        i = close_url + 1;
                        continue;
                    }
                }
            }
        }
        if (depth < 8) {
            struct Style { std::string_view marker; const char* tag; bool word_bound; };
            static constexpr Style styles[] = {
                {"**", "b", false}, {"__", "b", true}, {"~~", "s", false},
                {"*", "i", false}, {"_", "i", true},
            };
            bool matched = false;
            for (const auto& style : styles) {
                if (s.substr(i, style.marker.size()) != style.marker) continue;
                const auto start = i + style.marker.size();
                if (start >= s.size() || s[start] == ' ') continue;
                if (style.word_bound && i > 0 && is_alnum(s[i - 1])) continue;
                // 单字符标记不能与双字符标记混淆(例如 ** 的第一个 *)
                if (style.marker.size() == 1 && start < s.size() && s[start] == style.marker[0]) continue;
                const auto close = find_closing(s, start, style.marker);
                if (close == std::string_view::npos) continue;
                const auto after = close + style.marker.size();
                if (style.word_bound && after < s.size() && is_alnum(s[after])) continue;
                const auto inner = render_inline(s.substr(start, close - start), html, depth + 1);
                if (html) out += std::string("<") + style.tag + ">" + inner + "</" + style.tag + ">";
                else out += inner;
                i = after;
                matched = true;
                break;
            }
            if (matched) continue;
        }
        emit_text(c);
        ++i;
    }
    return out;
}

std::string render(std::string_view markdown, bool html) {
    std::string out;
    bool first = true;
    auto line = [&](const std::string& text) {
        if (!first) out += '\n';
        out += text;
        first = false;
    };
    for (const auto& block : parse_blocks(markdown)) {
        switch (block.kind) {
            case BlockKind::Blank: line(""); break;
            case BlockKind::Rule: line("——————"); break;
            case BlockKind::Text: line(render_inline(block.text, html, 0)); break;
            case BlockKind::Heading: {
                const auto inner = render_inline(block.text, html, 0);
                line(html ? "<b>" + inner + "</b>" : inner);
                break;
            }
            case BlockKind::Bullet:
                line(block.marker + "• " + render_inline(block.text, html, 0));
                break;
            case BlockKind::Ordered:
                line(block.marker + " " + render_inline(block.text, html, 0));
                break;
            case BlockKind::Code:
                if (html) {
                    const std::string open = block.lang.empty()
                        ? "<pre><code>"
                        : "<pre><code class=\"language-" + html_escape(block.lang, true) + "\">";
                    line(open + html_escape(block.text) + "</code></pre>");
                } else {
                    line(block.text);
                }
                break;
            case BlockKind::Quote: {
                std::string body;
                for (std::size_t k = 0; k < block.lines.size(); ++k) {
                    if (k) body += '\n';
                    body += render_inline(block.lines[k], html, 0);
                }
                line(html ? "<blockquote>" + body + "</blockquote>" : body);
                break;
            }
            case BlockKind::Table: {
                std::string body;
                for (std::size_t k = 0; k < block.lines.size(); ++k) {
                    if (k) body += '\n';
                    body += block.lines[k];
                }
                line(html ? "<pre>" + html_escape(body) + "</pre>" : body);
                break;
            }
        }
    }
    return out;
}

} // namespace

std::string html_escape(std::string_view text, bool attribute) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (attribute && c == '"') out += "&quot;";
        else out.push_back(c);
    }
    return out;
}

std::string markdown_to_plain(std::string_view markdown) { return render(markdown, false); }

std::string markdown_to_telegram_html(std::string_view markdown) { return render(markdown, true); }

} // namespace acecode::im
