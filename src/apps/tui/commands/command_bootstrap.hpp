#pragma once
#include <string>
namespace acecode { class CommandRegistry; class SkillRegistry; struct AppConfig; }
namespace acecode::tui {
void register_slash_commands(CommandRegistry& commands, SkillRegistry& skills,
    const AppConfig& config, const std::string& working_dir);
}
