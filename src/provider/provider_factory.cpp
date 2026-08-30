#include "provider_factory.hpp"

#include "anthropic_provider.hpp"
#include "copilot_provider.hpp"
#include "grok_provider.hpp"
#include "openai_provider.hpp"
#include "vision_capability.hpp"
#include "../config/config.hpp"
#include "../config/model_provider_registry.hpp"
#include "../utils/logger.hpp"
#include "../utils/sha256.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <memory>
#include <random>
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

struct EffectiveProviderBuildPlan {
    std::string provider_kind;
    std::string base_url;
    std::string api_key;
    std::string model;
    int stream_timeout_ms = OpenAiConfig::kDefaultStreamTimeoutMs;
    std::map<std::string, std::string> request_headers;
    ProviderRequestOptions request_options;
    bool uses_connection_inputs = false;
    bool uses_request_options = false;
    bool applies_vision_routing = false;
    bool model_has_vision = false;
    bool any_vision_model_available = false;
};

ProviderRequestOptions request_options_from_entry(const ModelProfile& entry) {
    ProviderRequestOptions options;
    options.endpoint_mode = entry.endpoint_mode.value_or("base_url");
    options.max_output_tokens = entry.max_output_tokens;
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

void append_u64(std::string& out, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<char>((value >> shift) & 0xffu));
    }
}

void append_bool(std::string& out, bool value) {
    out.push_back(value ? '\x01' : '\x00');
}

void append_string(std::string& out, const std::string& value) {
    append_u64(out, static_cast<std::uint64_t>(value.size()));
    out.append(value);
}

template <typename T>
void append_optional_int(std::string& out, const std::optional<T>& value) {
    append_bool(out, value.has_value());
    if (value.has_value()) {
        append_u64(out, static_cast<std::uint64_t>(*value));
    }
}

void append_optional_bool(std::string& out,
                          const std::optional<bool>& value) {
    append_bool(out, value.has_value());
    if (value.has_value()) append_bool(out, *value);
}

void append_optional_string(std::string& out,
                            const std::optional<std::string>& value) {
    append_bool(out, value.has_value());
    if (value.has_value()) append_string(out, *value);
}

void append_reasoning(std::string& out,
                      const std::optional<ModelReasoningOptions>& reasoning) {
    append_bool(out, reasoning.has_value());
    if (!reasoning.has_value()) return;
    append_bool(out, reasoning->supported);
    append_bool(out, reasoning->mandatory);
    append_bool(out, reasoning->default_enabled);
    append_optional_bool(out, reasoning->enabled);
    append_u64(out, reasoning->supported_efforts.size());
    for (const auto& effort : reasoning->supported_efforts) {
        append_string(out, effort);
    }
    append_optional_string(out, reasoning->default_effort);
    append_optional_string(out, reasoning->effort);
    append_bool(out, reasoning->supports_max_tokens);
    append_optional_int(out, reasoning->max_tokens);
}

std::string canonical_plan_bytes(const EffectiveProviderBuildPlan& plan) {
    std::string out;
    append_string(out, "acecode-provider-plan-v1");
    append_string(out, plan.provider_kind);
    append_string(out, plan.model);
    append_bool(out, plan.uses_connection_inputs);
    if (plan.uses_connection_inputs) {
        append_string(out, plan.base_url);
        append_string(out, plan.api_key);
        append_u64(out, static_cast<std::uint64_t>(plan.stream_timeout_ms));
        append_u64(out, plan.request_headers.size());
        for (const auto& [name, value] : plan.request_headers) {
            append_string(out, name);
            append_string(out, value);
        }
    }
    append_bool(out, plan.uses_request_options);
    if (plan.uses_request_options) {
        append_string(out, plan.request_options.endpoint_mode);
        append_optional_int(out, plan.request_options.max_output_tokens);
        append_u64(out, static_cast<std::uint64_t>(
            plan.request_options.reasoning_protocol));
        append_reasoning(out, plan.request_options.reasoning);
    }
    append_bool(out, plan.applies_vision_routing);
    if (plan.applies_vision_routing) {
        append_bool(out, plan.model_has_vision);
        append_bool(out, plan.any_vision_model_available);
    }
    return out;
}

const std::string& process_fingerprint_salt() {
    static const std::string salt = [] {
        std::random_device random;
        std::string bytes(32, '\0');
        for (char& byte : bytes) {
            byte = static_cast<char>(random() & 0xffu);
        }
        return bytes;
    }();
    return salt;
}

std::string fingerprint_digest_for_plan(
    const EffectiveProviderBuildPlan& plan) {
    return sha256_hex(process_fingerprint_salt() + canonical_plan_bytes(plan));
}

