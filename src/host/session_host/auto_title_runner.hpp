#pragma once
#include "session_host/session_auto_title.hpp"
#include "utils/joining_thread.hpp"
#include "utils/lifetime_token.hpp"
#include <atomic>
#include <functional>
namespace acecode {
class SessionManager; class AgentLoop;
class AutoTitleRunner {
public:
    using Applied = std::function<void(const std::string& session_id, const std::string& title)>;
    using Generate = std::function<std::optional<std::string>(ModelProfile,
        const std::string& text, const AppConfig&)>;
    AutoTitleRunner(const AppConfig& config, SessionManager& session, AgentLoop& agent,
        Applied applied, Generate generate = {});
    ~AutoTitleRunner();
    AutoTitleRunner(const AutoTitleRunner&) = delete;
    AutoTitleRunner& operator=(const AutoTitleRunner&) = delete;
    void maybe_start(const UserInput& input);
    void turn_finished(const std::string& status);
    void stop();
private:
    void start_attempt(const std::string& session_id, std::string text);
    void execute(std::string session_id, std::string text, ModelProfile profile);
    const AppConfig& config_;
    SessionManager& session_;
    AgentLoop& agent_;
    const Applied applied_;
    const Generate generate_;
    std::atomic<bool> shutting_down_{false};
    ReapingThreadSet workers_;
    LifetimeToken lifetime_;
};
}
