#include "memory_frontmatter.hpp"

#include "utils/frontmatter.hpp"
#include "utils/encoding.hpp"
#include "utils/logger.hpp"
#include "utils/utf8_path.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

namespace acecode {

namespace {

std::string read_file_to_string(const fs::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) return {};
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return ensure_utf8(oss.str());
}

// Escape a scalar string for the rendered frontmatter. Always double-quoted so
// values with colons or leading/trailing whitespace stay unambiguous; the
// reader below undoes exactly these escapes.
std::string render_scalar(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (char c : s) {
        if (c == '\\' || c == '"') {
            out.push_back('\\');
            out.push_back(c);
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            continue;
        } else {
            out.push_back(c);
        }
    }
    out.push_back('"');
    return out;
}

std::string strip(const std::string& s) {
    std::size_t a = 0;
    std::size_t b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

// 读回 render_scalar 写出的双引号值;单引号按 YAML 规则把 '' 还原为 '。
// 块标量(| / >)等复杂形态返回 nullopt,交给通用 frontmatter 解析器。
std::optional<std::string> parse_raw_scalar(const std::string& raw) {
    const std::string value = strip(raw);
    if (value.empty()) return std::string{};
    if (value[0] == '|' || value[0] == '>' || value[0] == '[') return std::nullopt;
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        std::string out;
        for (std::size_t i = 1; i + 1 < value.size(); ++i) {
            const char c = value[i];
            if (c == '\\' && i + 2 < value.size()) {
                const char next = value[++i];
                if (next == 'n') out.push_back('\n');
                else if (next == 't') out.push_back('\t');
                else out.push_back(next);
                continue;
            }
            out.push_back(c);
        }
        return out;
    }
    if (value.size() >= 2 && value.front() == '\'' && value.back() == '\'') {
        std::string out;
        for (std::size_t i = 1; i + 1 < value.size(); ++i) {
            out.push_back(value[i]);
            if (value[i] == '\'' && i + 2 < value.size() && value[i + 1] == '\'') ++i;
        }
        return out;
    }
    return value;
}

bool is_known_key(const std::string& key) {
    return key == "name" || key == "description" || key == "type" ||
           key == "created_at" || key == "updated_at" || key == "source" ||
           key == "source_sessions";
}

// 顶层 `key:` 行(无缩进)的键名;不是映射行返回空串。
std::string top_level_key(const std::string& line) {
    if (line.empty() || line[0] == ' ' || line[0] == '\t' || line[0] == '#' || line[0] == '-') {
        return {};
    }
    const std::size_t colon = line.find(':');
    if (colon == std::string::npos || colon == 0) return {};
    for (std::size_t i = 0; i < colon; ++i) {
        const auto c = static_cast<unsigned char>(line[i]);
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return {};
    }
    return line.substr(0, colon);
}

struct RawFrontmatter {
    std::vector<std::pair<std::string, std::string>> known;  // 已知键 → 首行的原始值
    std::string extra;                                       // 未知键的原始行
};

// 按顶层键切分 frontmatter 原文:已知键记下首行值,未知键连同其缩进续行 /
// 列表项原样收进 extra。frontmatter 不闭合时返回 nullopt。
std::optional<RawFrontmatter> split_raw_frontmatter(const std::string& content) {
    if (content.compare(0, 3, "---") != 0) return std::nullopt;
    std::size_t pos = content.find('\n');
    if (pos == std::string::npos) return std::nullopt;
    ++pos;
    RawFrontmatter out;
    bool in_extra = false;
    while (pos < content.size()) {
        std::size_t end = content.find('\n', pos);
        if (end == std::string::npos) end = content.size();
        std::string line = content.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        pos = end + 1;
        if (line == "---") return out;
        const std::string key = top_level_key(line);
        if (!key.empty()) {
            in_extra = !is_known_key(key);
            if (!in_extra) {
                out.known.emplace_back(key, line.substr(key.size() + 1));
                continue;
            }
        }
        if (in_extra) out.extra += line + "\n";
    }
    return std::nullopt;
}

std::string format_iso8601(std::time_t t) {
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

} // namespace

std::string memory_now_iso8601() {
    return format_iso8601(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
}

std::optional<long long> parse_memory_iso8601(const std::string& text) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    if (std::sscanf(text.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &s) != 6) {
        return std::nullopt;
    }
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 60) return std::nullopt;
    // days_from_civil(Howard Hinnant):与时区无关地把 UTC 日期换算成天数。
    const int yy = y - (mo <= 2 ? 1 : 0);
    const int era = (yy >= 0 ? yy : yy - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(yy - era * 400);
    const unsigned doy = (153u * static_cast<unsigned>(mo + (mo > 2 ? -3 : 9)) + 2u) / 5u +
                         static_cast<unsigned>(d) - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    const long long days = static_cast<long long>(era) * 146097LL + static_cast<long long>(doe) - 719468LL;
    return days * 86400LL + h * 3600LL + mi * 60LL + s;
}

std::string memory_file_mtime_iso8601(const fs::path& path) {
    std::error_code ec;
    const auto ftime = fs::last_write_time(path, ec);
    if (ec) return {};
    const auto system_time = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    return format_iso8601(std::chrono::system_clock::to_time_t(system_time));
}

std::optional<MemoryEntry> parse_memory_entry_text(const std::string& content,
                                                   const fs::path& path) {
    if (content.empty()) {
        LOG_WARN("[memory] entry file is empty or unreadable: " + path_to_utf8(path));
        return std::nullopt;
    }

    auto parsed_fm = parse_frontmatter(content);
    const Frontmatter& fm = parsed_fm.first;
    std::string body = std::move(parsed_fm.second);
    if (fm.empty()) {
        LOG_WARN("[memory] entry missing frontmatter: " + path_to_utf8(path));
        return std::nullopt;
    }
    const auto raw = split_raw_frontmatter(content);
    auto scalar = [&](const char* key) {
        if (raw) {
            for (const auto& [k, v] : raw->known) {
                if (k != key) continue;
                if (auto parsed = parse_raw_scalar(v)) return *parsed;
            }
        }
        return get_string(fm, key, "");
    };

    std::string desc = scalar("description");
    std::string type_s = scalar("type");
    if (desc.empty() || type_s.empty()) {
        LOG_WARN("[memory] entry missing required field (description/type): " +
                 path_to_utf8(path));
        return std::nullopt;
    }

    auto parsed_type = parse_memory_type(type_s);
    if (!parsed_type.has_value()) {
        LOG_WARN("[memory] entry has invalid type '" + type_s +
                 "' (allowed: user|feedback|project|reference): " + path_to_utf8(path));
        return std::nullopt;
    }

    MemoryEntry entry;
    // On-disk name wins: derive from the file stem so callers never confuse
    // frontmatter `name` with the identifier used for lookup.
    entry.name = path_to_utf8(path.stem());
    entry.description = desc;
    entry.type = *parsed_type;
    entry.path = path;
    // render_memory_entry 在 frontmatter 与正文之间固定写一个空行;读回时去掉,
    // 否则每次系统改写(补来源字段、整合 update)都会在正文前多攒一个空行。
    if (body.compare(0, 2, "\r\n") == 0) body.erase(0, 2);
    else if (!body.empty() && body.front() == '\n') body.erase(0, 1);
    entry.body = body;
    entry.created_at = scalar("created_at");
    entry.updated_at = scalar("updated_at");
    entry.source = scalar("source");
    entry.source_sessions = get_list(fm, "source_sessions");
    if (raw) entry.extra_frontmatter = raw->extra;
    return entry;
}

std::optional<MemoryEntry> parse_memory_entry_file(const fs::path& path) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || ec) {
        return std::nullopt;
    }
    return parse_memory_entry_text(read_file_to_string(path), path);
}

