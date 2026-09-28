#pragma once

#include "side_chat.hpp"
#include "llm/llm_provider.hpp"
#include "utils/joining_thread.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace acecode::agent {

// One request per thread. Context is copied under a leaf mutex; provider calls
// and callbacks run outside it. stop_requests suppresses pending callbacks,
// join waits for admitted callbacks and requests before dependencies disappear.
class SideQuestionService {
public:
    using ProviderAccessor = std::function<std::shared_ptr<LlmProvider>()>;
    using Callback = std::function<void(SideQuestionResult)>;
    explicit SideQuestionService(ProviderAccessor provider);
    ~SideQuestionService();
    void publish(const std::vector<ChatMessage>& messages);
    std::vector<ChatMessage> snapshot() const;
    SideQuestionResult ask(const std::string& question);
    bool ask_async(std::string question, Callback callback);
    SideChatResult stream(const std::string& question,
                          const std::vector<SideChatMessage>& history,
                          SideChatCancellation& cancellation,
                          const SideChatStreamCallback& callback);
    void stop_requests();
    void join();

private:
    struct State;
    static SideQuestionResult ask(const std::shared_ptr<State>& state,
                                  const std::string& question);
    // Shared only with outstanding requests, which may be reaped concurrently.
    std::shared_ptr<State> state_;
    ReapingThreadSet threads_; // joins before request state is destroyed
};

} // namespace acecode::agent
