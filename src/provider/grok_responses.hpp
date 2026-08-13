#pragma once

#include "llm_provider.hpp"

#include <nlohmann/json.hpp>

#include <map>
#include <string>
#include <vector>

namespace acecode {

// Convert the normalized Chat Completions shape produced by
// OpenAiCompatProvider::build_request_body() to Grok Build Responses input.
// original_messages is optional and is used only to restore provider-native
// encrypted reasoning blocks saved on earlier Grok assistant messages.
nlohmann::json build_grok_responses_request(
    const nlohmann::json& chat_body,
    const std::vector<ChatMessage>* original_messages = nullptr,
    std::string* error = nullptr);

ChatResponse parse_grok_responses_response(const nlohmann::json& envelope);

class GrokResponsesStreamParser {
public:
    // Consume one decoded SSE data object and return newly produced agent-loop
    // events. Unknown Responses event types are ignored for forward compatibility.
    std::vector<StreamEvent> consume(const nlohmann::json& event);

    // Called when the transport reaches EOF/[DONE]. If a terminal Responses
    // event was never seen, returns a structured malformed-stream Error.
    std::vector<StreamEvent> finish();

    bool terminal() const { return terminal_; }
    const ChatResponse& accumulated() const { return accumulated_; }

private:
    struct ToolState {
        int index = -1;
        std::string item_id;
        ToolCall call;
        bool emitted = false;
    };

    ToolState& tool_state_for(const nlohmann::json& event,
                              const nlohmann::json* item = nullptr);
    void update_tool_state(ToolState& state, const nlohmann::json& item);
    void emit_tool_once(ToolState& state, std::vector<StreamEvent>& events);
    void capture_reasoning_item(const nlohmann::json& item);
    void merge_final_envelope(const nlohmann::json& envelope,
                              std::vector<StreamEvent>& events,
                              bool incomplete);
    std::vector<StreamEvent> fail(const nlohmann::json& event,
                                  ProviderErrorKind kind,
                                  const std::string& fallback);

    ChatResponse accumulated_;
    std::map<int, ToolState> tools_by_index_;
    std::map<std::string, int> tool_index_by_item_id_;
    bool terminal_ = false;
    bool usage_emitted_ = false;
    int next_tool_index_ = 0;
};

} // namespace acecode
