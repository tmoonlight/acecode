#include "command_bootstrap.hpp"
#include "command_registry.hpp"
#include "builtin_commands.hpp"
#include "opencode_command_registry.hpp"
#include "skill_commands.hpp"
#include "swarm_mode_command.hpp"
#include "utils/logger.hpp"

namespace acecode::tui {

void register_slash_commands(CommandRegistry& cmd_registry,
                                    SkillRegistry& skill_registry,
                                    const AppConfig& config,
                                    const std::string& working_dir) {
    register_builtin_commands(cmd_registry);
    // builtin_commands.cpp 已到行数基线,新内置命令从这里注册。
    register_swarm_mode_command(cmd_registry);
    auto command_keys = register_opencode_commands_tracked(
        cmd_registry, config, working_dir);
    if (!command_keys.empty()) {
        LOG_INFO("[commands] Registered " + std::to_string(command_keys.size()) +
                 " opencode command slash command(s)");
    }
    auto keys = register_skill_commands_tracked(cmd_registry, skill_registry);
    if (!keys.empty()) {
        LOG_INFO("[skills] Registered " + std::to_string(keys.size()) +
                 " skill slash command(s)");
    }
}


} // namespace acecode::tui
