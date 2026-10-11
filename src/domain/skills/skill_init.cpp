#include "skill_init.hpp"

#include "skill_registry.hpp"
#include "config/config.hpp"
#include "utils/encoding.hpp"
#include "utils/paths.hpp"
#include "utils/utf8_path.hpp"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <unordered_set>
#include <vector>

namespace acecode {

namespace {

namespace fs = std::filesystem;

// Expand before determining scope, so ${ENV} can name an absolute global root.
// Unknown variables must not accidentally become relative workspace paths.
std::optional<fs::path> expanded_scan_path(const std::string& raw) {
    std::string expanded;
    for (std::size_t pos = 0; pos < raw.size();) {
        if (raw.compare(pos, 2, "${") != 0) {
            expanded += raw[pos++];
            continue;
        }
        const auto end = raw.find('}', pos + 2);
        if (end == std::string::npos) return std::nullopt;
        const auto name = raw.substr(pos + 2, end - pos - 2);
        std::string value;
        if (name == "ACECODE_DATA_DIR") value = get_acecode_dir();
        else value = getenv_utf8(name.c_str());
        if (value.empty() && name == "XDG_CONFIG_HOME") value = expand_path("~/.config");
        if (value.empty() && name == "XDG_CACHE_HOME") value = expand_path("~/.cache");
        if (value.empty()) return std::nullopt;
        expanded += value;
        pos = end + 1;
    }
    expanded = expand_path(expanded);
    if (expanded.empty() || expanded.front() == '~' ||
        expanded.find("${") != std::string::npos) return std::nullopt;
    return path_from_utf8(expanded);
}

std::vector<fs::path> configured_scan_roots(
    const AppConfig& config, const std::string& working_dir,
    std::optional<bool> project_only = std::nullopt) {
    std::vector<fs::path> roots;
    const auto dirs = get_project_dirs_up_to_home(working_dir);
    for (const auto& raw : *config.skills.scan_paths) {
        const auto path = expanded_scan_path(raw);
        if (!path) continue;
        const bool project = !path->is_absolute();
        if (project_only && project != *project_only) continue;
        if (project) {
            // Reject drive-relative/root-relative paths on Windows: they depend
            // on process state, not the selected workspace.
            if (path->has_root_path()) continue;
            for (const auto& dir : dirs) {
                roots.push_back((path_from_utf8(dir) / *path).lexically_normal());
            }
        } else {
            roots.push_back(path->lexically_normal());
        }
    }
    return roots;
}

std::string env_or_expanded_path(const char* name, const std::string& fallback) {
    std::string value = getenv_utf8(name);
    if (!value.empty()) return value;
    return expand_path(fallback);
}

void append_opencode_config_skill_roots(std::vector<fs::path>& roots,
                                        const fs::path& base) {
    if (base.empty()) return;
    roots.emplace_back(base / "skills");
    roots.emplace_back(base / "skill");
}

void append_opencode_project_skill_roots(std::vector<fs::path>& roots,
                                         const fs::path& dir) {
    append_opencode_config_skill_roots(roots, dir / ".opencode");
    roots.emplace_back(dir / ".agents" / "skills");
    roots.emplace_back(dir / ".claude" / "skills");
}

void append_opencode_global_skill_roots(std::vector<fs::path>& roots) {
    const fs::path xdg_config =
        path_from_utf8(env_or_expanded_path("XDG_CONFIG_HOME", "~/.config"));
    append_opencode_config_skill_roots(roots, xdg_config / "opencode");

    const fs::path home_opencode = path_from_utf8(expand_path("~/.opencode"));
    append_opencode_config_skill_roots(roots, home_opencode);

    const std::string custom_config = getenv_utf8("OPENCODE_CONFIG_DIR");
    if (!custom_config.empty()) {
        append_opencode_config_skill_roots(roots, path_from_utf8(custom_config));
    }

    roots.emplace_back(path_from_utf8(expand_path("~/.agents/skills")));
    roots.emplace_back(path_from_utf8(expand_path("~/.claude/skills")));

    const fs::path xdg_cache =
        path_from_utf8(env_or_expanded_path("XDG_CACHE_HOME", "~/.cache"));
    roots.emplace_back(xdg_cache / "opencode" / "skills");
}

} // namespace

std::vector<std::string> default_skill_scan_paths(const AppConfig& config) {
    std::vector<std::string> paths = {".acecode/skills", ".agent/skills"};
    if (config.skills.reuse_opencode) {
        for (const char* path : {".opencode/skills", ".opencode/skill",
                                 ".agents/skills", ".claude/skills"}) {
            paths.emplace_back(path);
        }
    }
    paths.emplace_back("${ACECODE_DATA_DIR}/skills");
    paths.emplace_back("~/.agent/skills");
    if (config.skills.reuse_opencode) {
        for (const char* path : {"${XDG_CONFIG_HOME}/opencode/skills",
                                 "${XDG_CONFIG_HOME}/opencode/skill",
                                 "~/.opencode/skills", "~/.opencode/skill",
                                 "${OPENCODE_CONFIG_DIR}/skills",
                                 "${OPENCODE_CONFIG_DIR}/skill",
                                 "~/.agents/skills", "~/.claude/skills",
                                 "${XDG_CACHE_HOME}/opencode/skills"}) {
            paths.emplace_back(path);
        }
    }
    for (const auto& raw : config.skills.external_dirs) {
        // Legacy external dirs are global, even when written relatively.
        const auto expanded = expand_path(raw);
        if (expanded.empty()) continue;
        auto path = path_from_utf8(expanded);
        if (path.is_relative()) {
            std::error_code ec;
            const auto absolute = fs::absolute(path, ec);
            if (!ec) path = absolute;
        }
        paths.push_back(path_to_utf8(path));
    }
    return paths;
}

std::vector<fs::path> skill_scan_roots(
    const AppConfig& config, const std::string& working_dir) {
    if (config.skills.scan_paths) return configured_scan_roots(config, working_dir);
    auto roots = project_skill_scan_roots(config, working_dir);
    auto global = global_skill_scan_roots(config);
    roots.insert(roots.end(), global.begin(), global.end());
    return roots;
}

std::vector<std::filesystem::path> project_skill_scan_roots(
    const AppConfig& config, const std::string& working_dir) {
    if (config.skills.scan_paths) return configured_scan_roots(config, working_dir, true);
    std::vector<std::filesystem::path> roots;

    for (const auto& dir : get_project_dirs_up_to_home(working_dir)) {
        roots.emplace_back(path_from_utf8(dir) / ".acecode" / "skills");
    }
    for (const auto& dir : get_project_dirs_up_to_home(working_dir)) {
        roots.emplace_back(path_from_utf8(dir) / ".agent" / "skills");
    }
    if (config.skills.reuse_opencode) {
        for (const auto& dir : get_project_dirs_up_to_home(working_dir)) {
            append_opencode_project_skill_roots(roots, path_from_utf8(dir));
        }
    }
    return roots;
}

std::vector<std::filesystem::path> global_skill_scan_roots(const AppConfig& config) {
    if (config.skills.scan_paths) return configured_scan_roots(config, {}, false);
    std::vector<std::filesystem::path> roots;

    roots.emplace_back(path_from_utf8(get_acecode_dir()) / "skills");
    roots.emplace_back(path_from_utf8(expand_path("~/.agent/skills")));

    if (config.skills.reuse_opencode) {
        append_opencode_global_skill_roots(roots);
    }

    for (const auto& raw : config.skills.external_dirs) {
        std::string expanded = expand_path(raw);
        if (!expanded.empty()) roots.emplace_back(path_from_utf8(expanded));
    }
    return roots;
}

void initialize_skill_registry(SkillRegistry& skill_registry,
                                 const AppConfig& config,
                                 const std::string& working_dir,
                                 const std::vector<std::filesystem::path>&
                                     prepended_roots,
                                 const std::optional<std::vector<std::string>>&
                                     expert_allowed) {
    std::error_code ec;

    std::vector<std::filesystem::path> roots = prepended_roots;
    for (auto& root : skill_scan_roots(config, working_dir)) {
        roots.emplace_back(std::move(root));
    }

    // ~/.acecode/skills 是历史上自动创建的全局根,保持行为不变。
    std::filesystem::path default_acecode_skills_dir =
        path_from_utf8(get_acecode_dir()) / "skills";
    if (!std::filesystem::exists(default_acecode_skills_dir, ec)) {
        std::filesystem::create_directories(default_acecode_skills_dir, ec);
    }

    std::optional<std::unordered_set<std::string>> effective_allowed;
    std::unordered_set<std::string> effective_disabled(
        config.skills.disabled.begin(), config.skills.disabled.end());
    if (expert_allowed) {
        std::unordered_set<std::string> selected(
            expert_allowed->begin(), expert_allowed->end());

        // Package-local Skills are part of the selected expert package rather
        // than the installed/global catalog. Preserve every valid bundled
        // Skill even when it was not named in capabilities.skills.
        if (!prepended_roots.empty()) {
            SkillRegistry packaged;
            packaged.set_scan_roots(prepended_roots);
            packaged.set_disabled({});
            packaged.set_allowed(std::nullopt);
            packaged.scan();
            for (const auto& skill : packaged.list()) {
                selected.insert(skill.name);
            }
        }

        // Explicit expert selections have first priority over daemon-global
        // allow/disable defaults. They still pass through ordinary permission,
        // sandbox and runtime safety checks when tools execute.
        for (const auto& name : selected) {
            effective_disabled.erase(name);
        }
        effective_allowed = std::move(selected);
    } else if (config.skills.allowed) {
        effective_allowed = std::unordered_set<std::string>(
            config.skills.allowed->begin(), config.skills.allowed->end());
    }
    skill_registry.configure(
        std::move(roots),
        std::move(effective_disabled),
        std::move(effective_allowed));
    skill_registry.scan();
}

} // namespace acecode
