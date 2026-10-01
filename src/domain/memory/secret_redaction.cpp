#include "secret_redaction.hpp"

#include <array>
#include <cstddef>

namespace acecode {

namespace {

constexpr std::string_view kMarker = "[REDACTED]";
// 全角冒号「:」(U+FF1A)的 UTF-8 字节。
constexpr std::string_view kFullWidthColon = "\xEF\xBC\x9A";
// 「密码」的 UTF-8 字节。
constexpr std::string_view kChinesePassword = "\xE5\xAF\x86\xE7\xA0\x81";

bool is_ascii_alpha(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool is_ascii_digit(unsigned char c) { return c >= '0' && c <= '9'; }

bool is_ascii_alnum(unsigned char c) { return is_ascii_alpha(c) || is_ascii_digit(c); }

bool is_token_char(unsigned char c) { return is_ascii_alnum(c) || c == '_' || c == '-'; }

unsigned char lower(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c - 'A' + 'a') : c;
}

bool starts_with_icase(std::string_view s, std::size_t i, std::string_view word) {
    if (s.size() - i < word.size()) return false;
    for (std::size_t k = 0; k < word.size(); ++k) {
        if (lower(static_cast<unsigned char>(s[i + k])) !=
            static_cast<unsigned char>(word[k])) {
            return false;
        }
    }
    return true;
}

bool starts_with(std::string_view s, std::size_t i, std::string_view word) {
    return s.size() - i >= word.size() && s.compare(i, word.size(), word) == 0;
}

struct Match {
    std::size_t consumed = 0;   // 从 i 起被替换的字节数
    std::string replacement;    // 替换文本
    bool counted = true;        // 已是 [REDACTED] 的值不再计数
};

// PEM 私钥块:-----BEGIN <label>PRIVATE KEY----- 到对应 END 行,整块替换;
// 缺少 END 行时替换到文本末尾(截断的私钥同样不能落盘)。
bool match_pem_private_key(std::string_view s, std::size_t i, Match& m) {
    constexpr std::string_view kBegin = "-----BEGIN ";
    if (!starts_with(s, i, kBegin)) return false;
    const std::size_t label_start = i + kBegin.size();
    const std::size_t label_end = s.find("-----", label_start);
    if (label_end == std::string_view::npos) return false;
    const std::string_view label = s.substr(label_start, label_end - label_start);
    if (label.find('\n') != std::string_view::npos) return false;
    if (label.find("PRIVATE KEY") == std::string_view::npos) return false;
    std::string end_marker = "-----END ";
    end_marker.append(label.data(), label.size());
    end_marker += "-----";
    const std::size_t end = s.find(end_marker, label_end + 5);
    const std::size_t stop = end == std::string_view::npos ? s.size() : end + end_marker.size();
    m.consumed = stop - i;
    m.replacement = std::string(kMarker);
    return true;
}

// scheme://user:pass@host:只在 userinfo 带冒号(用户名 + 密码)时替换,
// ssh://git@github.com 这类只有用户名的地址保持原样。
bool match_url_userinfo(std::string_view s, std::size_t i, Match& m) {
    if (!starts_with(s, i, "://")) return false;
    if (i == 0 || !is_ascii_alpha(static_cast<unsigned char>(s[i - 1]))) return false;
    std::size_t j = i + 3;
    bool has_colon = false;
    while (j < s.size()) {
        const char c = s[j];
        if (c == '@') break;
        if (c == '/' || c == '?' || c == '#' || c == ' ' || c == '\t' || c == '\r' ||
            c == '\n' || c == '"' || c == '\'' || c == '<' || c == '>') {
            return false;
        }
        if (c == ':') has_colon = true;
        ++j;
    }
    if (j >= s.size() || j == i + 3 || !has_colon) return false;
    const std::string_view userinfo = s.substr(i + 3, j - (i + 3));
    if (userinfo.back() == ':') return false;  // user: 后面没有密码
    m.consumed = j + 1 - i;
    m.replacement = "://" + std::string(kMarker) + "@";
    m.counted = userinfo != kMarker;
    return true;
}

struct TokenPrefix {
    std::string_view text;
    std::size_t min_tail;
    bool upper_alnum_tail;  // AKIA:后缀只允许大写字母与数字
};

constexpr std::array<TokenPrefix, 13> kTokenPrefixes = {{
    {"github_pat_", 20, false},
    {"sk-", 16, false},
    {"ghp_", 20, false},
    {"gho_", 20, false},
    {"ghu_", 20, false},
    {"ghs_", 20, false},
    {"ghr_", 20, false},
    {"xai-", 20, false},
    {"xoxb-", 10, false},
    {"xoxp-", 10, false},
    {"xoxa-", 10, false},
    {"AKIA", 16, true},
    {"AIza", 30, false},
}};

bool match_token_prefix(std::string_view s, std::size_t i, Match& m) {
    if (i > 0 && is_token_char(static_cast<unsigned char>(s[i - 1]))) return false;
    for (const auto& prefix : kTokenPrefixes) {
        if (!starts_with(s, i, prefix.text)) continue;
        std::size_t j = i + prefix.text.size();
        while (j < s.size()) {
            const auto c = static_cast<unsigned char>(s[j]);
            const bool ok = prefix.upper_alnum_tail
                ? (is_ascii_digit(c) || (c >= 'A' && c <= 'Z'))
                : is_token_char(c);
            if (!ok) break;
            ++j;
        }
        if (j - (i + prefix.text.size()) < prefix.min_tail) continue;
        m.consumed = j - i;
        m.replacement = std::string(kMarker);
        return true;
    }
    return false;
}

bool is_bearer_char(unsigned char c) {
    return is_ascii_alnum(c) || c == '.' || c == '_' || c == '~' || c == '+' ||
           c == '/' || c == '=' || c == '-';
}

// Bearer <token>:保留 Bearer 与空白,只替换令牌;令牌至少 8 个字符,
// 避免「bearer of bad news」这类自然语言被误改。
bool match_bearer(std::string_view s, std::size_t i, Match& m) {
    if (!starts_with_icase(s, i, "bearer")) return false;
    if (i > 0 && is_ascii_alnum(static_cast<unsigned char>(s[i - 1]))) return false;
    std::size_t j = i + 6;
    const std::size_t space_start = j;
    while (j < s.size() && (s[j] == ' ' || s[j] == '\t')) ++j;
    if (j == space_start) return false;
    const std::size_t token_start = j;
    if (starts_with(s, token_start, kMarker)) {
        m.consumed = token_start + kMarker.size() - i;
        m.replacement = std::string(s.substr(i, m.consumed));
        m.counted = false;
        return true;
    }
    while (j < s.size() && is_bearer_char(static_cast<unsigned char>(s[j]))) ++j;
    if (j - token_start < 8) return false;
    m.consumed = j - i;
    m.replacement = std::string(s.substr(i, token_start - i)) + std::string(kMarker);
    return true;
}

constexpr std::array<std::string_view, 7> kSecretKeys = {
    "password", "passwd", "pwd", "token", "secret", "api_key", "apikey",
};

// 键名长度(ASCII 键名不区分大小写;「密码」按字节比较)。0 = 不匹配。
std::size_t match_secret_key(std::string_view s, std::size_t i) {
    if (starts_with(s, i, kChinesePassword)) return kChinesePassword.size();
    if (i > 0 && is_ascii_alnum(static_cast<unsigned char>(s[i - 1]))) return 0;
    for (const auto key : kSecretKeys) {
        if (!starts_with_icase(s, i, key)) continue;
        const std::size_t end = i + key.size();
        // tokens=、token_type= 是别的键,不是 token 本身。
        if (end < s.size() && is_token_char(static_cast<unsigned char>(s[end]))) continue;
        return key.size();
    }
    return 0;
}

bool is_value_terminator(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',' || c == ';' ||
           c == '&' || c == '"' || c == '\'' || c == '<' || c == '>' || c == ')' ||
           c == ']' || c == '}';
}

// key = value / key: value / key:value / "key": "value"。
bool match_secret_assignment(std::string_view s, std::size_t i, Match& m) {
    const std::size_t key_len = match_secret_key(s, i);
    if (key_len == 0) return false;
    std::size_t j = i + key_len;
    if (j < s.size() && (s[j] == '"' || s[j] == '\'')) ++j;  // JSON 键的结束引号
    while (j < s.size() && (s[j] == ' ' || s[j] == '\t')) ++j;
    if (j >= s.size()) return false;
    if (s[j] == '=') {
        if (j + 1 < s.size() && s[j + 1] == '=') return false;  // 比较运算 ==
        ++j;
    } else if (s[j] == ':') {
        if (starts_with(s, j, "://")) return false;
        ++j;
    } else if (starts_with(s, j, kFullWidthColon)) {
        j += kFullWidthColon.size();
    } else {
        return false;
    }
    while (j < s.size() && (s[j] == ' ' || s[j] == '\t')) ++j;
    char quote = 0;
    if (j < s.size() && (s[j] == '"' || s[j] == '\'')) quote = s[j++];
    const std::size_t value_start = j;
    if (starts_with(s, value_start, kMarker)) {
        // 已脱敏的值原样保留(] 也是值终止符,不能把 [REDACTED] 切成两半再替换)。
        m.consumed = value_start + kMarker.size() - i;
        m.replacement = std::string(s.substr(i, m.consumed));
        m.counted = false;
        return true;
    }
    if (quote) {
        while (j < s.size() && s[j] != quote && s[j] != '\n') ++j;
    } else {
        while (j < s.size() && !is_value_terminator(s[j])) ++j;
    }
    if (j == value_start) return false;
    const std::string_view value = s.substr(value_start, j - value_start);
    m.consumed = j - i;
    m.replacement = std::string(s.substr(i, value_start - i)) + std::string(kMarker);
    m.counted = value != kMarker;
    return true;
}

bool match_at(std::string_view s, std::size_t i, Match& m) {
    const auto c = static_cast<unsigned char>(s[i]);
    if (c == '-' && match_pem_private_key(s, i, m)) return true;
    if (c == ':' && match_url_userinfo(s, i, m)) return true;
    if ((is_ascii_alpha(c) || c == 0xE5) && match_secret_assignment(s, i, m)) return true;
    if ((c == 'b' || c == 'B') && match_bearer(s, i, m)) return true;
    if (is_ascii_alpha(c) && match_token_prefix(s, i, m)) return true;
    return false;
}

} // namespace

SecretRedactionResult redact_secrets(std::string_view input) {
    SecretRedactionResult result;
    result.text.reserve(input.size());
    std::size_t i = 0;
    while (i < input.size()) {
        Match m;
        if (match_at(input, i, m) && m.consumed > 0) {
            result.text += m.replacement;
            if (m.counted) ++result.replacements;
            i += m.consumed;
            continue;
        }
        result.text.push_back(input[i]);
        ++i;
    }
    return result;
}

} // namespace acecode