std::string render_memory_entry(const MemoryEntry& entry) {
    std::ostringstream oss;
    oss << "---\n"
        << "name: "        << render_scalar(entry.name)        << "\n"
        << "description: " << render_scalar(entry.description) << "\n"
        << "type: "        << memory_type_to_string(entry.type) << "\n";
    if (!entry.created_at.empty()) oss << "created_at: " << entry.created_at << "\n";
    if (!entry.updated_at.empty()) oss << "updated_at: " << entry.updated_at << "\n";
    if (!entry.source.empty()) oss << "source: " << entry.source << "\n";
    if (!entry.source_sessions.empty()) {
        oss << "source_sessions: [";
        for (std::size_t i = 0; i < entry.source_sessions.size(); ++i) {
            if (i) oss << ", ";
            oss << render_scalar(entry.source_sessions[i]);
        }
        oss << "]\n";
    }
    oss << entry.extra_frontmatter;
    if (!entry.extra_frontmatter.empty() && entry.extra_frontmatter.back() != '\n') oss << "\n";
    oss << "---\n\n"
        << entry.body;
    // Ensure a trailing newline so tools like `git diff` stay sane.
    if (entry.body.empty() || entry.body.back() != '\n') {
        oss << "\n";
    }
    return oss.str();
}

} // namespace acecode
