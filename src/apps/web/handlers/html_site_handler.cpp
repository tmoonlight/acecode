#include "web/handlers/html_site_handler.hpp"

#include "utils/encoding.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <random>
#include <system_error>
#include <utility>

namespace acecode::web {

namespace fs = std::filesystem;

namespace {

constexpr std::string_view kHtmlSitePrefix = "/api/html-site/";

fs::path utf8_path(const std::string& s) {
#ifdef _WIN32
    return fs::path(acecode::utf8_to_wide(s));
#else
    return fs::path(s);
#endif
}

std::string generic_utf8(const fs::path& p) {
#ifdef _WIN32
    return acecode::wide_to_utf8(p.lexically_normal().generic_wstring());
#else
    return p.lexically_normal().generic_string();
#endif
}

// 前缀比较用:正斜杠、去掉结尾斜杠;Windows 上不分大小写。
std::string compare_key(const fs::path& p) {
    std::string out = generic_utf8(p);
    if (out.size() > 1 && out.back() == '/') out.pop_back();
#ifdef _WIN32
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
#endif
    return out;
}

fs::path canonical_or_normal(const fs::path& p) {
    std::error_code ec;
    fs::path out = fs::weakly_canonical(p, ec);
    if (ec) out = p.lexically_normal();
    return out;
}

bool is_within(const std::string& root_key, const std::string& target_key) {
    if (target_key == root_key) return false;  // 根目录本身不是文件
    std::string prefix = root_key;
    if (prefix.empty() || prefix.back() != '/') prefix += '/';
    return target_key.rfind(prefix, 0) == 0;
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string lower_extension(const std::string& path) {
    const auto slash = path.find_last_of("/\\");
    const auto dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash) ||
        dot + 1 >= path.size()) {
        return {};
    }
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext;
}

} // namespace

HtmlSiteRegistry::HtmlSiteRegistry(std::size_t capacity)
    : capacity_(capacity == 0 ? 1 : capacity) {}

std::string HtmlSiteRegistry::register_root(const fs::path& root) {
    const std::string key = compare_key(root);
    std::lock_guard<std::mutex> lock(mu_);
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (it->root_key != key) continue;
        Entry hit = std::move(*it);
        entries_.erase(it);
        entries_.push_back(std::move(hit));
        return entries_.back().id;
    }
    while (entries_.size() >= capacity_) entries_.erase(entries_.begin());
    entries_.push_back(Entry{generate_html_site_id(), root, key});
    return entries_.back().id;
}

std::optional<fs::path> HtmlSiteRegistry::lookup(const std::string& id) {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (it->id != id) continue;
        Entry hit = std::move(*it);
        entries_.erase(it);
        entries_.push_back(std::move(hit));
        return entries_.back().root;
    }
    return std::nullopt;
}

std::string generate_html_site_id() {
    static constexpr char kHex[] = "0123456789abcdef";
    std::random_device device;
    std::string id;
    id.reserve(32);
    for (int word = 0; word < 4; ++word) {
        auto value = static_cast<std::uint32_t>(device());
        for (int nibble = 0; nibble < 8; ++nibble) {
            id.push_back(kHex[value & 0xF]);
            value >>= 4;
        }
    }
    return id;
}

