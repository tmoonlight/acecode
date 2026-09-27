#pragma once
#include "channels/gateway.hpp"
#include "channels/bridge.hpp"
#include "session/ask_user_question_prompter.hpp"
#include "utils/utf8_path.hpp"
#include <gtest/gtest.h>
#include <atomic>
#include <cstdlib>
#include <map>
#include <set>

namespace acecode::channels::test {
class Client : public SessionClient {
public:
    std::vector<SessionOptions> creates, resumes;
    std::vector<std::pair<std::string, UserInput>> inputs;
    std::vector<std::pair<std::string, PermissionDecision>> decisions;
    std::vector<std::string> aborts;
    std::map<std::string, std::map<SubscriptionId, EventListener>> listeners;
    std::set<std::string> answered;
    bool resume_ok = true, send_ok = true;
    std::function<void(const std::string&)> after_subscribe;
    std::vector<PendingQuestionRequestSnapshot> questions;
    std::uint64_t next = 1;
    std::string create_session(const SessionOptions& opts) override {
        creates.push_back(opts); return "channel-session-" + std::to_string(creates.size());
    }
    bool resume_session(const std::string&, const SessionOptions& opts) override { resumes.push_back(opts); return resume_ok; }
    std::vector<SessionInfo> list_sessions() override { return {}; }
    void destroy_session(const std::string&) override {}
    SubscriptionId subscribe(const std::string& id, EventListener listener, std::uint64_t) override {
        auto sub = next++; listeners[id][sub] = std::move(listener);
        if (after_subscribe) after_subscribe(id);
        return sub;
    }
    void unsubscribe(const std::string& id, SubscriptionId sub) override { listeners[id].erase(sub); }
    bool send_input(const std::string& id, const std::string& text) override { UserInput input; input.text = text; return send_input(id, input); }
    bool send_input(const std::string& id, const UserInput& input) override {
        if (!send_ok) return false;
        inputs.emplace_back(id, input);
        emit(id, SessionEventKind::Message, {{"role", "user"}, {"content", input.text}, {"metadata", input.metadata}});
        return true;
    }
    BuiltinCommandResult execute_builtin_command(const std::string&, const BuiltinCommandRequest&) override { return {}; }
    void respond_permission(const std::string& id, const PermissionDecision& decision) override {
        decisions.emplace_back(id, decision);
        emit(id, SessionEventKind::PermissionClosed, {{"request_id", decision.request_id}, {"choice", to_string(decision.choice)}});
    }
    QuestionResponseStatus respond_question(const std::string& id, const std::string& request,
                                            const AskUserQuestionResponse&) override {
        if (!answered.insert(request).second) return QuestionResponseStatus::Closed;
        emit(id, SessionEventKind::QuestionClosed, {{"request_id", request}, {"reason", "answered"}});
        return QuestionResponseStatus::Accepted;
    }
    std::optional<std::vector<PendingQuestionRequestSnapshot>> snapshot_pending_questions(const std::string&) override { return questions; }
    void abort(const std::string& id) override { aborts.push_back(id); }
    void emit(const std::string& id, SessionEventKind kind, const Json& payload) {
        auto copy = listeners[id];
        for (const auto& item : copy) item.second({kind, next++, 0, payload});
    }
};
class Home {
public:
#ifdef _WIN32
    static constexpr const char* name = "USERPROFILE";
#else
    static constexpr const char* name = "HOME";
#endif
    std::string previous;
    explicit Home(const std::filesystem::path& path) {
        if (const char* value = std::getenv(name)) previous = value;
        set(path_to_utf8(path));
    }
    ~Home() { set(previous); }
    static void set(const std::string& value) {
#ifdef _WIN32
        _putenv_s(name, value.c_str());
#else
        if (value.empty()) unsetenv(name); else setenv(name, value.c_str(), 1);
#endif
    }
};
inline std::filesystem::path temporary(const std::string& suffix) {
    return std::filesystem::path(testing::TempDir()) / ("acecode-channels-" + suffix + "-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
}
inline lsp::LspSpawnOptions fake_bridge(const std::filesystem::path&) {
    return {{"node", path_to_utf8(std::filesystem::path(__FILE__).parent_path() / "fake_bridge.mjs")}, {}, {}};
}
} // namespace acecode::channels::test
