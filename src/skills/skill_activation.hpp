#pragma once

#include "skill_metadata.hpp"

#include <string>
#include <vector>

namespace acecode {

class SkillRegistry;

// Result of applying Codex-compatible explicit Skill selection to one user
// turn. `prompt` is model-visible; `injected_skill_names` is ordered by the
// registry and contains each successfully-read Skill at most once.
struct ExplicitSkillPromptExpansion {
    std::string prompt;
    std::vector<std::string> injected_skill_names;
};

// Parse `$SkillName` and `[$SkillName](SKILL.md path)` mentions, ignore common
// environment variables, resolve linked paths before plain names, preserve
// registry order, and deduplicate by Skill path.
std::vector<SkillMetadata> collect_explicit_skill_mentions(
    const std::string& text,
    const SkillRegistry& registry);

// Render the user-role fragment Codex injects for an explicitly selected
// filesystem-backed Skill. `skill_contents` is the complete SKILL.md,
// including frontmatter.
std::string build_skill_instructions_fragment(
    const SkillMetadata& meta,
    const std::string& skill_contents);

// Append one `<skill>` fragment per explicit mention to the current user
// prompt. No mention means a byte-identical prompt. A missing/unreadable Skill
// is skipped without preventing other selected Skills from loading.
ExplicitSkillPromptExpansion inject_explicit_skill_instructions(
    const std::string& user_text,
    const SkillRegistry& registry);

// Map ACECode's `/<skill-name> [args]` UI selection onto the same linked
// mention syntax used by the explicit-selection parser. The AgentLoop then
// performs the actual full-SKILL.md injection for TUI, Web, and subagents.
std::string build_skill_invocation_hint(const SkillMetadata& meta,
                                         const std::string& args);

} // namespace acecode
