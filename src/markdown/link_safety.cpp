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

    // scheme:跳过 "://" 之前的 authority 前缀。无 "://" 时整个字符串按
    // authority 解析(裸域名 / userinfo / 端口 / 路径形式),不做协议名判定——
    // "mailto:" 这类单冒号协议无法与 "user:pass@" 可靠区分,而两者解析出
    // 的 host 都是安全的比较对象(file:/// 等 host 为空的情况仍会失败)。
    const std::string::size_type scheme = u.find("://");
    if (scheme != std::string::npos) {
        u = u.substr(scheme + 3);
    }

    // userinfo:取最后一个 '@' 之后(host 之前的 user:pass@)。
    const std::string::size_type at = u.rfind('@');
    if (at != std::string::npos) {
        u = u.substr(at + 1);
    }

    // host 段:到 ':'(端口)、'/'(路径)、'?'(查询)、'#'(片段) 为止。
    const std::string::size_type end = u.find_first_of(":/?#");
    if (end != std::string::npos) {
        u = u.substr(0, end);
    }

    // 去尾部句点(句子结尾标点)。
    while (!u.empty() && u.back() == '.') {
        u.pop_back();
    }
    if (u.empty()) {
        return std::nullopt;
    }

    // 域名不区分大小写 → 统一小写,便于比较。
    std::transform(u.begin(), u.end(), u.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return u;
}

bool is_safe_link_label(const std::string& label, const std::string& href) {
    // href 不是远程 URL(无 "://")→ 本地文件路径 / 裸域名通道,label 是
    // 文件名或任意文字,不参与防骗比较,放行。注意防骗只针对网页链接
    // (http/https),本地相对路径 `[foo.md](docs/foo.md)` 是常见写法,
    // 不能因文件名带点号而被误降级。
    if (href.find("://") == std::string::npos) {
        return true;
    }

    // href 是远程 URL:host 无法解析(file:///、畸形)→ 无目标可比,放行。
    const auto href_host = extract_url_host(href);
    if (!href_host.has_value()) {
        return true;
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
