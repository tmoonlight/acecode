#include "skill_activation.hpp"

#include "skill_registry.hpp"
#include "../utils/logger.hpp"
#include "../utils/utf8_path.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace acecode {

namespace {

struct ToolMentions {
    std::unordered_set<std::string> paths;
    std::unordered_set<std::string> plain_names;
};

bool is_mention_name_char(unsigned char byte) {
    return (byte >= 'a' && byte <= 'z') ||
           (byte >= 'A' && byte <= 'Z') ||
           (byte >= '0' && byte <= '9') ||
           byte == '_' || byte == '-' || byte == ':';
}

bool is_common_env_var(const std::string& name) {
    std::string upper;
    upper.reserve(name.size());
    for (unsigned char ch : name) {
        upper.push_back(static_cast<char>(std::toupper(ch)));
    }
    static const std::unordered_set<std::string> kNames = {
        "PATH", "HOME", "USER", "SHELL", "PWD", "TMPDIR", "TEMP",
        "TMP", "LANG", "TERM", "XDG_CONFIG_HOME",
    };
    return kNames.count(upper) != 0;
}

bool is_non_skill_resource_path(const std::string& path) {
    return path.rfind("app://", 0) == 0 ||
           path.rfind("mcp://", 0) == 0 ||
           path.rfind("plugin://", 0) == 0;
}

std::string normalize_skill_path(std::string path) {
    constexpr const char* kSkillPrefix = "skill://";
    if (path.rfind(kSkillPrefix, 0) == 0) {
        path.erase(0, std::char_traits<char>::length(kSkillPrefix));
    }
    std::replace(path.begin(), path.end(), '\\', '/');
#if defined(_WIN32)
    for (char& ch : path) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        if (byte >= 'A' && byte <= 'Z') {
            ch = static_cast<char>(byte - 'A' + 'a');
        }
    }
#endif
    return path;
}

ToolMentions extract_tool_mentions(const std::string& text) {
    ToolMentions mentions;
    const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
    std::size_t index = 0;
    while (index < text.size()) {
        if (bytes[index] == '[' && index + 2 < text.size() &&
            bytes[index + 1] == '$' && is_mention_name_char(bytes[index + 2])) {
            const std::size_t name_start = index + 2;
            std::size_t name_end = name_start + 1;
            while (name_end < text.size() && is_mention_name_char(bytes[name_end])) {
                ++name_end;
            }
            if (name_end < text.size() && bytes[name_end] == ']') {
                std::size_t path_start = name_end + 1;
                while (path_start < text.size() &&
                       std::isspace(bytes[path_start])) {
                    ++path_start;
                }
                if (path_start < text.size() && bytes[path_start] == '(') {
                    std::size_t path_end = path_start + 1;
                    while (path_end < text.size() && bytes[path_end] != ')') {
                        ++path_end;
                    }
                    if (path_end < text.size()) {
                        std::size_t value_start = path_start + 1;
                        std::size_t value_end = path_end;
                        while (value_start < value_end &&
                               std::isspace(bytes[value_start])) {
                            ++value_start;
                        }
                        while (value_end > value_start &&
                               std::isspace(bytes[value_end - 1])) {
                            --value_end;
                        }
                        const std::string name =
                            text.substr(name_start, name_end - name_start);
                        const std::string path =
                            text.substr(value_start, value_end - value_start);
                        if (!path.empty() && !is_common_env_var(name) &&
                            !is_non_skill_resource_path(path)) {
                            mentions.paths.insert(normalize_skill_path(path));
                        }
                        index = path_end + 1;
                        continue;
                    }
                }
            }
        }

        if (bytes[index] != '$') {
            ++index;
            continue;
        }
        const std::size_t name_start = index + 1;
        if (name_start >= text.size() || !is_mention_name_char(bytes[name_start])) {
            ++index;
            continue;
        }
        std::size_t name_end = name_start + 1;
        while (name_end < text.size() && is_mention_name_char(bytes[name_end])) {
            ++name_end;
        }
        const std::string name = text.substr(name_start, name_end - name_start);
        if (!is_common_env_var(name)) mentions.plain_names.insert(name);
        index = name_end;
    }
    return mentions;
}

