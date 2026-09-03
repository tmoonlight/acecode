#include "skill_loader.hpp"

#include "frontmatter.hpp"
#include "../utils/encoding.hpp"
#include "../utils/logger.hpp"
#include "../utils/utf8_path.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace acecode {

namespace {

constexpr size_t FRONTMATTER_READ_BUDGET = 8 * 1024; // first 8KB is plenty

std::string read_frontmatter_chunk(const fs::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) return "";
    std::string buf(FRONTMATTER_READ_BUDGET, '\0');
    ifs.read(buf.data(), static_cast<std::streamsize>(FRONTMATTER_READ_BUDGET));
    buf.resize(static_cast<size_t>(ifs.gcount()));
    // A SKILL.md larger than the read budget gets sliced at an arbitrary byte,
    // often mid-character. Drop the partial trailing UTF-8 sequence so ensure_utf8
    // doesn't see "invalid UTF-8" and re-decode the whole (valid UTF-8) chunk as
    // the system codepage — which would turn the frontmatter into mojibake.
    trim_trailing_partial_utf8(buf);
    return ensure_utf8(buf);
}

std::string truncate(const std::string& s, size_t n) {
    if (s.size() <= n) return s;
    // 字节预算截断必须回退到 UTF-8 序列边界,否则切在多字节字符中间会
    // 留下"残缺引导字节 + '.'"的非法序列(如 0xE9 0x94 + "..." 的 0x2E),
    // 进入 skills 索引后会被 body.dump() 以 type_error.316 打挂整个请求。
    return truncate_utf8_prefix(s, n, "...");
}

std::string first_non_empty_body_line(const std::string& body) {
    std::istringstream iss(body);
    std::string line;
    while (std::getline(iss, line)) {
        std::string t;
        t.reserve(line.size());
        size_t i = 0;
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
        while (i < line.size()) t.push_back(line[i++]);
        while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back()))) t.pop_back();
        if (t.empty()) continue;
        if (t.front() == '#') continue;
        // A stray delimiter is never a description. Unterminated frontmatter
        // dumps the whole file into `body`, so without this the settings page
        // would show a skill whose description reads "---".
        if (t == "---") continue;
        return t;
    }
    return "";
}

std::string derive_category(const fs::path& skill_dir, const fs::path& scan_root) {
    std::error_code ec;
    auto rel = fs::relative(skill_dir, scan_root, ec);
    if (ec) return "";
    auto it = rel.begin();
    if (it == rel.end()) return "";
    // rel = "<category>/<name>" (at minimum two components) → first segment
    // is the category. A flat "<name>" layout has no category.
    auto first = *it;
    ++it;
    if (it == rel.end()) return "";
    return path_to_utf8(first);
}

// Cap every string that reaches the JSON payload / prompt index. Names and
// categories were previously unbounded and unsanitised: a SKILL.md whose
// `name:` carried bytes that survived as invalid UTF-8 made nlohmann's
// dump() abort the whole /api/skills response with type_error.316, i.e. one
// malformed skill turned the entire settings page into a 500.
constexpr size_t FIELD_BUDGET = 1024;

std::string sanitize_field(const std::string& s, size_t budget = FIELD_BUDGET) {
    return ensure_utf8(truncate(s, budget));
}

std::vector<std::string> sanitize_fields(std::vector<std::string> items) {
    for (auto& item : items) item = sanitize_field(item, 256);
    return items;
}

// `detail` is English on purpose: it matches the rest of the web layer's
// error strings and doubles as the log line. The settings page renders its
// own localized wording from `error_code` and only falls back to this text
// when it meets a code it does not know (newer daemon, older front end).
SkillLoadIssue make_issue(SkillLoadFailure failure,
                          std::string detail,
                          const fs::path& dir,
                          const fs::path& scan_root,
                          const fs::path& skill_md,
                          std::string name) {
    SkillLoadIssue issue;
    issue.failure = failure;
    issue.detail = sanitize_field(std::move(detail));
    issue.skill_dir = dir;
    issue.scan_root = scan_root;
    issue.skill_md_path = skill_md;
    issue.category = sanitize_field(derive_category(dir, scan_root), 256);
    if (name.empty()) name = path_to_utf8(dir.filename());
    issue.name = sanitize_field(std::move(name), 256);
    return issue;
}

