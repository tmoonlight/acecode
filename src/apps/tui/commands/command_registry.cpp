#include "command_registry.hpp"
#include "opencode_command_registry.hpp"
#include "skills/skill_activation.hpp"
#include "tui/commands/skill_commands.hpp"
#include "skills/skill_registry.hpp"
#include "utils/logger.hpp"
#include "utils/scope_exit.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <mutex>
#include <sstream>

namespace acecode {

namespace {

void notify_command_recognized(CommandContext& ctx,
                               const std::string& command_name) {
    if (!ctx.on_command_recognized) return;
    try {
        ctx.on_command_recognized(command_name);
    } catch (const std::exception& e) {
        LOG_WARN(std::string("[commands] command observer failed: ") + e.what());
    } catch (...) {
        LOG_WARN("[commands] command observer failed with unknown exception");
    }
}

} // namespace

void CommandRegistry::register_command(const SlashCommand& cmd) {
    // 同名重复注册:先把旧条目连同它的别名一起摘掉,避免残留别名指向新命令。
    unregister_command(cmd.name);

    SlashCommand stored = cmd;
    stored.aliases.clear();
    for (const auto& alias : cmd.aliases) {
        if (alias.empty() || alias == cmd.name) continue;
        if (std::find(stored.aliases.begin(), stored.aliases.end(), alias) !=
            stored.aliases.end()) {
            continue;
        }
        if (commands_.count(alias) != 0 || alias_to_name_.count(alias) != 0) {
            LOG_WARN("[commands] alias /" + alias + " of /" + cmd.name +
                     " collides with an existing slash command; alias skipped");
            continue;
        }
        alias_to_name_[alias] = cmd.name;
        stored.aliases.push_back(alias);
    }
    commands_[cmd.name] = std::move(stored);
}

bool CommandRegistry::unregister_command(const std::string& name) {
    auto it = commands_.find(name);
    if (it == commands_.end()) return false;
    for (const auto& alias : it->second.aliases) {
        auto alias_it = alias_to_name_.find(alias);
        if (alias_it != alias_to_name_.end() && alias_it->second == name) {
            alias_to_name_.erase(alias_it);
        }
    }
    commands_.erase(it);
    return true;
}

std::string CommandRegistry::resolve_name(const std::string& name) const {
    if (commands_.count(name) != 0) return name;
    auto it = alias_to_name_.find(name);
    if (it != alias_to_name_.end() && commands_.count(it->second) != 0) {
        return it->second;
    }
    return {};
}

const SlashCommand* CommandRegistry::find(const std::string& name) const {
    const std::string canonical = resolve_name(name);
    if (canonical.empty()) return nullptr;
    return &commands_.at(canonical);
}

bool CommandRegistry::dispatch(const std::string& input, CommandContext& ctx) {
    if (input.empty() || input[0] != '/') return false;
    ScopeExit publish([&ctx] {
        if (ctx.on_command_completed) ctx.on_command_completed();
    });

    // Parse: "/command args..."
    std::string trimmed = input.substr(1); // remove leading '/'
    std::string cmd_name;
    std::string args;

    size_t space_pos = trimmed.find(' ');
    if (space_pos == std::string::npos) {
        cmd_name = trimmed;
    } else {
        cmd_name = trimmed.substr(0, space_pos);
        args = trimmed.substr(space_pos + 1);
    }

    // 别名先解析回原名:使用计数、执行都归到原名那一条;用户实际敲的名字
    // 经 ctx.invoked_command_name 交给需要原样回显的处理函数(如 /side)。
    auto execute_resolved = [&]() {
        const std::string canonical = resolve_name(cmd_name);
        if (canonical.empty()) return false;
        // 拷贝一份再执行:命令本身可能重载注册表(如 /skills reload)。
        auto execute = commands_.at(canonical).execute;
        notify_command_recognized(ctx, canonical);
        std::string previous_invoked = std::move(ctx.invoked_command_name);
        ctx.invoked_command_name = cmd_name;
        ScopeExit restore_invoked([&ctx, &previous_invoked] {
            ctx.invoked_command_name = std::move(previous_invoked);
        });
        if (execute) execute(ctx, args);
        return true;
    };

    if (!has_command(cmd_name)) {
        if (!ctx.cwd.empty()) {
            reload_opencode_commands(*this, ctx.config, ctx.cwd);
            if (execute_resolved()) return true;
        }
        // 未命中:先就着磁盘重扫一次并重绑 skill 斜杠命令(会话进行中新写到磁盘的
        // skill 才能第一次敲就用上,且回填 commands_ 让后续自动补全/`/help` 列得出),
        // 然后再查一次。reload 走已测过的 reload_skill_commands(只动 skill key,
        // 内建命令不受影响)。command_registry 即 *this。
        if (ctx.skills) {
            reload_skill_commands(*this, *ctx.skills);
            if (execute_resolved()) return true;
        }
        if (ctx.skills) {
            auto skill = ctx.skills->find(cmd_name);
            if (skill) {
                notify_command_recognized(ctx, cmd_name);
                std::string message = build_skill_invocation_hint(*skill, args);
                {
                    std::lock_guard<std::mutex> lk(ctx.state.mu);
                    ctx.state.conversation.push_back(
                        {"system", "[Invoking skill: " + skill->name + "]", false});
                    ctx.state.chat_follow_tail = true;
                    if (ctx.state.is_waiting) {
                        ctx.state.pending_queue.push_back(message);
                        return true;
                    }
                    ctx.state.thinking_start_time = std::chrono::steady_clock::now();
                    ctx.state.streaming_output_chars = 0;
                    ctx.state.turn_completion_tokens_confirmed = 0;
                    ctx.state.is_waiting = true;
                }
                submit_user_text(ctx, message);
                return true;
            }
        }

        ctx.state.conversation.push_back(
            {"system", "Unknown command: /" + cmd_name + ". Type /help for available commands.", false});
        ctx.state.chat_follow_tail = true;
        return true; // consumed the input (even though command unknown)
    }

    execute_resolved();
    return true;
}

} // namespace acecode
