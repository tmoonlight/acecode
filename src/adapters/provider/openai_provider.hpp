#pragma once

#include "llm/llm_provider.hpp"
#include "provider_request_options.hpp"
#include "config/config.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>

namespace acecode {

class OpenAiCompatProvider : public LlmProvider {
public:
    OpenAiCompatProvider(const std::string& base_url,
                         const std::string& api_key,
                         const std::string& model,
                         int stream_timeout_ms = OpenAiConfig::kDefaultStreamTimeoutMs,
                         std::map<std::string, std::string> request_headers = {},
                         ProviderRequestOptions request_options = {});

    ChatResponse chat(
        const std::vector<ChatMessage>& messages,
        const std::vector<ToolDef>& tools
    ) override { return OpenAiCompatProvider::chat_cancellable(messages, tools, nullptr); }

    ChatResponse chat_cancellable(
        const std::vector<ChatMessage>& messages,
        const std::vector<ToolDef>& tools,
        const std::atomic<bool>* abort_flag) override;

    void chat_stream(
        const std::vector<ChatMessage>& messages,
        const std::vector<ToolDef>& tools,
        const StreamCallback& callback,
        std::atomic<bool>* abort_flag = nullptr
    ) override;

    void set_prompt_cache_key(const std::string& key) override;
    bool supports_compaction_prefix_reuse() const override {
        // Copilot and Grok own different transport/authentication paths.
        return name() == "openai";
    }
    ChatResponse chat_for_compaction(
        const std::vector<ChatMessage>& messages,
        const std::vector<ToolDef>& tools,
        const std::atomic<bool>* abort_flag) override;
    ChatResponse chat_with_options(
        const std::vector<ChatMessage>& messages,
        const std::vector<ToolDef>& tools,
        const ChatRequestOptions& options,
        const std::atomic<bool>* abort_flag) override;
    void chat_stream_with_options(
        const std::vector<ChatMessage>& messages,
        const std::vector<ToolDef>& tools,
        const ChatRequestOptions& options,
        const StreamCallback& callback,
        std::atomic<bool>* abort_flag = nullptr) override;

    std::string name() const override { return "openai"; }
    bool is_authenticated() override { return true; }
    std::string model() const override { return model_; }
    void set_model(const std::string& m) override { model_ = m; }

    // 能力路由上下文(route-attachments-by-capability D5)。create_provider_from_entry
    // 在构造后调用:model_has_vision 来自 entry.capabilities,any_vision_model_available
    // 来自 has_any_runtime_vision_model(config)。daemon 切换模型时必须重新设置。
    // 默认 model_has_vision_=true 是 fail-open —— 未接线时维持旧行为(照发图片),
    // 不会因漏接线把视觉模型的图也剥掉。
    void set_vision_routing(bool model_has_vision, bool any_vision_model_available) {
        model_has_vision_ = model_has_vision;
        any_vision_model_available_ = any_vision_model_available;
    }
    bool model_has_vision() const { return model_has_vision_; }
    bool any_vision_model_available() const { return any_vision_model_available_; }
    bool supports_vision() const override { return model_has_vision_; }

    // 运行时切换同-provider 的 entry 时,base_url / api_key 可能也变了。
    // 调用方假定在持 provider_mu 锁内调用 —— 不再加内部锁。
    // 对应 openspec/changes/model-profiles 任务 4.4 与 design.md D4。
    void reconfigure(const std::string& base_url,
                     const std::string& api_key,
                     int stream_timeout_ms = OpenAiConfig::kDefaultStreamTimeoutMs,
                     std::map<std::string, std::string> request_headers = {},
                     ProviderRequestOptions request_options = {}) {
        request_options_ = std::move(request_options);
        base_url_ = normalize_endpoint(base_url, request_options_.endpoint_mode);
        api_key_ = api_key;
        stream_timeout_ms_ = stream_timeout_ms > 0
            ? stream_timeout_ms
            : OpenAiConfig::kDefaultStreamTimeoutMs;
        request_headers_ = std::move(request_headers);
    }

    const ProviderRequestOptions& request_options() const {
        return request_options_;
    }