SkillLoadOutcome inspect_skill_dir_impl(const fs::path& dir,
                                        const fs::path& scan_root) {
    SkillLoadOutcome outcome;
    fs::path skill_md = dir / "SKILL.md";
    std::error_code ec;
    if (!fs::exists(skill_md, ec) || !fs::is_regular_file(skill_md, ec)) {
        // Not a skill directory at all. Stay silent rather than reporting a
        // phantom broken skill.
        return outcome;
    }

    std::string chunk = read_frontmatter_chunk(skill_md);
    if (chunk.empty()) {
        LOG_WARN("[skills] Empty or unreadable SKILL.md: " + path_to_utf8(skill_md));
        outcome.issue = make_issue(SkillLoadFailure::Unreadable,
                                   "SKILL.md is empty or could not be read",
                                   dir, scan_root, skill_md, "");
        return outcome;
    }

    FrontmatterShape shape = FrontmatterShape::None;
    auto [fm, body] = parse_frontmatter(chunk, &shape);

    SkillMetadata meta;
    meta.skill_md_path = skill_md;
    meta.skill_dir = dir;
    meta.scan_root = scan_root;

    std::string fm_name = get_string(fm, "name");
    meta.name = sanitize_field(
        fm_name.empty() ? path_to_utf8(dir.filename()) : fm_name, 256);

    if (meta.name.empty()) {
        outcome.issue = make_issue(SkillLoadFailure::MissingName,
                                   "SKILL.md has no name and its directory "
                                   "name cannot be used as one",
                                   dir, scan_root, skill_md, "");
        return outcome;
    }

    meta.command_key = normalize_skill_command_key(meta.name);
    if (meta.command_key.empty()) {
        LOG_WARN("[skills] Skill name '" + meta.name + "' has no usable command slug; skipping " + path_to_utf8(skill_md));
        outcome.issue = make_issue(SkillLoadFailure::UnusableName,
                                   "skill name '" + meta.name +
                                       "' has no characters usable in a "
                                       "slash command",
                                   dir, scan_root, skill_md, meta.name);
        return outcome;
    }

    std::string desc = get_string(fm, "description");
    if (desc.empty()) desc = first_non_empty_body_line(body);
    meta.description = sanitize_field(desc);

    // 可选触发条件:写明"什么时候该用这个 skill"。主键 whenToUse 与
    // claude-code 的 frontmatter 约定一致,snake_case 作别名。
    std::string when = get_string(fm, "whenToUse");
    if (when.empty()) when = get_string(fm, "when_to_use");
    meta.when_to_use = sanitize_field(when);

    meta.category = sanitize_field(derive_category(dir, scan_root), 256);
    meta.platforms = sanitize_fields(get_list(fm, "platforms"));

    // tags live under metadata.hermes.tags (hermes convention) OR metadata.tags
    if (const FrontmatterValue* tv = get_nested(fm, {"metadata", "hermes", "tags"})) {
        if (tv->is_list()) meta.tags = tv->list_value;
    }
    if (meta.tags.empty()) {
        if (const FrontmatterValue* tv = get_nested(fm, {"metadata", "tags"})) {
            if (tv->is_list()) meta.tags = tv->list_value;
        }
    }
    meta.tags = sanitize_fields(std::move(meta.tags));

    // Non-fatal metadata problems: the skill still loads (name from the
    // directory, description from the first body line) but the model gets a
    // degraded — or missing — trigger condition, which is exactly the class of
    // breakage users cannot otherwise see.
    if (shape == FrontmatterShape::Unterminated) {
        outcome.issue = make_issue(SkillLoadFailure::UnterminatedFrontmatter,
                                   "frontmatter opens with --- but is never "
                                   "closed; the whole file is treated as body",
                                   dir, scan_root, skill_md, meta.name);
    } else if (shape == FrontmatterShape::None) {
        outcome.issue = make_issue(SkillLoadFailure::MissingFrontmatter,
                                   "SKILL.md has no --- YAML frontmatter "
                                   "block",
                                   dir, scan_root, skill_md, meta.name);
    } else if (meta.description.empty()) {
        outcome.issue = make_issue(SkillLoadFailure::MissingDescription,
                                   "frontmatter has no description, so the "
                                   "model cannot tell when to use this skill",
                                   dir, scan_root, skill_md, meta.name);
    }

    outcome.meta = std::move(meta);
    return outcome;
}

} // namespace

