#include "provider_factory.hpp"
#include "anthropic_provider.hpp"
#include "openai_provider.hpp"
#include "copilot_provider.hpp"
#include "grok_provider.hpp"
#include "vision_capability.hpp"
#include "../config/config.hpp"
#include "../config/model_provider_registry.hpp"
#include "../utils/logger.hpp"

#include <algorithm>
#include <cctype>
#include <memory>
#include <string>
#include <utility>

namespace acecode {

namespace {

bool equals_ascii_ci(const std::string& left, const std::string& right) {
    if (left.size() != right.size()) return false;
    return std::equal(left.begin(), left.end(), right.begin(),
                      [](unsigned char a, unsigned char b) {
                          return std::tolower(a) == std::tolower(b);
                      });
}

ProviderRequestOptions request_options_from_entry(const ModelProfile& entry) {
    ProviderRequestOptions options;
    options.endpoint_mode = entry.endpoint_mode.value_or("base_url");
    options.max_output_tokens = entry.max_output_tokens;
    if (entry.capabilities_source.has_value()) {
        options.tools_enabled = std::find(entry.capabilities.begin(),
                                          entry.capabilities.end(),
                                          "tool_use") != entry.capabilities.end();
    }
    options.reasoning = entry.reasoning;
    if (entry.provider == "anthropic" && entry.reasoning.has_value()) {
        options.reasoning_protocol = ReasoningWireProtocol::Anthropic;
    } else if (entry.provider == "openai" && entry.reasoning.has_value() &&
               entry.models_dev_provider_id.has_value() &&
               equals_ascii_ci(*entry.models_dev_provider_id, "openrouter")) {
        options.reasoning_protocol = ReasoningWireProtocol::OpenRouter;
    }
    return options;
}

} // namespace

std::shared_ptr<LlmProvider> create_provider_from_entry(const ModelProfile& entry,
                                                        const AppConfig* config) {
    std::shared_ptr<LlmProvider> provider;
    ProviderRequestOptions request_options = request_options_from_entry(entry);
    if (entry.provider == "openai") {
        int stream_timeout_ms = entry.stream_timeout_ms.value_or(
            config ? config->openai.stream_timeout_ms
                   : OpenAiConfig::kDefaultStreamTimeoutMs);
        auto request_headers = entry.request_headers;
        if (request_headers.empty() && config) {
            request_headers = config->openai.request_headers;
        }
        provider = std::make_shared<OpenAiCompatProvider>(
            entry.base_url,
            entry.api_key,
            entry.model,
            stream_timeout_ms,
            std::move(request_headers),
            std::move(request_options)
        );
    } else if (entry.provider == "anthropic") {
        if (entry.reasoning.has_value()) {
            if (entry.reasoning->max_tokens.has_value() &&
                entry.max_output_tokens.value_or(
                    AnthropicProvider::kDefaultMaxTokens) <=
                    *entry.reasoning->max_tokens) {
                LOG_WARN("[provider_factory] refusing Anthropic reasoning budget "
                         "that leaves no final-answer output budget");
                return nullptr;
            }
        }
        int stream_timeout_ms = entry.stream_timeout_ms.value_or(
            config ? config->openai.stream_timeout_ms
                   : OpenAiConfig::kDefaultStreamTimeoutMs);
        provider = std::make_shared<AnthropicProvider>(
            entry.base_url,
            entry.api_key,
            entry.model,
            stream_timeout_ms,
            entry.request_headers,
            std::move(request_options)
        );
    } else if (entry.provider == "copilot") {
        if (!entry.base_url.empty() || !entry.api_key.empty() ||
            !entry.request_headers.empty() || entry.endpoint_mode.has_value() ||
            entry.max_output_tokens.has_value() || entry.reasoning.has_value()) {
            LOG_WARN("[provider_factory] refusing unsupported Copilot endpoint, "
                     "credential, output, or reasoning customization");
            return nullptr;
        }
        // Copilot endpoint/auth/output/reasoning options are rejected by schema
        // validation. The authoritative tool capability remains meaningful.
        ProviderRequestOptions copilot_options;
        copilot_options.tools_enabled = request_options.tools_enabled;
        provider = std::make_shared<CopilotProvider>(
            entry.model, std::move(copilot_options));
    } else if (entry.provider == "grok") {
        if (!entry.base_url.empty() || !entry.api_key.empty() ||
            !entry.request_headers.empty() || entry.endpoint_mode.has_value() ||
            entry.max_output_tokens.has_value() || entry.reasoning.has_value() ||
            entry.stream_timeout_ms.has_value()) {
            LOG_WARN("[provider_factory] refusing unsupported Grok endpoint, "
                     "credential, timeout, output, or reasoning customization");
            return nullptr;
        }
        ProviderRequestOptions grok_options;
        grok_options.tools_enabled = request_options.tools_enabled;
        provider = std::make_shared<GrokProvider>(
            entry.model, std::move(grok_options));
    } else if (entry.provider == "codex") {
        LOG_WARN(std::string("[provider_factory] ") +
                 disabled_model_provider_reason(entry.provider));
        return nullptr;
    } else {
        if (entry.provider.empty()) {
            LOG_WARN("[provider_factory] no model provider configured");
        } else {
            LOG_WARN("[provider_factory] unknown model provider '" + entry.provider + "'");
        }
        return nullptr;
    }

    // 能力路由上下文(route-attachments-by-capability D5)。OpenAI 与 Copilot
    // 都派生自 OpenAiCompatProvider,共用 build_request_body 的图片 gate。
    if (auto compat = std::dynamic_pointer_cast<OpenAiCompatProvider>(provider)) {
        const bool model_has_vision = model_profile_has_vision(entry);
        const bool any_vision = config ? has_any_runtime_vision_model(*config) : false;
        compat->set_vision_routing(model_has_vision, any_vision);
    }
    return provider;
}

} // namespace acecode