bool is_html_site_id(std::string_view id) {
    if (id.size() != 32) return false;
    return std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

std::optional<std::string> html_site_mime(const std::string& path) {
    static const std::array<std::pair<const char*, const char*>, 33> kTypes = {{
        {"html", "text/html; charset=utf-8"},
        {"htm", "text/html; charset=utf-8"},
        {"css", "text/css; charset=utf-8"},
        {"js", "text/javascript; charset=utf-8"},
        {"mjs", "text/javascript; charset=utf-8"},
        {"cjs", "text/javascript; charset=utf-8"},
        {"map", "application/json; charset=utf-8"},
        {"json", "application/json; charset=utf-8"},
        {"txt", "text/plain; charset=utf-8"},
        {"csv", "text/csv; charset=utf-8"},
        {"xml", "application/xml; charset=utf-8"},
        {"svg", "image/svg+xml"},
        {"png", "image/png"},
        {"jpg", "image/jpeg"},
        {"jpeg", "image/jpeg"},
        {"gif", "image/gif"},
        {"webp", "image/webp"},
        {"avif", "image/avif"},
        {"bmp", "image/bmp"},
        {"ico", "image/x-icon"},
        {"woff", "font/woff"},
        {"woff2", "font/woff2"},
        {"ttf", "font/ttf"},
        {"otf", "font/otf"},
        {"eot", "application/vnd.ms-fontobject"},
        {"mp4", "video/mp4"},
        {"webm", "video/webm"},
        {"ogv", "video/ogg"},
        {"mp3", "audio/mpeg"},
        {"wav", "audio/wav"},
        {"ogg", "audio/ogg"},
        {"m4a", "audio/mp4"},
        {"wasm", "application/wasm"},
    }};
    const std::string ext = lower_extension(path);
    if (ext.empty()) return std::nullopt;
    for (const auto& [name, mime] : kTypes) {
        if (ext == name) return std::string(mime);
    }
    return std::nullopt;
}

bool html_site_mime_is_document(const std::string& mime) {
    return mime.rfind("text/html", 0) == 0 || mime.rfind("image/svg+xml", 0) == 0 ||
           mime.rfind("application/xml", 0) == 0;
}

bool html_site_allows_cors(std::string_view dest) {
    static constexpr std::array<std::string_view, 9> kSubresources = {
        "script", "style", "font", "image", "audio", "video", "track", "worker",
        "sharedworker",
    };
    return std::find(kSubresources.begin(), kSubresources.end(), dest) != kSubresources.end();
}

std::optional<HtmlSiteRequestPath> split_html_site_url(std::string_view url) {
    if (url.rfind(kHtmlSitePrefix, 0) != 0) return std::nullopt;
    std::string_view tail = url.substr(kHtmlSitePrefix.size());
    const auto slash = tail.find('/');
    if (slash == std::string_view::npos || slash == 0) return std::nullopt;
    HtmlSiteRequestPath out;
    out.id = std::string(tail.substr(0, slash));
    out.rest = std::string(tail.substr(slash + 1));
    return out;
}

std::optional<std::string> decode_html_site_path(std::string_view encoded) {
    std::string out;
    out.reserve(encoded.size());
    for (std::size_t i = 0; i < encoded.size(); ++i) {
        const char c = encoded[i];
        if (c != '%') {
            out.push_back(c);
            continue;
        }
        if (i + 2 >= encoded.size()) return std::nullopt;
        const int hi = hex_value(encoded[i + 1]);
        const int lo = hex_value(encoded[i + 2]);
        if (hi < 0 || lo < 0) return std::nullopt;
        const char decoded = static_cast<char>((hi << 4) | lo);
        if (decoded == '\0') return std::nullopt;
        out.push_back(decoded);
        i += 2;
    }
    return out;
}

std::optional<fs::path> resolve_html_site_file(const fs::path& root, const std::string& relative) {
    if (relative.empty() || relative.find('\0') != std::string::npos) return std::nullopt;
    if (relative.find('\\') != std::string::npos || relative.find(':') != std::string::npos) {
        return std::nullopt;
    }
    fs::path joined = root;
    std::size_t start = 0;
    while (start <= relative.size()) {
        const auto end = relative.find('/', start);
        const std::string segment = relative.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        if (segment.empty() || segment == "." || segment == "..") return std::nullopt;
        joined /= utf8_path(segment);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    const fs::path canonical_root = canonical_or_normal(root);
    const fs::path target = canonical_or_normal(joined);
    if (!is_within(compare_key(canonical_root), compare_key(target))) return std::nullopt;
    return target;
}

std::optional<HtmlSitePlan> plan_html_site(const fs::path& file,
                                           const std::optional<fs::path>& workspace_root) {
    const fs::path target = canonical_or_normal(file);
    if (!target.is_absolute() || !target.has_parent_path()) return std::nullopt;
    fs::path root = target.parent_path();
    if (workspace_root) {
        const fs::path workspace = canonical_or_normal(*workspace_root);
        if (is_within(compare_key(workspace), compare_key(target))) root = workspace;
    }
    const fs::path relative = target.lexically_relative(root);
    if (relative.empty()) return std::nullopt;
    const std::string entry = generic_utf8(relative);
    if (entry.empty() || entry.rfind("..", 0) == 0) return std::nullopt;
    return HtmlSitePlan{root, entry};
}

std::string encode_html_site_path(const std::string& relative) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(relative.size());
    for (const unsigned char c : relative) {
        const bool unreserved = std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' ||
                                c == '/';
        if (unreserved) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0xF]);
        }
    }
    return out;
}

} // namespace acecode::web
