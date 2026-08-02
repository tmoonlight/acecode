#include "session_title_generator.hpp"

#include "../utils/encoding.hpp"
#include "../utils/terminal_title.hpp"

#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>
#include <optional>
#include <utility>
#include <vector>

namespace acecode {
namespace {

constexpr std::size_t kMaxGeneratedTitleBytes = 120;

std::string trim_ascii(std::string s) {
    auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && is_space(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && is_space(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

std::string collapse_whitespace(std::string s) {
    std::string out;
    out.reserve(s.size());
    bool in_space = false;
    for (unsigned char c : s) {
        if (std::isspace(c)) {
            if (!out.empty() && !in_space) out.push_back(' ');
            in_space = true;
        } else {
            out.push_back(static_cast<char>(c));
            in_space = false;
        }
    }
    if (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::string strip_wrapping_quotes(std::string s) {
    s = trim_ascii(std::move(s));
    while (s.size() >= 2 &&
           ((s.front() == '"' && s.back() == '"') ||
            (s.front() == '\'' && s.back() == '\''))) {
        s = trim_ascii(s.substr(1, s.size() - 2));
    }
    return s;
}

std::optional<std::string> title_from_json(const std::string& raw) {
    try {
        auto j = nlohmann::json::parse(raw);
        if (j.is_object()) {
            for (const auto* key : {"title", "name", "summary"}) {
                if (j.contains(key) && j[key].is_string()) {
                    return j[key].get<std::string>();
                }
            }
        }
        if (j.is_string()) return j.get<std::string>();
    } catch (...) {
    }
    return std::nullopt;
}

std::optional<std::string> unwrap_markdown_json_fence(const std::string& raw) {
    std::string text = trim_ascii(raw);
    if (text.rfind("```", 0) != 0) return std::nullopt;

    const std::size_t closing = text.rfind("```");
    if (closing <= 3 || !trim_ascii(text.substr(closing + 3)).empty()) {
        return std::nullopt;
    }

    std::string body = trim_ascii(text.substr(3, closing - 3));
    if (body.empty()) return std::nullopt;

    if (body.size() >= 4) {
        std::string language = body.substr(0, 4);
        std::transform(language.begin(), language.end(), language.begin(),
                       [](unsigned char c) {
                           return static_cast<char>(std::tolower(c));
                       });
        if (language == "json" &&
            (body.size() == 4 ||
             std::isspace(static_cast<unsigned char>(body[4])) != 0)) {
            body = trim_ascii(body.substr(4));
        }
    }

    if (body.empty() || (body.front() != '{' && body.front() != '"')) {
        return std::nullopt;
    }
    return body;
}

bool contains_line_break(const std::string& text) {
    return text.find('\n') != std::string::npos ||
           text.find('\r') != std::string::npos;
}

bool contains_sentence_ending(const std::string& text) {
    if (text.find("\xE3\x80\x82") != std::string::npos || // CJK full stop
        text.find("\xEF\xBC\x81") != std::string::npos || // full-width exclamation
        text.find("\xEF\xBC\x9F") != std::string::npos) { // full-width question mark
        return true;
    }
    if (text.empty()) return false;
    const char last = text.back();
    return last == '.' || last == '!' || last == '?';
}

std::size_t whitespace_word_count(const std::string& text) {
    std::size_t count = 0;
    bool in_word = false;
    for (unsigned char c : text) {
        if (std::isspace(c) != 0) {
            in_word = false;
        } else if (!in_word) {
            ++count;
            in_word = true;
        }
    }
    return count;
}

bool looks_like_generated_title(const std::string& title) {
    if (title.empty() || title.size() > kMaxGeneratedTitleBytes) return false;
    if (title.find("```") != std::string::npos ||
        title.find("**") != std::string::npos ||
        contains_line_break(title) ||
        contains_sentence_ending(title)) {
        return false;
    }
    return whitespace_word_count(title) <= 12;
}

std::string strip_common_prefix(std::string s) {
    const std::string lower = [&] {
        std::string v = s;
        std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return v;
    }();
    for (const auto* prefix : {"title:", "session title:", "标题:"}) {
        const std::string p(prefix);
        if (lower.rfind(p, 0) == 0) {
            return trim_ascii(s.substr(p.size()));
        }
    }
    return s;
}

} // namespace

bool is_generated_session_error_title(const std::string& title) {
    std::size_t i = 0;
    while (i < title.size() &&
           std::isspace(static_cast<unsigned char>(title[i])) != 0) {
        ++i;
    }
    return title.compare(i, 7, "[Error]") == 0;
}

std::string sanitize_generated_session_title(std::string raw) {
    std::string normalized = trim_ascii(ensure_utf8(raw));
    if (normalized.empty() || is_generated_session_error_title(normalized)) return {};

    bool fenced = false;
    if (normalized.rfind("```", 0) == 0) {
        auto payload = unwrap_markdown_json_fence(normalized);
        if (!payload.has_value()) return {};
        normalized = std::move(*payload);
        fenced = true;
    } else if (normalized.find("```") != std::string::npos) {
        return {};
    }

    auto json_title = title_from_json(normalized);
    if (!json_title.has_value() &&
        (fenced || normalized.front() == '{' || normalized.front() == '[')) {
        return {};
    }

    std::string title = json_title.has_value() ? std::move(*json_title) : normalized;
    if (contains_line_break(title)) return {};
    title = strip_wrapping_quotes(strip_common_prefix(collapse_whitespace(title)));
    if (!looks_like_generated_title(title) ||
        is_generated_session_error_title(title)) {
        return {};
    }
    std::string err;
    if (!sanitize_title(title, err)) return {};
    return trim_ascii(title);
}

std::optional<std::string> generate_session_title(
    LlmProvider& provider,
    const std::string& first_user_text,
    int max_input_bytes) {
    const int bounded_input = std::max(1, max_input_bytes);
    const std::string input = truncate_utf8_prefix(
        first_user_text,
        static_cast<std::size_t>(bounded_input),
        "");
    if (trim_ascii(input).empty()) return std::nullopt;

    ChatMessage system;
    system.role = "system";
    system.content =
        "Generate a concise title for this coding-agent session. "
        "Return only the title text, without JSON, Markdown, code fences, "
        "quotes, prefixes, or explanation. "
        "Use at most 8 English words or 24 Chinese characters. "
        "Do not include punctuation unless needed for a file or symbol name.";

    ChatMessage user;
    user.role = "user";
    user.content = input;

    ChatResponse response = provider.chat({system, user}, {});
    if (response.finish_reason == "error" ||
        response.has_tool_calls() ||
        is_generated_session_error_title(response.content)) {
        return std::nullopt;
    }
    std::string title = sanitize_generated_session_title(response.content);
    if (title.empty() || is_generated_session_error_title(title)) {
        return std::nullopt;
    }
    return title;
}

} // namespace acecode
