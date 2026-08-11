#pragma once

#include "llm_provider.hpp"
#include "provider_request_options.hpp"
#include "../config/config.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <string>
#include <utility>

namespace acecode {

class AnthropicProvider : public LlmProvider {
public:
    static constexpr const char* kDefaultBaseUrl = "https://api.anthropic.com/v1";
    static constexpr int kDefaultMaxTokens = 4096;

    AnthropicProvider(const std::string& base_url,
                      const std::string& api_key,
                      const std::string& model,
                      int stream_timeout_ms = OpenAiConfig::kDefaultStreamTimeoutMs,
                      std::map<std::string, std::string> request_headers = {},
                      ProviderRequestOptions request_options = {});

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

    std::string name() const override { return "anthropic"; }
    bool is_authenticated() override { return !api_key_.empty(); }
    std::string model() const override { return model_; }
    void set_model(const std::string& m) override { model_ = m; }

    void reconfigure(const std::string& base_url,
                     const std::string& api_key,
                     int stream_timeout_ms = OpenAiConfig::kDefaultStreamTimeoutMs,
                     std::map<std::string, std::string> request_headers = {},
                     ProviderRequestOptions request_options = {}) {
        base_url_ = normalize_base_url(base_url);
        api_key_ = api_key;
        stream_timeout_ms_ = stream_timeout_ms > 0
            ? stream_timeout_ms
            : OpenAiConfig::kDefaultStreamTimeoutMs;
        request_headers_ = std::move(request_headers);
        request_options_ = std::move(request_options);
    }

    const ProviderRequestOptions& request_options() const {
        return request_options_;
    }

    static std::string normalize_base_url(std::string value) {
        auto not_space = [](unsigned char c) { return !std::isspace(c); };
        value.erase(value.begin(),
                    std::find_if(value.begin(), value.end(), not_space));
        value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(),
                    value.end());
        while (!value.empty() && value.back() == '/') value.pop_back();
        return value.empty() ? std::string(kDefaultBaseUrl) : value;
    }

    nlohmann::json build_request_body(
        const std::vector<ChatMessage>& messages,
        const std::vector<ToolDef>& tools,
        bool stream = false
    ) const;

    static ChatResponse parse_response(const nlohmann::json& j);

    // Merge one Anthropic `usage` node into an accumulator, normalizing
    // prompt_tokens to the total input (cache reads/writes included) so
    // it matches the OpenAI-compatible contract. Safe to call repeatedly with
    // the same node, as the streaming path does.
    static void merge_usage(TokenUsage& usage, const nlohmann::json& node);

private:
    ChatResponse parse_sse_stream(
        const std::string& url,
        const nlohmann::json& body,
        const std::map<std::string, std::string>& extra_headers,
        const StreamCallback& callback,
        std::atomic<bool>* abort_flag
    );

    std::string base_url_;
    std::string api_key_;
    std::string model_;
    std::map<std::string, std::string> request_headers_;
    ProviderRequestOptions request_options_;
    int stream_timeout_ms_ = OpenAiConfig::kDefaultStreamTimeoutMs;
};

} // namespace acecode
