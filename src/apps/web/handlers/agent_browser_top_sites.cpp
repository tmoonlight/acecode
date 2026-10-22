#include "web/handlers/agent_browser_top_sites.hpp"

#include "utils/atomic_file.hpp"
#include "utils/encoding.hpp"
#include "utils/utf8_path.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace acecode::web::agent_browser_top_sites {
using nlohmann::json;

namespace {

constexpr int kDocumentVersion = 1;
constexpr std::int64_t kDayMs = 24LL * 60 * 60 * 1000;

bool starts_with_ascii_ci(const std::string& value, const char* prefix) {
    std::size_t i = 0;
    for (; prefix[i] != '\0'; ++i) {
        if (i >= value.size()) return false;
        const auto a = static_cast<unsigned char>(value[i]);
        const auto b = static_cast<unsigned char>(prefix[i]);
        if (std::tolower(a) != std::tolower(b)) return false;
    }
    return true;
}

bool has_control_or_space(const std::string& value) {
    return std::any_of(value.begin(), value.end(), [](char c) {
        const auto byte = static_cast<unsigned char>(c);
        return byte <= 0x20 || byte == 0x7F;
    });
}

std::string string_field(const json& object, const char* key) {
    if (!object.is_object()) return {};
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::int64_t int_field(const json& object, const char* key) {
    if (!object.is_object()) return 0;
    const auto it = object.find(key);
    if (it == object.end() || !it->is_number()) return 0;
    if (it->is_number_float()) {
        const double value = it->get<double>();
        return value > 0 ? static_cast<std::int64_t>(value) : 0;
    }
    const auto value = it->get<std::int64_t>();
    return value > 0 ? value : 0;
}

std::string clean_title(const std::string& title) {
    std::string out;
    out.reserve(title.size());
    for (char c : title) {
        const auto byte = static_cast<unsigned char>(c);
        out.push_back(byte < 0x20 || byte == 0x7F ? ' ' : c);
    }
    const auto first = out.find_first_not_of(' ');
    if (first == std::string::npos) return {};
    const auto last = out.find_last_not_of(' ');
    out = out.substr(first, last - first + 1);
    return ensure_utf8(truncate_utf8_prefix(out, kMaxTitleBytes));
}

// "https://user@Example.com:8080/a" → "Example.com:8080"(推荐无标题时显示)。
std::string host_of(const std::string& url) {
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return url;
    const auto start = scheme_end + 3;
    auto end = url.find_first_of("/?#", start);
    if (end == std::string::npos) end = url.size();
    std::string authority = url.substr(start, end - start);
    const auto at = authority.rfind('@');
    if (at != std::string::npos) authority = authority.substr(at + 1);
    return authority;
}

double recency_weight(std::int64_t age_ms) {
    if (age_ms < 0) age_ms = 0;
    if (age_ms <= 4 * kDayMs) return 1.0;
    if (age_ms <= 14 * kDayMs) return 0.7;
    if (age_ms <= 31 * kDayMs) return 0.5;
    if (age_ms <= 90 * kDayMs) return 0.3;
    return 0.1;
}

json site_entry(const std::string& url,
                const std::string& title,
                const std::string& favicon,
                std::int64_t visit_count,
                std::int64_t last_visit_ms) {
    json site = json::object();
    site["url"] = url;
    if (!title.empty()) site["title"] = title;
    if (!favicon.empty()) site["favicon"] = favicon;
    site["visit_count"] = visit_count;
    site["last_visit_ms"] = last_visit_ms;
    return site;
}

// 分数从高到低;同分时最近访问的在前,再按 URL 保证顺序稳定。
void sort_by_score(std::vector<json>& sites, std::int64_t now_ms) {
    std::stable_sort(sites.begin(), sites.end(), [now_ms](const json& a, const json& b) {
        const double score_a = site_score(a, now_ms);
        const double score_b = site_score(b, now_ms);
        if (score_a != score_b) return score_a > score_b;
        const auto last_a = int_field(a, "last_visit_ms");
        const auto last_b = int_field(b, "last_visit_ms");
        if (last_a != last_b) return last_a > last_b;
        return string_field(a, "url") < string_field(b, "url");
    });
}

std::int64_t latest_visit(const json& doc) {
    std::int64_t latest = 0;
    for (const auto& site : doc["sites"]) {
        latest = std::max(latest, int_field(site, "last_visit_ms"));
    }
    return latest;
}

// 超出上限时丢分数最低的网站;分数的「现在」取文档里最近一次访问,
// 让同一份磁盘内容每次归一出相同结果。
void trim_sites(json& doc, std::int64_t now_ms) {
    auto& sites = doc["sites"];
    if (sites.size() <= kMaxStoredSites) return;
    std::vector<json> sorted(sites.begin(), sites.end());
    sort_by_score(sorted, now_ms);
    sorted.resize(kMaxStoredSites);
    sites = json(sorted);
}

json::iterator find_site(json& doc, const std::string& url) {
    auto& sites = doc["sites"];
    return std::find_if(sites.begin(), sites.end(), [&url](const json& site) {
        return string_field(site, "url") == url;
    });
}

} // namespace

std::string file_path(const std::string& data_dir) {
    return path_to_utf8(path_from_utf8(data_dir) / "agent_browser_top_sites.json");
}

std::string normalize_url(const std::string& raw) {
    std::string url = raw;
    const auto hash = url.find('#');
    if (hash != std::string::npos) url.resize(hash);
    if (url.empty() || url.size() > kMaxUrlBytes || has_control_or_space(url)) return {};
    std::size_t scheme_length = 0;
    if (starts_with_ascii_ci(url, "https://")) {
        scheme_length = 5;
    } else if (starts_with_ascii_ci(url, "http://")) {
        scheme_length = 4;
    } else {
        return {};
    }
    for (std::size_t i = 0; i < scheme_length; ++i) {
        url[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(url[i])));
    }
    if (host_of(url).empty()) return {};
    return url;
}