    virtual std::string request_url() const {
        return request_options_.endpoint_mode == "full_url"
            ? base_url_
            : base_url_ + "/chat/completions";
    }

    // 容忍用户配置 base_url 时多打/少打尾部斜杠:统一裁掉所有尾部 '/',
    // 拼接端点时再补一个前导 '/'(见 chat / chat_stream 的 "/chat/completions")。
    // 否则 "http://host/v1/" + "/chat/completions" = ".../v1//chat/completions",
    // 不少自建网关对双斜杠返回 404 Not Found。前导/内部空白一并裁掉,避免
    // 复制粘贴带进来的空格污染 URL。static + public 便于单测直接覆盖。
    static std::string normalize_base_url(std::string value) {
        auto not_space = [](unsigned char c) { return !std::isspace(c); };
        value.erase(value.begin(),
                    std::find_if(value.begin(), value.end(), not_space));
        value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(),
                    value.end());
        while (!value.empty() && value.back() == '/') value.pop_back();
        return value;
    }

    static std::string normalize_endpoint(std::string value,
                                          const std::string& endpoint_mode) {
        if (endpoint_mode != "full_url") return normalize_base_url(std::move(value));
        auto not_space = [](unsigned char c) { return !std::isspace(c); };
        value.erase(value.begin(),
                    std::find_if(value.begin(), value.end(), not_space));
        value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(),
                    value.end());
        return value;
    }

protected:
    // Build the request JSON body (reusable by CopilotProvider)
    nlohmann::json build_request_body(
        const std::vector<ChatMessage>& messages,
        const std::vector<ToolDef>& tools,
        bool stream = false,
        bool for_compaction = false,
        const ChatRequestOptions* call_options = nullptr
    ) const;

    // Capability seams also let protocol adapters use an explicit policy;
    // the default implementation recognizes verified official endpoints only.
    virtual bool supports_prompt_cache_key() const;
    virtual bool supports_compaction_tool_choice_none() const;
    bool remove_rejected_prompt_cache_key(
        nlohmann::json& body, const std::string& url,
        int status_code, const std::string& error_body);

    // Parse a chat completions response JSON (reusable by CopilotProvider)
    static ChatResponse parse_response(const nlohmann::json& j);

    // Parse SSE stream chunks and call callback. Returns accumulated ChatResponse.
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
    bool model_has_vision_ = true;             // fail-open,见 set_vision_routing
    bool any_vision_model_available_ = false;

private:
    void apply_call_options(nlohmann::json& body,
        const std::vector<ToolDef>& tools, bool for_compaction,
        const ChatRequestOptions* call_options) const;
    ChatResponse chat_cancellable_impl(
        const std::vector<ChatMessage>& messages,
        const std::vector<ToolDef>& tools,
        const std::atomic<bool>* abort_flag,
        bool for_compaction,
        const ChatRequestOptions* call_options = nullptr);
    void chat_stream_impl(
        const std::vector<ChatMessage>& messages,
        const std::vector<ToolDef>& tools,
        const StreamCallback& callback,
        std::atomic<bool>* abort_flag,
        const ChatRequestOptions* call_options);
    // Leaf lock: copies request state only; never held during I/O/callbacks.
    mutable std::mutex prompt_cache_mu_;
    std::string prompt_cache_key_;
    std::set<std::pair<std::string, std::string>> unsupported_prompt_cache_scopes_;
};

// 把一条 ChatMessage 的 content_parts 转成 OpenAI-compatible content payload。
// 这是 image/file 附件路由的唯一收口点(OpenAI + Copilot 共用 build_request_body,
// stream / 非 stream 同源),按模型能力 gate 图片 part。导出到头文件以便单测直接
// 覆盖(route-attachments-by-capability tasks 1.4 / 1.6 / 1.9 / 1.10)。
//   - model_has_vision           : active 模型是否能看图。false 时图片降级为句柄文本。
//   - any_vision_model_available : 系统是否还有可用视觉模型,决定 fallback 文本措辞。
nlohmann::json openai_content_for_message(const ChatMessage& msg,
                                          bool model_has_vision,
                                          bool any_vision_model_available);

} // namespace acecode
