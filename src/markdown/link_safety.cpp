#include "link_safety.hpp"

#include <algorithm>
#include <cctype>

namespace acecode::markdown {

namespace {

std::string trim_copy(const std::string& s) {
    const auto is_space = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    std::string::size_type begin = 0;
    while (begin < s.size() && is_space(static_cast<unsigned char>(s[begin]))) {
        ++begin;
    }
    std::string::size_type end = s.size();
    while (end > begin && is_space(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return s.substr(begin, end - begin);
}

// "URL 形状":含点号、不含空白。普通文字标签(如"我的博客")不满足;
// 伪装 URL 文本(如 "google.com" / "https://google.com/search")满足。
bool looks_like_url(const std::string& s) {
    if (s.find('.') == std::string::npos) {
        return false;
    }
    return s.find_first_of(" \t\r\n") == std::string::npos;
}

bool is_valid_port(const std::string& port) {
    if (port.empty()) return false;
    unsigned int value = 0;
    for (const unsigned char c : port) {
        if (std::isdigit(c) == 0) return false;
        const unsigned int digit = static_cast<unsigned int>(c - '0');
        if (value > (65535u - digit) / 10u) return false;
        value = value * 10u + digit;
    }
    return true;
}

bool is_valid_dns_host(const std::string& host) {
    if (host.empty() || host.front() == '.' || host.back() == '.' ||
        host.find("..") != std::string::npos) {
        return false;
    }
    return std::all_of(host.begin(), host.end(), [](unsigned char c) {
        return std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.';
    });
}

bool is_valid_bracketed_ipv6_host(const std::string& host) {
    if (host.empty() || host.find(':') == std::string::npos) {
        return false;
    }
    return std::all_of(host.begin(), host.end(), [](unsigned char c) {
        return std::isxdigit(c) != 0 || c == ':' || c == '.';
    });
}

} // namespace

std::optional<std::string> extract_url_host(const std::string& url) {
    std::string u = trim_copy(url);
    if (u.empty()) {
        return std::nullopt;
    }

    // 非 ASCII(IDN 域名 / 中文文件名)→ 不做 IDN/百分号解码,按畸形处理。
    for (const char c : u) {
        if (static_cast<unsigned char>(c) > 0x7F) {
            return std::nullopt;
        }
    }

    // scheme:跳过 "://" 之前的前缀。无 "://" 时从字符串开头按 authority
    // 解析(裸域名 / userinfo / 端口 / 路径形式)。先找 authority 的结束位置,
    // 再处理 userinfo:路径/query 中的 "@trusted.example" 绝不能覆盖真实 host。
    const std::string::size_type scheme = u.find("://");
    std::string::size_type authority_begin = 0;
    if (scheme != std::string::npos) {
        if (scheme == 0 || !std::isalpha(static_cast<unsigned char>(u[0]))) {
            return std::nullopt;
        }
        for (std::string::size_type i = 1; i < scheme; ++i) {
            const unsigned char c = static_cast<unsigned char>(u[i]);
            if (std::isalnum(c) == 0 && c != '+' && c != '-' && c != '.') {
                return std::nullopt;
            }
        }
        authority_begin = scheme + 3;
    }

    const std::string::size_type authority_end =
        u.find_first_of("/\\?#", authority_begin);
    std::string authority = u.substr(
        authority_begin,
        authority_end == std::string::npos
            ? std::string::npos
            : authority_end - authority_begin);
    if (authority.empty()) {
        return std::nullopt;
    }

    // userinfo 只允许出现在 authority 内。取最后一个 '@' 后的 host:port。
    const std::string::size_type at = authority.rfind('@');
    if (at != std::string::npos) {
        authority = authority.substr(at + 1);
    }
    if (authority.empty()) {
        return std::nullopt;
    }

    std::string host;
    bool bracketed_ipv6 = false;
    if (authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string::npos) {
            return std::nullopt;
        }
        host = authority.substr(1, close - 1);
        const std::string suffix = authority.substr(close + 1);
        if (!suffix.empty() &&
            (suffix.front() != ':' || !is_valid_port(suffix.substr(1)))) {
            return std::nullopt;
        }
        bracketed_ipv6 = true;
    } else {
        const auto colon = authority.find(':');
        if (colon == std::string::npos) {
            host = authority;
        } else {
            // 未加方括号的 IPv6 与 host:port 有歧义,拒绝。
            if (authority.find(':', colon + 1) != std::string::npos ||
                !is_valid_port(authority.substr(colon + 1))) {
                return std::nullopt;
            }
            host = authority.substr(0, colon);
        }
    }

    // 去尾部句点(裸域名位于句子结尾时的标点)。
    while (!bracketed_ipv6 && !host.empty() && host.back() == '.') {
        host.pop_back();
    }
    if (bracketed_ipv6 ? !is_valid_bracketed_ipv6_host(host)
                       : !is_valid_dns_host(host)) {
        return std::nullopt;
    }

    // 域名不区分大小写 → 统一小写,便于比较。
    std::transform(host.begin(), host.end(), host.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return host;
}

bool is_safe_link_label(const std::string& label, const std::string& href) {
    // href 不是远程 URL(无 "://")→ 本地文件路径 / 裸域名通道,label 是
    // 文件名或任意文字,不参与防骗比较,放行。注意防骗只针对网页链接
    // (http/https),本地相对路径 `[foo.md](docs/foo.md)` 是常见写法,
    // 不能因文件名带点号而被误降级。
    if (href.find("://") == std::string::npos) {
        return true;
    }

    // href 是远程 URL:URL 形 label 必须有可验证目标;否则 fail closed。
    const auto href_host = extract_url_host(href);
    if (!href_host.has_value()) {
        return !looks_like_url(label);
    }

    // label 不呈 URL 形状 → 普通文字标签(如"我的博客"),放行。
    if (!looks_like_url(label)) {
        return true;
    }

    // label 呈 URL 形状 → host 必须与 href host 一致;畸形/非 ASCII → 降级。
    const auto label_host = extract_url_host(label);
    if (!label_host.has_value()) {
        return false;
    }
    return *label_host == *href_host;
}

} // namespace acecode::markdown
