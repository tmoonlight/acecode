#include "session_host/auto_title_runner.hpp"
#include "session/session_manager.hpp"
#include "agent/agent_loop.hpp"
#include "utils/logger.hpp"
namespace acecode {
static std::optional<std::string> generate_title(ModelProfile profile,
    const std::string& text, const AppConfig& config) {
    auto provider = create_auto_title_provider(std::move(profile), config);
    if (!provider) return std::nullopt;
    return generate_auto_session_title(*provider, text, config);
}
AutoTitleRunner::AutoTitleRunner(const AppConfig& config, SessionManager& session,
    AgentLoop& agent, Applied applied, Generate generate)
    : config_(config), session_(session), agent_(agent), applied_(std::move(applied)),
      generate_(generate ? std::move(generate) : Generate(generate_title)) {}
AutoTitleRunner::~AutoTitleRunner() {
    shutting_down_.store(true);
    lifetime_.revoke();
    workers_.shutdown();
}
void AutoTitleRunner::stop() {
    shutting_down_.store(true);
    workers_.shutdown();
}
void AutoTitleRunner::maybe_start(const UserInput& input) {
    if (!config_.session_title.enabled) return;
    std::string text = visible_auto_title_input(input);
    if (text.empty()) return;
    const auto session_id = session_.ensure_active_session_id();
    if (session_id.empty()) return;
    auto attempt = session_.begin_auto_title_generation(std::move(text));
    if (attempt) start_attempt(session_id, std::move(*attempt));
}
void AutoTitleRunner::turn_finished(const std::string& status) {
    const auto session_id = session_.current_session_id();
    auto retry = session_.mark_auto_title_turn_finished(status);
    if (retry && !session_id.empty()) start_attempt(session_id, std::move(*retry));
}
void AutoTitleRunner::start_attempt(const std::string& session_id, std::string text) {
    if (shutting_down_.load() || session_id.empty()
        || session_.current_session_id() != session_id) return;
    auto profile = resolve_auto_title_profile(config_, session_.current_model_preset(), agent_.cwd());
    if (!profile) {
        auto retry = session_.finish_auto_title_generation_for_session(session_id, false);
        if (retry && !shutting_down_.load()) start_attempt(session_id, std::move(*retry));
        return;
    }
    if (shutting_down_.load()) return;
    workers_.spawn([ref = lifetime_.ref(*this), session_id,
                    text = std::move(text), profile = std::move(*profile)]() mutable {
        ref.with([&](AutoTitleRunner& runner) {
            runner.execute(session_id, std::move(text), std::move(profile));
        });
    });
}
void AutoTitleRunner::execute(std::string session_id, std::string text, ModelProfile profile) {
    bool applied = false;
    try {
        auto title = generate_(std::move(profile), text, config_);
        if (title && !title->empty()
            && session_.try_set_generated_session_title_for_session(session_id, *title)) {
            applied = true;
            if (session_.current_session_id() == session_id && applied_) applied_(session_id, *title);
        }
    } catch (const std::exception& e) {
        LOG_WARN("[tui] auto session title generation failed: " + std::string(e.what()));
    } catch (...) {
        LOG_WARN("[tui] auto session title generation failed");
    }
    auto retry = session_.finish_auto_title_generation_for_session(session_id, applied);
    if (retry && !shutting_down_.load()) start_attempt(session_id, std::move(*retry));
}
}