std::optional<EffectiveProviderBuildPlan> effective_plan_from_entry(
    const ModelProfile& entry,
    const AppConfig* config) {
    EffectiveProviderBuildPlan plan;
    plan.provider_kind = entry.provider;
    plan.model = entry.model;

    if (entry.provider == "openai" || entry.provider == "anthropic") {
        plan.uses_connection_inputs = true;
        plan.uses_request_options = true;
        plan.base_url = entry.base_url;
        plan.api_key = entry.api_key;
        plan.stream_timeout_ms = entry.stream_timeout_ms.value_or(
            config ? config->openai.stream_timeout_ms
                   : OpenAiConfig::kDefaultStreamTimeoutMs);
        plan.request_headers = entry.request_headers;
        if (entry.provider == "openai" && plan.request_headers.empty() && config) {
            plan.request_headers = config->openai.request_headers;
        }
        plan.request_options = request_options_from_entry(entry);

        if (entry.provider == "anthropic" && entry.reasoning.has_value() &&
            entry.reasoning->max_tokens.has_value() &&
            entry.max_output_tokens.value_or(
                AnthropicProvider::kDefaultMaxTokens) <=
                *entry.reasoning->max_tokens) {
            LOG_WARN("[provider_factory] refusing Anthropic reasoning budget "
                     "that leaves no final-answer output budget");
            return std::nullopt;
        }
    } else if (entry.provider == "copilot") {
        if (!entry.base_url.empty() || !entry.api_key.empty() ||
            !entry.request_headers.empty() || entry.endpoint_mode.has_value() ||
            entry.max_output_tokens.has_value() || entry.reasoning.has_value()) {
            LOG_WARN("[provider_factory] refusing unsupported Copilot endpoint, "
                     "credential, output, or reasoning customization");
            return std::nullopt;
        }
    } else if (entry.provider == "grok") {
        if (!entry.base_url.empty() || !entry.api_key.empty() ||
            !entry.request_headers.empty() || entry.endpoint_mode.has_value() ||
            entry.max_output_tokens.has_value() || entry.reasoning.has_value() ||
            entry.stream_timeout_ms.has_value()) {
            LOG_WARN("[provider_factory] refusing unsupported Grok endpoint, "
                     "credential, timeout, output, or reasoning customization");
            return std::nullopt;
        }
    } else if (entry.provider == "codex") {
        LOG_WARN(std::string("[provider_factory] ") +
                 disabled_model_provider_reason(entry.provider));
        return std::nullopt;
    } else {
        if (entry.provider.empty()) {
            LOG_WARN("[provider_factory] no model provider configured");
        } else {
            LOG_WARN("[provider_factory] unknown model provider '" +
                     entry.provider + "'");
        }
        return std::nullopt;
    }

    plan.applies_vision_routing =
        entry.provider == "openai" || entry.provider == "copilot" ||
        entry.provider == "grok";
    if (plan.applies_vision_routing) {
        plan.model_has_vision = model_profile_has_vision(entry);
        plan.any_vision_model_available =
            config ? has_any_runtime_vision_model(*config) : false;
    }
    return plan;
}

std::shared_ptr<LlmProvider> construct_from_plan(
    const EffectiveProviderBuildPlan& plan) {
    std::shared_ptr<LlmProvider> provider;
    if (plan.provider_kind == "openai") {
        provider = std::make_shared<OpenAiCompatProvider>(
            plan.base_url,
            plan.api_key,
            plan.model,
            plan.stream_timeout_ms,
            plan.request_headers,
            plan.request_options);
    } else if (plan.provider_kind == "anthropic") {
        provider = std::make_shared<AnthropicProvider>(
            plan.base_url,
            plan.api_key,
            plan.model,
            plan.stream_timeout_ms,
            plan.request_headers,
            plan.request_options);
    } else if (plan.provider_kind == "copilot") {
        provider = std::make_shared<CopilotProvider>(plan.model);
    } else if (plan.provider_kind == "grok") {
        provider = std::make_shared<GrokProvider>(plan.model);
    }

    if (plan.applies_vision_routing) {
        if (auto compat = std::dynamic_pointer_cast<OpenAiCompatProvider>(provider)) {
            compat->set_vision_routing(plan.model_has_vision,
                                       plan.any_vision_model_available);
        }
    }
    return provider;
}

} // namespace

struct PreparedProviderConstruction::Impl {
    EffectiveProviderBuildPlan plan;
    ProviderConstructionFingerprint fingerprint;

    Impl(EffectiveProviderBuildPlan value,
         ProviderConstructionFingerprint token)
        : plan(std::move(value)), fingerprint(std::move(token)) {}
};

ProviderConstructionFingerprint::ProviderConstructionFingerprint(
    std::string digest)
    : digest_(std::move(digest)) {}

bool ProviderConstructionFingerprint::operator==(
    const ProviderConstructionFingerprint& other) const noexcept {
    return digest_ == other.digest_;
}

PreparedProviderConstruction::PreparedProviderConstruction(
    std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

PreparedProviderConstruction::PreparedProviderConstruction(
    PreparedProviderConstruction&&) noexcept = default;

PreparedProviderConstruction& PreparedProviderConstruction::operator=(
    PreparedProviderConstruction&&) noexcept = default;

PreparedProviderConstruction::~PreparedProviderConstruction() = default;

const ProviderConstructionFingerprint&
PreparedProviderConstruction::fingerprint() const noexcept {
    return impl_->fingerprint;
}

ProviderConstructionResult PreparedProviderConstruction::construct() const {
    return {
        construct_from_plan(impl_->plan),
        impl_->fingerprint,
    };
}

std::optional<PreparedProviderConstruction> prepare_provider_construction(
    const ModelProfile& entry,
    const AppConfig* config) {
    auto plan = effective_plan_from_entry(entry, config);
    if (!plan.has_value()) return std::nullopt;
    ProviderConstructionFingerprint fingerprint(
        fingerprint_digest_for_plan(*plan));
    return PreparedProviderConstruction(
        std::make_unique<PreparedProviderConstruction::Impl>(
            std::move(*plan), std::move(fingerprint)));
}

std::optional<ProviderConstructionResult> create_provider_construction(
    const ModelProfile& entry,
    const AppConfig* config) {
    auto prepared = prepare_provider_construction(entry, config);
    if (!prepared.has_value()) return std::nullopt;
    return prepared->construct();
}

std::shared_ptr<LlmProvider> create_provider_from_entry(
    const ModelProfile& entry,
    const AppConfig* config) {
    auto result = create_provider_construction(entry, config);
    return result.has_value() ? std::move(result->provider) : nullptr;
}

} // namespace acecode