std::string strip_ascii_whitespace(const std::string& text) {
    std::size_t start = 0;
    while (start < text.size() &&
           std::isspace(static_cast<unsigned char>(text[start]))) {
        ++start;
    }
    std::size_t end = text.size();
    while (end > start &&
           std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return text.substr(start, end - start);
}

bool is_valid_mention_name(const std::string& name) {
    return !name.empty() && std::all_of(
        name.begin(), name.end(), [](unsigned char ch) {
            return is_mention_name_char(ch);
        });
}

} // namespace

std::vector<SkillMetadata> collect_explicit_skill_mentions(
    const std::string& text,
    const SkillRegistry& registry) {
    const ToolMentions mentions = extract_tool_mentions(text);
    if (mentions.paths.empty() && mentions.plain_names.empty()) return {};

    const auto skills = registry.list();
    std::vector<SkillMetadata> selected;
    selected.reserve(skills.size());
    std::unordered_set<std::string> selected_paths;

    // Linked paths have precedence and do not depend on a unique display name.
    for (const auto& skill : skills) {
        const std::string path = normalize_skill_path(
            path_to_utf8_generic(skill.skill_md_path));
        if (mentions.paths.count(path) == 0) continue;
        if (selected_paths.insert(path).second) selected.push_back(skill);
    }

    // SkillRegistry already applies first-seen name deduplication, so a name in
    // this snapshot is unambiguous. Iterate the registry rather than mention
    // order to match Codex's stable selection semantics.
    for (const auto& skill : skills) {
        if (mentions.plain_names.count(skill.name) == 0) continue;
        const std::string path = normalize_skill_path(
            path_to_utf8_generic(skill.skill_md_path));
        if (selected_paths.insert(path).second) selected.push_back(skill);
    }
    return selected;
}

std::string build_skill_instructions_fragment(
    const SkillMetadata& meta,
    const std::string& skill_contents) {
    std::string fragment;
    fragment.reserve(meta.name.size() + skill_contents.size() + 128);
    fragment += "<skill>\n<name>";
    fragment += meta.name;
    fragment += "</name>\n<path>";
    fragment += path_to_utf8_generic(meta.skill_md_path);
    fragment += "</path>\n";
    fragment += skill_contents;
    if (fragment.empty() || fragment.back() != '\n') fragment.push_back('\n');
    fragment += "</skill>";
    return fragment;
}

ExplicitSkillPromptExpansion inject_explicit_skill_instructions(
    const std::string& user_text,
    const SkillRegistry& registry) {
    ExplicitSkillPromptExpansion result;
    result.prompt = user_text;
    for (const auto& skill : collect_explicit_skill_mentions(user_text, registry)) {
        auto contents = registry.read_skill_text(skill.name);
        if (!contents.has_value()) {
            LOG_WARN("[skills] Failed to load explicitly selected Skill '" +
                     skill.name + "' at " +
                     path_to_utf8_generic(skill.skill_md_path));
            continue;
        }
        if (!result.prompt.empty()) result.prompt += "\n\n";
        result.prompt += build_skill_instructions_fragment(skill, *contents);
        result.injected_skill_names.push_back(skill.name);
    }
    return result;
}

std::string build_skill_invocation_hint(const SkillMetadata& meta,
                                         const std::string& args) {
    const std::string mention_name = is_valid_mention_name(meta.name)
        ? meta.name
        : meta.command_key;
    std::string prompt = "[$" + mention_name + "](" +
        path_to_utf8_generic(meta.skill_md_path) + ")";
    const std::string trimmed_args = strip_ascii_whitespace(args);
    if (!trimmed_args.empty()) prompt += "\n\n" + trimmed_args;
    return prompt;
}

} // namespace acecode
