#pragma once

#include "openai_provider.hpp"

namespace acecode {

// API-key Responses transport. Shared Chat normalization still owns history
// repair and attachment routing; wire encoding and streaming are independent.
class OpenAiResponsesProvider : public OpenAiCompatProvider {
public:
    using OpenAiCompatProvider::OpenAiCompatProvider;

    std::string request_url() const override;

    ChatResponse chat(const std::vector<ChatMessage>& messages,
        const std::vector<ToolDef>& tools) override;
    ChatResponse chat_cancellable(const std::vector<ChatMessage>& messages,
        const std::vector<ToolDef>& tools,
        const std::atomic<bool>* abort_flag) override;
    ChatResponse chat_for_compaction(const std::vector<ChatMessage>& messages,
        const std::vector<ToolDef>& tools,
        const std::atomic<bool>* abort_flag) override;
    ChatResponse chat_with_options(const std::vector<ChatMessage>& messages,
        const std::vector<ToolDef>& tools, const ChatRequestOptions& options,
        const std::atomic<bool>* abort_flag) override;
    void chat_stream(const std::vector<ChatMessage>& messages,
        const std::vector<ToolDef>& tools, const StreamCallback& callback,
        std::atomic<bool>* abort_flag = nullptr) override;
    void chat_stream_with_options(const std::vector<ChatMessage>& messages,
        const std::vector<ToolDef>& tools, const ChatRequestOptions& options,
        const StreamCallback& callback,
        std::atomic<bool>* abort_flag = nullptr) override;

protected:
    bool supports_prompt_cache_key() const override;
    bool supports_compaction_tool_choice_none() const override { return true; }

private:
    nlohmann::json responses_body(const std::vector<ChatMessage>& messages,
        const std::vector<ToolDef>& tools, bool stream, bool for_compaction,
        const ChatRequestOptions* options, std::string& error) const;
    ChatResponse chat_impl(const std::vector<ChatMessage>& messages,
        const std::vector<ToolDef>& tools, const std::atomic<bool>* abort_flag,
        bool for_compaction, const ChatRequestOptions* options);
    void stream_impl(const std::vector<ChatMessage>& messages,
        const std::vector<ToolDef>& tools, const StreamCallback& callback,
        std::atomic<bool>* abort_flag, const ChatRequestOptions* options);
};

} // namespace acecode
