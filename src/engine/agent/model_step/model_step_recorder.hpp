#pragma once

#include "agent/turn/turn_types.hpp"
#include <memory>
#include <string>

namespace acecode { class ToolExecutor; class EventDispatcher; class SessionManager; }
namespace acecode::agent {
class ModelStepRecorder {
public:
    ModelStepRecorder(ToolExecutor& tools, EventDispatcher& events)
        : tools_(tools), events_(events) {}
    void start(int step_index);
    void finish(int step_index, std::string reason, const TokenUsage& usage);
    void request(SessionManager* session, int step_index,
                 const std::shared_ptr<LlmProvider>& provider,
                 const ApiRequestBundle& bundle, int context_window);
    void response(SessionManager* session, int step_index,
                  const ProviderCallResult& result, const TokenUsage& usage,
                  std::string status);
    void first_output(SessionManager* session, int step_index,
                      int attempt, const std::string& channel);
private:
    ToolExecutor& tools_;
    EventDispatcher& events_;
};
} // namespace acecode::agent
