#pragma once

#include "auth/xai_auth.hpp"
#include "openai_provider.hpp"

#include <string>

namespace acecode {

class GrokProvider : public OpenAiCompatProvider {
public:
    explicit GrokProvider(
        const std::string& model,
        ProviderRequestOptions request_options = {},
        GrokAuthConfig auth_config = {},
        int stream_timeout_ms = OpenAiConfig::kDefaultStreamTimeoutMs);

    ChatResponse chat(
        const std::vector<ChatMessage>& messages,
        const std::vector<ToolDef>& tools) override;

    void chat_stream(
        const std::vector<ChatMessage>& messages,
        const std::vector<ToolDef>& tools,
        const StreamCallback& callback,
        std::atomic<bool>* abort_flag = nullptr) override;

    std::string name() const override { return "grok"; }
    bool is_authenticated() override;
    bool authenticate() override;

private:
    std::string responses_url() const;

    GrokAuthConfig auth_config_;
    std::string agent_id_;
    std::string session_id_;
};

} // namespace acecode
