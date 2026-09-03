#pragma once

#include <string>
#include <vector>
#include <filesystem>

namespace acecode {

struct SkillMetadata {
    std::string name;              // Normalized skill name (from frontmatter, or dir name)
    std::string command_key;       // Kebab-case command key (e.g. "my-plan" -> "/my-plan")
    std::string description;       // Short description shown in skills_list / /help
    std::string when_to_use;        // Optional trigger condition (frontmatter whenToUse / when_to_use)
    std::string category;          // First-level directory under skills root, or "" if flat
    std::filesystem::path skill_md_path; // Absolute path to SKILL.md
    std::filesystem::path skill_dir;     // Absolute path to the skill directory
    std::filesystem::path scan_root;     // The scan root this skill was discovered under
    std::vector<std::string> platforms;  // Empty = all platforms
    std::vector<std::string> tags;
};

// What is wrong with a SKILL.md on disk.
//
// A malformed skill used to vanish from the scan with only a log line, so the
// settings page showed nothing and the user had no way to tell "not installed"
// apart from "installed but broken" — and, worse, some malformed inputs threw
// on the way to nlohmann's dump(), turning the whole /api/skills response into
// a 500. Issues are now collected alongside the successful skills so surfaces
// can render them as「加载失败」/「配置异常」rows instead.
enum class SkillLoadFailure {
    // Fatal — the skill cannot be used.
    Unreadable,         // SKILL.md missing, empty, or the read failed
    MissingName,        // no usable name in frontmatter and none derivable from the dir
    UnusableName,       // name has no characters that survive slug normalization
    ParseError,         // an exception escaped while inspecting this file
    // Non-fatal — the skill still loads, but its metadata is degraded.
    UnterminatedFrontmatter, // opens with `---` but never closes it
    MissingFrontmatter,      // no `---` frontmatter block at all
    MissingDescription,      // nothing usable for the description / skill index
};

// Fatal issues drop the skill from the registry; non-fatal ones travel
// alongside a loaded skill.
bool is_fatal_skill_load_failure(SkillLoadFailure failure);

struct SkillLoadIssue {
    std::string name;    // Best-effort display name (frontmatter name, else dir name)
    SkillLoadFailure failure = SkillLoadFailure::ParseError;
    std::string detail;  // Human-readable reason, safe for UI display
    std::string category;
    std::filesystem::path skill_md_path;
    std::filesystem::path skill_dir;
    std::filesystem::path scan_root;

    bool fatal() const { return is_fatal_skill_load_failure(failure); }
};

// Stable, machine-readable code for a load failure (used as the REST
// `error_code` and by tests).
const char* skill_load_failure_code(SkillLoadFailure failure);

} // namespace acecode
