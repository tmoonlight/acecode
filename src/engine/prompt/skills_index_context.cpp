#include "system_prompt.hpp"
#include "skills/skill_registry.hpp"

#include <algorithm>
#include <sstream>

namespace acecode {

PromptContextBlock build_skills_index_context_prompt(
    const SkillRegistry* skills,
    int context_window_tokens,
    bool skill_view_available,
    bool skills_list_available,
    const std::set<std::string>* dormant_names,
    const std::vector<SkillGatedToolInfo>* gated_tools) {
    PromptContextBlock block;
    if (!skills) return block;

    auto all = skills->list();
    if (all.empty()) return block;
    // 门控内置工具的技能是产品能力入口:不进休眠,且单列一节不受目录截断影响。
    // 只列已安装的技能(被禁用 / 不存在时模型加载不到,列了反而误导)。
    std::vector<SkillGatedToolInfo> gated;
    if (gated_tools && skill_view_available) {
        for (const auto& tool : *gated_tools) {
            const bool installed = std::any_of(all.begin(), all.end(),
                [&](const SkillMetadata& s) { return s.name == tool.activation_skill; });
            if (installed) gated.push_back(tool);
        }
    }
    if (dormant_names && !dormant_names->empty()) {
        all.erase(
            std::remove_if(all.begin(), all.end(),
                           [&](const SkillMetadata& s) {
                               if (dormant_names->count(s.name) == 0) return false;
                               return std::none_of(gated.begin(), gated.end(),
                                   [&](const SkillGatedToolInfo& tool) {
                                       return tool.activation_skill == s.name;
                                   });
                           }),
            all.end());
        if (all.empty()) return block;
    }

    SkillIndexRenderResult rendered = format_skills_index_within_budget(
        all, skills_index_budget(context_window_tokens), skills_list_available);
    if (rendered.content.empty()) return block;

    std::ostringstream oss;
    oss << "<skills_instructions>\n"
        << "## Skills\n\n"
        << "A skill is a set of local instructions to follow that is stored in a "
        << "`SKILL.md` file. Below is the list of skills that can be used. Each "
        << "entry includes a name, description, and source locator.\n\n";
    if (!rendered.skill_root_lines.empty()) {
        oss << "### Skill roots\n\n";
        for (const auto& line : rendered.skill_root_lines) {
            oss << line << "\n";
        }
        oss << "\n";
    }
    oss << "### Available skills\n\n"
        << rendered.content << "\n\n";
    if (!gated.empty()) {
        oss << "### Skill-gated built-in tools\n\n"
            << "These built-in tools stay out of the tool list until their skill is "
            << "loaded with `skill_view`. When a request matches a hint below, load "
            << "that skill first (even if the list above shortened or omitted it), "
            << "then call the tool.\n";
        for (const auto& tool : gated) {
            oss << "- `" << tool.name << "` (load skill `" << tool.activation_skill << "`)";
            if (!tool.activation_hint.empty()) oss << ": " << tool.activation_hint;
            oss << "\n";
        }
        oss << "\n";
    }
    oss << "### How to use skills\n\n"
        << "- Discovery: The list above is the skills available in this session "
        << "(name + description + source locator). `file` entries live on the host "
        << "filesystem.";
    if (!rendered.skill_root_lines.empty()) {
        oss << " Expand short locators using the matching alias from `### Skill roots`.";
    }
    oss << "\n"
        << "- Trigger rules: If the user names a skill (with `$SkillName` or plain "
        << "text) OR the task clearly matches a skill's description shown above, "
        << "you must use that skill for that turn. Multiple mentions mean use them "
        << "all. Do not carry skills across turns unless re-mentioned.\n"
        << "- Explicit selection: `$SkillName`, a linked Skill mention, and "
        << "`/<skill-name>` cause ACECode to append a complete `<skill>` instruction "
        << "fragment to that user turn. When present, follow it directly and do not "
        << "load the same main `SKILL.md` again.\n"
        << "- Missing/blocked: If a named skill is not in the list or its source "
        << "cannot be read, say so briefly and continue with the best fallback.\n"
        << "- How to use a skill (progressive disclosure):\n";
    if (skill_view_available) {
        oss << "  1) After deciding to use a skill that was not already explicitly "
            << "injected, call `skill_view(name=\"...\")` and read its full "
            << "`SKILL.md` completely before taking task actions.\n"
            << "  2) When `SKILL.md` references another file, load it with "
            << "`skill_view` using the same skill name and referenced relative path.\n";
    } else {
        oss << "  1) After deciding to use a skill that was not already explicitly "
            << "injected, open the listed `SKILL.md` path and read it completely "
            << "before taking task actions.\n"
            << "  2) Resolve relative references against the directory containing "
            << "that `SKILL.md`.\n";
    }
    oss << "  3) If `SKILL.md` points to folders such as `references/`, use its "
        << "routing instructions to identify only the files required for the task.\n"
        << "  4) Prefer running or patching provided scripts instead of retyping "
        << "large code blocks.\n"
        << "  5) Reuse provided assets or templates instead of recreating them.\n"
        << "- Coordination and sequencing:\n"
        << "  - If multiple skills apply, choose the minimal set that covers the "
        << "request and state the order you will use them.\n"
        << "  - Announce which skills you are using and why. If you skip an obvious "
        << "skill, say why.\n"
        << "- Context hygiene:\n"
        << "  - Progressive disclosure applies to selecting relevant files, not "
        << "partially reading a selected instruction file.\n"
        << "  - Avoid deep reference-chasing: prefer files directly linked from "
        << "`SKILL.md` unless blocked.\n"
        << "  - When variants exist, select only the relevant references and note "
        << "the choice.\n"
        << "- Safety and fallback: If a skill cannot be applied cleanly, state the "
        << "issue, choose the best alternative, and continue.\n";
    if (skills_list_available) {
        oss << "- Call skills_list only when this bounded catalog omitted skills or "
            << "you need to double-check the full set.\n";
    }
    oss << "</skills_instructions>\n";

    block.content = oss.str();
    block.cache_key = "skills:" + prompt_component_hash(block.content);
    block.warning = rendered.report.warning_message();
    return block;
}

} // namespace acecode