std::string normalize_favicon(const std::string& favicon) {
    if (favicon.empty()) return {};
    if (starts_with_ascii_ci(favicon, "data:image/")) {
        return favicon.size() <= kMaxFaviconDataUrlBytes ? favicon : std::string();
    }
    if (favicon.size() > kMaxUrlBytes || has_control_or_space(favicon)) return {};
    if (starts_with_ascii_ci(favicon, "https://") || starts_with_ascii_ci(favicon, "http://")) {
        return favicon;
    }
    return {};
}

json empty_document() {
    return json{{"version", kDocumentVersion},
                {"sites", json::array()},
                {"blocked", json::array()}};
}

json normalize_document(const json& raw) {
    json doc = empty_document();
    if (!raw.is_object()) return doc;

    std::unordered_map<std::string, std::size_t> index;
    const auto sites_it = raw.find("sites");
    if (sites_it != raw.end() && sites_it->is_array()) {
        for (const auto& site : *sites_it) {
            const auto url = normalize_url(string_field(site, "url"));
            if (url.empty()) continue;
            const auto title = clean_title(string_field(site, "title"));
            const auto favicon = normalize_favicon(string_field(site, "favicon"));
            const auto count = std::max<std::int64_t>(1, int_field(site, "visit_count"));
            const auto last = int_field(site, "last_visit_ms");
            const auto existing = index.find(url);
            if (existing == index.end()) {
                index.emplace(url, doc["sites"].size());
                doc["sites"].push_back(site_entry(url, title, favicon, count, last));
                continue;
            }
            // 同一 URL 出现两次(手改文件 / 旧版本 fragment 未去掉):合并计数,
            // 标题图标取最近那次。
            auto& merged = doc["sites"][existing->second];
            merged["visit_count"] = int_field(merged, "visit_count") + count;
            if (last >= int_field(merged, "last_visit_ms")) {
                merged["last_visit_ms"] = last;
                if (!title.empty()) merged["title"] = title;
                if (!favicon.empty()) merged["favicon"] = favicon;
            }
        }
    }

    const auto blocked_it = raw.find("blocked");
    if (blocked_it != raw.end() && blocked_it->is_array()) {
        std::unordered_set<std::string> seen;
        for (const auto& value : *blocked_it) {
            if (!value.is_string()) continue;
            const auto url = normalize_url(value.get<std::string>());
            if (url.empty() || !seen.insert(url).second) continue;
            doc["blocked"].push_back(url);
        }
        auto& blocked = doc["blocked"];
        if (blocked.size() > kMaxBlockedUrls) {
            blocked.erase(blocked.begin(),
                          blocked.begin() + static_cast<std::ptrdiff_t>(blocked.size() - kMaxBlockedUrls));
        }
    }

    trim_sites(doc, latest_visit(doc));
    return doc;
}

