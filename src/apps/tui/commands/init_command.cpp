#include "init_command.hpp"

#include "config/config.hpp"
#include "prompt/init_prompt.hpp"
#include "utils/utf8_path.hpp"

#include <filesystem>
#include <fstream>
#include <mutex>

namespace fs = std::filesystem;

namespace acecode {

namespace {

void emit(CommandContext& ctx, const std::string& msg) {
    std::lock_guard<std::mutex> lk(ctx.state.mu);
    ctx.state.conversation.push_back({"system", msg, false});
    ctx.state.chat_follow_tail = true;
}

void cmd_init(CommandContext& ctx, const std::string& /*args*/) {
    fs::path cwd = path_from_utf8(ctx.agent_loop.cwd());
    std::error_code ec;
    bool agent_exists = fs::exists(cwd / "AGENTS.md", ec);

    if (!has_usable_init_provider(ctx.config)) {
        // Offline fallback: write the static skeleton, same refuse-on-exists
        // behavior as the pre-LLM implementation. The skeleton helper already
        // tolerates the case where neither legacy file is present.
        fs::path target = cwd / "AGENTS.md";
        if (agent_exists) {
            emit(ctx,
                 "AGENTS.md already exists at " + path_to_utf8_generic(target) +
                 " — no model is configured, so /init cannot propose "
                 "improvements. Edit it by hand, or run /configure first and "
                 "re-run /init to get an LLM-driven improvement pass.");
            return;
        }

        std::ofstream ofs(target, std::ios::binary);
        if (!ofs.is_open()) {
            emit(ctx,
                 "Failed to open " + path_to_utf8_generic(target) +
                 " for writing.");
            return;
        }
        ofs << build_agent_md_skeleton(cwd);
        emit(ctx,
             "Created " + path_to_utf8_generic(target) +
             " (offline skeleton — no model is configured, run /configure to "
             "get a filled-in version).");
        return;
    }

    // LLM-driven path: build the prompt and submit it through the agent loop.
    std::string prompt = build_init_prompt(cwd);

    const std::string ack =
        "[Invoking /init — analyzing codebase and authoring AGENTS.md...]";

    {
        std::lock_guard<std::mutex> lk(ctx.state.mu);
        ctx.state.conversation.push_back({"system", ack, false});
        ctx.state.chat_follow_tail = true;
        if (ctx.state.is_waiting) {
            ctx.state.pending_queue.push_back(prompt);
            return;
        }
        // 新一轮等待：提前把计时/计数字段重置，否则 on_busy_changed 会因为
        // is_waiting 已为 true 跳过它的重置块，thinking_start_time 停在 0
        // 会让底部 chip 秒数巨大。
        ctx.state.thinking_start_time = std::chrono::steady_clock::now();
        ctx.state.streaming_output_chars = 0;
        ctx.state.turn_completion_tokens_confirmed = 0;
        ctx.state.is_waiting = true;
    }

    submit_user_text(ctx, prompt);
}

} // namespace

// Mirrors the preconditions AgentLoop relies on when calling chat_stream, not a
// comprehensive auth probe — false positives just reach AgentLoop which
// surfaces the HTTP error normally. Rationale in design.md D4.
bool has_usable_init_provider(const AppConfig& cfg) {
    if (cfg.provider == "copilot") {
        // Background token refresh handles validity; treat as usable if chosen.
        return true;
    }
    if (cfg.provider == "codex") return false;
    if (cfg.provider == "openai") {
        return !cfg.openai.api_key.empty();
    }
    if (cfg.provider == "anthropic") {
        for (const auto& entry : cfg.saved_models) {
            if (entry.provider == "anthropic" &&
                entry.name == cfg.default_model_name &&
                !entry.api_key.empty()) {
                return true;
            }
        }
        for (const auto& entry : cfg.saved_models) {
            if (entry.provider == "anthropic" && !entry.api_key.empty()) {
                return true;
            }
        }
    }
    return false;
}

void register_init_command(CommandRegistry& registry) {
    registry.register_command(
        {"init",
         "Analyze this codebase and generate (or improve) AGENTS.md",
         cmd_init});
}

} // namespace acecode
