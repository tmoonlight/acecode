#pragma once

#include "llm/llm_provider.hpp"

#include <nlohmann/json.hpp>

#include <map>
#include <string>
#include <vector>

namespace acecode {

// The Chat-shaped input has already passed the existing attachment and history
// repair policy. Native output is replayed only when it agrees with that input.
nlohmann::json build_openai_responses_request(
    const nlohmann::json& chat_body,
    const std::vector<ChatMessage>* original_messages = nullptr,
    std::string* error = nullptr);

ChatResponse parse_openai_responses_response(const nlohmann::json& envelope);

class OpenAiResponsesStreamParser {
public:
    std::vector<StreamEvent> consume(const nlohmann::json& event);
    std::vector<StreamEvent> finish();

    bool terminal() const { return terminal_; }
    const ChatResponse& accumulated() const { return accumulated_; }

private:
    struct OutputState {
        nlohmann::json item = nlohmann::json::object();
        std::string arguments;
        bool arguments_seen = false;
        bool done = false;
    };

    bool identify_output(const nlohmann::json& event,
                         const nlohmann::json* item,
                         int& index);
    std::vector<StreamEvent> complete(const nlohmann::json& event);
    std::vector<StreamEvent> fail(const nlohmann::json& event,
                                  ProviderErrorKind kind,
                                  const std::string& message,
                                  bool retryable = false);

    ChatResponse accumulated_;
    std::map<int, OutputState> output_;
    std::map<std::string, int> index_by_id_;
    bool terminal_ = false;
};

} // namespace acecode
