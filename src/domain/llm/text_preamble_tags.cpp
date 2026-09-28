#include "text_preamble_tags.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <utility>

namespace acecode::llm {

namespace {

bool is_space_byte(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

std::string trim(std::string_view s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && is_space_byte(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && is_space_byte(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}

std::size_t utf8_sequence_length(unsigned char lead) {
    if (lead < 0x80) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 1;  // 非法前导字节按单字节走,不会越界也不会死循环
}

std::size_t count_code_points(std::string_view s) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < s.size();) {
        i += utf8_sequence_length(static_cast<unsigned char>(s[i]));
        ++count;
    }
    return count;
}

// 前缀 max 个 code point 的字节长度(不切断多字节序列)。
std::size_t byte_length_of_prefix(std::string_view s, std::size_t max_code_points) {
    std::size_t i = 0;
    std::size_t count = 0;
    while (i < s.size() && count < max_code_points) {
        const std::size_t len = utf8_sequence_length(static_cast<unsigned char>(s[i]));
        if (i + len > s.size()) break;
        i += len;
        ++count;
    }
    return i;
}

bool starts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() &&
           s.substr(s.size() - suffix.size()) == suffix;
}

char lower_ascii(char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

bool starts_with_ci(std::string_view s, std::string_view prefix) {
    if (s.size() < prefix.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (lower_ascii(s[i]) != lower_ascii(prefix[i])) return false;
    }
    return true;
}

std::size_t find_ci(std::string_view hay, std::string_view needle, std::size_t from = 0) {
    if (needle.empty()) return from <= hay.size() ? from : std::string_view::npos;
    for (std::size_t i = from; i + needle.size() <= hay.size(); ++i) {
        if (starts_with_ci(hay.substr(i), needle)) return i;
    }
    return std::string_view::npos;
}

// s 的最长后缀同时是 needle 的真前缀(大小写不敏感)时,返回那段后缀的长度;
// 没有这样的后缀返回 0。用来判断增量尾部是不是被切断的标签。
std::size_t partial_suffix_match(std::string_view s, std::string_view needle) {
    const std::size_t max_len = std::min(s.size(), needle.size() - 1);
    for (std::size_t len = max_len; len > 0; --len) {
        if (starts_with_ci(needle, s.substr(s.size() - len))) return len;
    }
    return 0;
}

std::string collapse_whitespace(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    bool pending_space = false;
    for (unsigned char c : s) {
        if (is_space_byte(c)) {
            pending_space = !out.empty();
            continue;
        }
        if (pending_space) {
            out.push_back(' ');
            pending_space = false;
        }
        out.push_back(static_cast<char>(c));
    }
    return out;
}

// 去掉一层包裹记号(引号 / 反引号 / 加粗 / 斜体),可能嵌套所以循环到不变。
std::string strip_wrappers(std::string s) {
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 10> kPairs{{
        {"**", "**"}, {"__", "__"}, {"`", "`"}, {"\"", "\""}, {"'", "'"},
        {"\xE2\x80\x9C", "\xE2\x80\x9D"},   // “ ”
        {"\xE2\x80\x98", "\xE2\x80\x99"},   // ‘ ’
        {"\xE3\x80\x8C", "\xE3\x80\x8D"},   // 「 」
        {"*", "*"}, {"_", "_"},
    }};
    for (;;) {
        std::string before = s;
        s = trim(s);
        for (const auto& [open, close] : kPairs) {
            if (s.size() > open.size() + close.size() &&
                starts_with(s, open) && ends_with(s, close)) {
                s = trim(s.substr(open.size(), s.size() - open.size() - close.size()));
                break;
            }
        }
        if (s == before) return s;
    }
}

std::string strip_leading_markers(std::string s) {
    for (;;) {
        std::string before = s;
        s = trim(s);
        // 标题井号
        std::size_t hashes = 0;
        while (hashes < s.size() && s[hashes] == '#') ++hashes;
        if (hashes > 0 && hashes < s.size() && s[hashes] == ' ') {
            s = trim(s.substr(hashes));
        }
        // 列表记号 "- " / "* " / "• " / "1. " / "1) "
        if (starts_with(s, "- ") || starts_with(s, "* ") || starts_with(s, "+ ")) {
            s = trim(s.substr(2));
        } else if (starts_with(s, "\xE2\x80\xA2 ")) {
            s = trim(s.substr(4));
        } else {
            std::size_t digits = 0;
            while (digits < s.size() && std::isdigit(static_cast<unsigned char>(s[digits]))) ++digits;
            if (digits > 0 && digits < 3 && digits + 1 < s.size() &&
                (s[digits] == '.' || s[digits] == ')') && s[digits + 1] == ' ') {
                s = trim(s.substr(digits + 2));
            }
        }
        if (s == before) return s;
    }
}

std::string strip_trailing_punctuation(std::string s) {
    static constexpr std::array<std::string_view, 8> kSuffixes{
        "...", "\xE2\x80\xA6" /* … */, ":", "\xEF\xBC\x9A" /* ： */, ".",
        "\xE3\x80\x82" /* 。 */, ",", "\xEF\xBC\x8C" /* ， */,
    };
    for (;;) {
        std::string before = s;
        s = trim(s);
        for (const auto& suffix : kSuffixes) {
            if (s.size() > suffix.size() && ends_with(s, suffix)) {
                s = trim(s.substr(0, s.size() - suffix.size()));
                break;
            }
        }
        if (s == before) return s;
    }
}

// "<text_preamble" —— 后面必须跟空白、'>' 或 '/' 才算我们的标签。
constexpr std::string_view kOpenPrefix = "<text_preamble";
constexpr std::string_view kCloseTag = "</text_preamble>";

// 从开标签的属性串里取 type 的值:type="read" / type='write' / type=read,
// 大小写不敏感;不是 read / write 的一律当没写。
std::string parse_kind_attribute(std::string_view attrs) {
    const std::size_t key = find_ci(attrs, "type");
    if (key == std::string_view::npos) return {};
    std::size_t i = key + 4;
    while (i < attrs.size() && is_space_byte(static_cast<unsigned char>(attrs[i]))) ++i;
    if (i >= attrs.size() || attrs[i] != '=') return {};
    ++i;
    while (i < attrs.size() && is_space_byte(static_cast<unsigned char>(attrs[i]))) ++i;
    if (i >= attrs.size()) return {};
    std::string value;
    if (attrs[i] == '"' || attrs[i] == '\'') {
        const char quote = attrs[i];
        const std::size_t end = attrs.find(quote, i + 1);
        value = std::string(attrs.substr(i + 1, end == std::string_view::npos
                                                        ? std::string_view::npos
                                                        : end - i - 1));
    } else {
        std::size_t end = i;
        while (end < attrs.size() &&
               !is_space_byte(static_cast<unsigned char>(attrs[end])) && attrs[end] != '/') {
            ++end;
        }
        value = std::string(attrs.substr(i, end - i));
    }
    value = trim(value);
    for (auto& c : value) c = lower_ascii(c);
    if (value == kKindRead || value == kKindWrite) return value;
    return {};
}

}  // namespace

std::string truncate_code_points(const std::string& text, std::size_t max_code_points) {
    if (count_code_points(text) <= max_code_points) return text;
    return text.substr(0, byte_length_of_prefix(text, max_code_points)) + "\xE2\x80\xA6";
}

std::string normalize_title_line(const std::string& text, std::size_t max_code_points) {
    std::string s = collapse_whitespace(text);
    s = strip_leading_markers(s);
    s = strip_wrappers(s);
    s = strip_trailing_punctuation(s);
    s = strip_wrappers(s);
    s = trim(s);
    if (s.empty()) return {};
    return truncate_code_points(s, max_code_points);
}

// ---- TextPreambleScanner ----

void TextPreambleScanner::reset() {
    pending_.clear();
    in_tag_ = false;
    kind_.clear();
    swallow_leading_ws_ = true;
}

TextPreambleScanner::Output TextPreambleScanner::feed(std::string_view delta) {
    Output out;
    pending_.append(delta.data(), delta.size());
    drain(out, /*at_end=*/false);
    return out;
}

TextPreambleScanner::Output TextPreambleScanner::flush() {
    Output out;
    drain(out, /*at_end=*/true);
    // drain(at_end) 已把一切结清;这里只是兜底。
    if (!pending_.empty()) {
        if (in_tag_) close_tag(out, pending_);
        else emit_visible(out, pending_);
        pending_.clear();
    }
    in_tag_ = false;
    return out;
}

void TextPreambleScanner::emit_visible(Output& out, std::string_view text) {
    if (text.empty()) return;
    if (swallow_leading_ws_) {
        std::size_t i = 0;
        while (i < text.size() && is_space_byte(static_cast<unsigned char>(text[i]))) ++i;
        if (i == text.size()) return;
        swallow_leading_ws_ = false;
        text = text.substr(i);
    }
    out.visible.append(text.data(), text.size());
}

void TextPreambleScanner::close_tag(Output& out, std::string_view body) {
    const std::string title =
        normalize_title_line(std::string(body), kTextPreambleMaxCodePoints);
    if (!title.empty()) out.preambles.push_back({title, kind_});
    kind_.clear();
    // 闭合标签后面紧跟的 "\n\n" 不进正文:那只是模型给标签留的空行。
    swallow_leading_ws_ = true;
}

void TextPreambleScanner::drain(Output& out, bool at_end) {
    for (;;) {
        if (!in_tag_) {
            std::string_view view(pending_);
            // 正文里(标签外)出现的孤立闭合标签 —— 上一段正文按换行提前闭合时
            // 模型补写的 `</text_preamble>` —— 直接丢掉,不当正文。
            {
                std::size_t i = 0;
                while (i < view.size() && is_space_byte(static_cast<unsigned char>(view[i]))) ++i;
                const std::string_view tail = view.substr(i);
                if (starts_with_ci(tail, kCloseTag)) {
                    emit_visible(out, view.substr(0, i));
                    pending_.erase(0, i + kCloseTag.size());
                    swallow_leading_ws_ = true;
                    continue;
                }
                if (!at_end && !tail.empty() && tail[0] == '<' &&
                    partial_suffix_match(tail, kCloseTag) == tail.size()) {
                    // 尾部可能是被切断的闭合标签,等下一段再判。
                    emit_visible(out, view.substr(0, i));
                    pending_.erase(0, i);
                    return;
                }
            }
            const std::size_t pos = find_ci(view, kOpenPrefix);
            if (pos == std::string_view::npos) {
                const std::size_t keep = at_end ? 0 : partial_suffix_match(view, kOpenPrefix);
                emit_visible(out, view.substr(0, view.size() - keep));
                pending_.erase(0, view.size() - keep);
                return;
            }
            const std::size_t after = pos + kOpenPrefix.size();
            if (after >= view.size()) {
                // "<text_preamble" 刚好在增量尾部:还不知道后面是不是标签的一部分。
                if (at_end) {
                    emit_visible(out, view);
                    pending_.clear();
                    return;
                }
                emit_visible(out, view.substr(0, pos));
                pending_.erase(0, pos);
                return;
            }
            const char next = view[after];
            if (!(is_space_byte(static_cast<unsigned char>(next)) || next == '>' || next == '/')) {
                // "<text_preambleX…":不是我们的标签,连同 '<' 一起放行再往后扫。
                emit_visible(out, view.substr(0, after));
                pending_.erase(0, after);
                continue;
            }
            const std::size_t gt = view.find('>', after);
            if (gt == std::string_view::npos) {
                if (at_end) {
                    emit_visible(out, view);
                    pending_.clear();
                    return;
                }
                emit_visible(out, view.substr(0, pos));
                pending_.erase(0, pos);
                return;
            }
            const std::string_view attrs = view.substr(after, gt - after);
            const bool self_closing = !attrs.empty() && attrs.back() == '/';
            kind_ = parse_kind_attribute(attrs);
            emit_visible(out, view.substr(0, pos));
            pending_.erase(0, gt + 1);
            if (self_closing) {
                kind_.clear();
                swallow_leading_ws_ = true;
                continue;
            }
            in_tag_ = true;
            continue;
        }

        // 标签正文:闭合标签一到就结清;宽松规则见头文件。
        const std::string_view body(pending_);
        const std::size_t close = find_ci(body, kCloseTag);
        if (close != std::string_view::npos) {
            close_tag(out, body.substr(0, close));
            pending_.erase(0, close + kCloseTag.size());
            in_tag_ = false;
            continue;
        }
        std::size_t first_visible = 0;
        while (first_visible < body.size() &&
               is_space_byte(static_cast<unsigned char>(body[first_visible]))) {
            ++first_visible;
        }
        const std::size_t nl = body.find('\n', first_visible);
        if (nl != std::string_view::npos) {
            close_tag(out, body.substr(0, nl));
            pending_.erase(0, nl + 1);
            in_tag_ = false;
            continue;
        }
        if (body.size() > kTextPreambleMaxBodyBytes) {
            const std::size_t cut = byte_length_of_prefix(body, count_code_points(
                body.substr(0, kTextPreambleMaxBodyBytes)));
            close_tag(out, body.substr(0, cut));
            pending_.erase(0, cut);
            in_tag_ = false;
            continue;
        }
        if (at_end) {
            close_tag(out, body);
            pending_.clear();
            in_tag_ = false;
            return;
        }
        return;  // 正文还没流完
    }
}

std::string strip_text_preamble_tags(const std::string& text) {
    // 没有标签的正文逐字节原样返回(含前导空白):扫描器的「吞前导空白」只针对
    // 带标签的流,普通正文不能被误伤。
    if (find_ci(text, "text_preamble") == std::string_view::npos) return text;
    TextPreambleScanner scanner;
    auto first = scanner.feed(text);
    auto rest = scanner.flush();
    return first.visible + rest.visible;
}

}  // namespace acecode::llm