double site_score(const json& site, std::int64_t now_ms) {
    const auto count = std::max<std::int64_t>(1, int_field(site, "visit_count"));
    return static_cast<double>(count) *
           recency_weight(now_ms - int_field(site, "last_visit_ms"));
}

bool record_visit(json& doc, const Visit& visit, std::int64_t now_ms) {
    const auto url = normalize_url(visit.url);
    if (url.empty()) return false;
    const auto title = clean_title(visit.title);
    const auto favicon = normalize_favicon(visit.favicon);

    auto site = find_site(doc, url);
    if (site == doc["sites"].end()) {
        if (!visit.new_visit) return false;
        doc["sites"].push_back(site_entry(url, title, favicon, 1, now_ms));
        trim_sites(doc, now_ms);
        return true;
    }

    bool changed = false;
    if (visit.new_visit) {
        (*site)["visit_count"] = int_field(*site, "visit_count") + 1;
        (*site)["last_visit_ms"] = now_ms;
        changed = true;
    }
    if (!title.empty() && string_field(*site, "title") != title) {
        (*site)["title"] = title;
        changed = true;
    }
    if (!favicon.empty() && string_field(*site, "favicon") != favicon) {
        (*site)["favicon"] = favicon;
        changed = true;
    }
    return changed;
}

bool dismiss(json& doc, const std::string& raw_url) {
    const auto url = normalize_url(raw_url);
    if (url.empty()) return false;
    auto& blocked = doc["blocked"];
    if (std::find(blocked.begin(), blocked.end(), json(url)) != blocked.end()) return false;
    blocked.push_back(url);
    if (blocked.size() > kMaxBlockedUrls) blocked.erase(blocked.begin());
    return true;
}

bool restore(json& doc, const std::string& raw_url) {
    const auto url = normalize_url(raw_url);
    if (url.empty()) return false;
    auto& blocked = doc["blocked"];
    const auto it = std::find(blocked.begin(), blocked.end(), json(url));
    if (it == blocked.end()) return false;
    blocked.erase(it);
    return true;
}

json display_sites(const json& doc, std::size_t limit, std::int64_t now_ms) {
    std::unordered_set<std::string> blocked;
    for (const auto& value : doc["blocked"]) {
        if (value.is_string()) blocked.insert(value.get<std::string>());
    }
    std::vector<json> candidates;
    for (const auto& site : doc["sites"]) {
        if (blocked.count(string_field(site, "url")) == 0) candidates.push_back(site);
    }
    sort_by_score(candidates, now_ms);

    json out = json::array();
    for (const auto& site : candidates) {
        if (out.size() >= limit) break;
        const auto url = string_field(site, "url");
        auto title = string_field(site, "title");
        if (title.empty()) title = host_of(url);
        json item{{"url", url}, {"title", title}};
        const auto favicon = string_field(site, "favicon");
        if (!favicon.empty()) item["favicon"] = favicon;
        out.push_back(std::move(item));
    }
    return out;
}

json load(const std::string& path, std::string* warning) {
    if (warning) warning->clear();
    std::error_code ec;
    const auto fs_path = path_from_utf8(path);
    if (!std::filesystem::exists(fs_path, ec) || ec) return empty_document();
    std::ifstream in(fs_path, std::ios::binary);
    if (!in.is_open()) {
        if (warning) *warning = "cannot open " + path;
        return empty_document();
    }
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    const auto parsed = json::parse(text, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        if (warning) *warning = path + ": invalid JSON";
        return empty_document();
    }
    return normalize_document(parsed);
}

bool save(const std::string& path, const json& doc, std::string* error) {
    if (error) error->clear();
    const std::string text = doc.dump(-1, ' ', false, json::error_handler_t::replace);
    if (!atomic_write_file(path, text)) {
        if (error) *error = "cannot write " + path;
        return false;
    }
    return true;
}

} // namespace acecode::web::agent_browser_top_sites