const char* skill_load_failure_code(SkillLoadFailure failure) {
    switch (failure) {
        case SkillLoadFailure::Unreadable:              return "unreadable";
        case SkillLoadFailure::MissingName:             return "missing_name";
        case SkillLoadFailure::UnusableName:            return "unusable_name";
        case SkillLoadFailure::ParseError:              return "parse_error";
        case SkillLoadFailure::UnterminatedFrontmatter: return "unterminated_frontmatter";
        case SkillLoadFailure::MissingFrontmatter:      return "missing_frontmatter";
        case SkillLoadFailure::MissingDescription:      return "missing_description";
    }
    return "parse_error";
}

bool is_fatal_skill_load_failure(SkillLoadFailure failure) {
    switch (failure) {
        case SkillLoadFailure::Unreadable:
        case SkillLoadFailure::MissingName:
        case SkillLoadFailure::UnusableName:
        case SkillLoadFailure::ParseError:
            return true;
        case SkillLoadFailure::UnterminatedFrontmatter:
        case SkillLoadFailure::MissingFrontmatter:
        case SkillLoadFailure::MissingDescription:
            return false;
    }
    return true;
}

std::string current_platform_identifier() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

bool skill_matches_platform(const std::vector<std::string>& platforms) {
    if (platforms.empty()) return true;
    std::string current = current_platform_identifier();
    for (const auto& p : platforms) {
        std::string lower;
        lower.reserve(p.size());
        for (char c : p) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        if (lower == current) return true;
    }
    return false;
}

std::string normalize_skill_command_key(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc == ' ' || uc == '_') {
            out.push_back('-');
        } else if (std::isalnum(uc)) {
            out.push_back(static_cast<char>(std::tolower(uc)));
        } else if (c == '-') {
            out.push_back('-');
        }
        // else: drop silently (matches hermes behaviour)
    }
    // Collapse multiple hyphens and trim.
    std::string collapsed;
    collapsed.reserve(out.size());
    bool last_hyphen = false;
    for (char c : out) {
        if (c == '-') {
            if (last_hyphen) continue;
            last_hyphen = true;
        } else {
            last_hyphen = false;
        }
        collapsed.push_back(c);
    }
    while (!collapsed.empty() && collapsed.front() == '-') collapsed.erase(collapsed.begin());
    while (!collapsed.empty() && collapsed.back() == '-')  collapsed.pop_back();
    return collapsed;
}

SkillLoadOutcome inspect_skill_dir(const fs::path& dir,
                                   const fs::path& scan_root) noexcept {
    // Whatever a SKILL.md contains, discovery must degrade to "this one skill
    // is broken" — never to an exception escaping into the scan loop and, from
    // there, into the HTTP handler as a 500.
    try {
        return inspect_skill_dir_impl(dir, scan_root);
    } catch (const std::exception& e) {
        SkillLoadOutcome outcome;
        try {
            LOG_WARN("[skills] Failed to load " + path_to_utf8(dir / "SKILL.md") +
                     ": " + e.what());
            outcome.issue = make_issue(SkillLoadFailure::ParseError,
                                       std::string("failed to parse SKILL.md: ") + e.what(),
                                       dir, scan_root, dir / "SKILL.md", "");
        } catch (...) {
        }
        return outcome;
    } catch (...) {
        SkillLoadOutcome outcome;
        try {
            outcome.issue = make_issue(SkillLoadFailure::ParseError,
                                       "failed to parse SKILL.md: unknown error",
                                       dir, scan_root, dir / "SKILL.md", "");
        } catch (...) {
        }
        return outcome;
    }
}

std::optional<SkillMetadata> load_skill_from_dir(const fs::path& dir,
                                                 const fs::path& scan_root) {
    return inspect_skill_dir(dir, scan_root).meta;
}

} // namespace acecode
