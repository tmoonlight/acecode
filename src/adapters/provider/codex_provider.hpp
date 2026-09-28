#pragma once

#include "llm/llm_provider.hpp"

#include <string>
#include <vector>

namespace acecode {

namespace codex_detail {

// Flattens the conversation into the single text input a Codex app-server
// turn takes. User messages carry their `file` content parts as the same
// [Attached file reference] text the other providers send; a message with no
// text but with file parts is kept. Exposed for unit tests.
std::string build_codex_input_text(const std::vector<ChatMessage>& messages);

} // namespace codex_detail

class CodexProvider : public LlmProvider {
public:
    explicit CodexProvider(std::string model);

    ChatResponse chat(
        const std::vector<ChatMessage>& messages,
        const std::vector<ToolDef>& tools
    ) override;

    void chat_stream(
        const std::vector<ChatMessage>& messages,
        const std::vector<ToolDef>& tools,
        const StreamCallback& callback,
        std::atomic<bool>* abort_flag = nullptr
    ) override;

    std::string name() const override { return "codex"; }
    bool is_authenticated() override;
    bool supports_tool_free_chat() const override { return false; }
    bool authenticate() override { return is_authenticated(); }

    std::string model() const override { return model_; }
    void set_model(const std::string& m) override { model_ = m; }

private:
    std::string model_;
};

} // namespace acecode
