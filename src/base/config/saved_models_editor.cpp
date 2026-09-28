// src/config/saved_models_editor.cpp
#include "saved_models_editor.hpp"

#include "model_provider_registry.hpp"
#include "request_headers.hpp"

#include <algorithm>
#include <cctype>
#include <set>

namespace acecode {

const char* to_string(SavedModelEditError e) {
    switch (e) {
        case SavedModelEditError::OK: return "OK";
        case SavedModelEditError::INVALID_NAME: return "INVALID_NAME";
        case SavedModelEditError::RESERVED_NAME: return "RESERVED_NAME";
        case SavedModelEditError::NAME_TAKEN: return "NAME_TAKEN";
        case SavedModelEditError::UNKNOWN_PROVIDER: return "UNKNOWN_PROVIDER";
        case SavedModelEditError::PROVIDER_DISABLED: return "PROVIDER_DISABLED";
        case SavedModelEditError::MISSING_MODEL: return "MISSING_MODEL";
        case SavedModelEditError::MISSING_BASE_URL: return "MISSING_BASE_URL";
        case SavedModelEditError::INVALID_API_KEY: return "INVALID_API_KEY";
        case SavedModelEditError::INVALID_CONTEXT_WINDOW:
            return "INVALID_CONTEXT_WINDOW";
        case SavedModelEditError::INVALID_STREAM_TIMEOUT:
            return "INVALID_STREAM_TIMEOUT";
        case SavedModelEditError::INVALID_CAPABILITY:
            return "INVALID_CAPABILITY";
        case SavedModelEditError::INVALID_REQUEST_HEADER:
            return "INVALID_REQUEST_HEADER";
        case SavedModelEditError::INVALID_ENDPOINT_MODE:
            return "INVALID_ENDPOINT_MODE";
        case SavedModelEditError::INVALID_MAX_OUTPUT_TOKENS:
            return "INVALID_MAX_OUTPUT_TOKENS";
        case SavedModelEditError::INVALID_CAPABILITIES_SOURCE:
            return "INVALID_CAPABILITIES_SOURCE";
        case SavedModelEditError::INVALID_REASONING: return "INVALID_REASONING";
        case SavedModelEditError::INVALID_CREDENTIAL_SOURCE:
            return "INVALID_CREDENTIAL_SOURCE";
        case SavedModelEditError::CREDENTIAL_CONFLICT:
            return "CREDENTIAL_CONFLICT";
        case SavedModelEditError::UNSUPPORTED_MODEL_OPTION:
            return "UNSUPPORTED_MODEL_OPTION";
        case SavedModelEditError::NOT_FOUND: return "NOT_FOUND";
        case SavedModelEditError::IN_USE_AS_DEFAULT: return "IN_USE_AS_DEFAULT";
    }
    return "UNKNOWN";
}

namespace {

bool valid_capability_tag(const std::string& tag) {
    if (tag.empty()) return false;
    for (unsigned char ch : tag) {
        if (std::iscntrl(ch)) return false;
    }
    return true;
}

bool has_reasoning_capability(const ModelProfile& profile) {
    return std::find(profile.capabilities.begin(), profile.capabilities.end(),
                     "reasoning") != profile.capabilities.end();
}

bool field_supplied(bool flag, bool has_nondefault_value) {
    return flag || has_nondefault_value;
}

SavedModelEditError validate_draft_shape(const SavedModelDraft& d) {
    if (d.name.empty()) return SavedModelEditError::INVALID_NAME;
    if (d.name.front() == '(') return SavedModelEditError::RESERVED_NAME;
    if (!is_known_model_provider(d.provider)) {
        return SavedModelEditError::UNKNOWN_PROVIDER;
    }
    if (!is_runtime_model_provider_enabled(d.provider)) {
        return SavedModelEditError::PROVIDER_DISABLED;
    }
    if (d.model.empty()) return SavedModelEditError::MISSING_MODEL;
    if (d.context_window.has_value() && *d.context_window < 0) {
        return SavedModelEditError::INVALID_CONTEXT_WINDOW;
    }
    if (d.stream_timeout_ms.has_value() && *d.stream_timeout_ms < 0) {
        return SavedModelEditError::INVALID_STREAM_TIMEOUT;
    }
    if (d.max_output_tokens.has_value() && *d.max_output_tokens < 0) {
        return SavedModelEditError::INVALID_MAX_OUTPUT_TOKENS;
    }
    std::set<std::string> seen_capabilities;
    for (const auto& tag : d.capabilities) {
        if (!valid_capability_tag(tag) ||
            !seen_capabilities.insert(tag).second) {
            return SavedModelEditError::INVALID_CAPABILITY;
        }
    }
    if (d.endpoint_mode.has_value() &&
        *d.endpoint_mode != "base_url" &&
        *d.endpoint_mode != "full_url") {
        return SavedModelEditError::INVALID_ENDPOINT_MODE;
    }
    if (d.capabilities_source.has_value() &&
        *d.capabilities_source != "catalog" &&
        *d.capabilities_source != "manual") {
        return SavedModelEditError::INVALID_CAPABILITIES_SOURCE;
    }
    if (!d.request_headers.empty()) {
        if (d.provider != "openai" && d.provider != "anthropic") {
            return SavedModelEditError::INVALID_REQUEST_HEADER;
        }
        std::string error;
        if (!validate_request_headers(d.request_headers, error)) {
            return SavedModelEditError::INVALID_REQUEST_HEADER;
        }
    }
    const bool key_supplied = d.api_key_supplied || !d.api_key.empty();
    const bool source_supplied = d.credential_source_name.has_value();
    if ((d.clear_api_key && (key_supplied || source_supplied)) ||
        (key_supplied && source_supplied)) {
        return SavedModelEditError::CREDENTIAL_CONFLICT;
    }
    if (source_supplied && d.credential_source_name->empty()) {
        return SavedModelEditError::INVALID_CREDENTIAL_SOURCE;
    }

    if (d.provider == "copilot" || d.provider == "grok") {
        if (field_supplied(d.base_url_supplied, !d.base_url.empty()) ||
            key_supplied || d.clear_api_key || source_supplied ||
            field_supplied(d.request_headers_supplied,
                           !d.request_headers.empty()) ||
            field_supplied(d.endpoint_mode_supplied,
                           d.endpoint_mode.has_value()) ||
            field_supplied(d.max_output_tokens_supplied,
                           d.max_output_tokens.has_value()) ||
            field_supplied(d.reasoning_supplied, d.reasoning.has_value()) ||
            (d.provider == "grok" &&
             field_supplied(d.stream_timeout_ms_supplied,
                            d.stream_timeout_ms.has_value()))) {
            return SavedModelEditError::UNSUPPORTED_MODEL_OPTION;
        }
    }
    if (d.endpoint_mode == "full_url" &&
        (d.provider != "openai" || d.models_dev_provider_id.has_value())) {
        return SavedModelEditError::UNSUPPORTED_MODEL_OPTION;
    }
    return SavedModelEditError::OK;
}

void reset_provider_specific_fields(ModelProfile& profile) {
    profile.base_url.clear();
    profile.api_key.clear();
    profile.models_dev_provider_id.reset();
    profile.endpoint_mode.reset();
    profile.max_output_tokens.reset();
    profile.capabilities_source.reset();
    profile.reasoning.reset();
    profile.request_headers.clear();
}

ModelProfile merge_profile(const SavedModelDraft& d,
                           const ModelProfile* existing) {
    ModelProfile profile = existing ? *existing : ModelProfile{};
    const bool provider_changed = existing && existing->provider != d.provider;
    if (provider_changed) reset_provider_specific_fields(profile);

    profile.name = d.name;
    profile.provider = d.provider;
    profile.model = d.model;
    profile.readonly = false;

    const bool is_new = existing == nullptr;
    if (is_new || d.base_url_supplied || !d.base_url.empty()) {
        profile.base_url = d.base_url;
    }
    if (is_new || d.models_dev_provider_id_supplied ||
        d.models_dev_provider_id.has_value()) {
        profile.models_dev_provider_id = d.models_dev_provider_id;
    }
    if (is_new || d.context_window_supplied || d.context_window.has_value()) {
        profile.context_window = d.context_window.has_value() &&
                                 *d.context_window > 0
            ? d.context_window
            : std::nullopt;
    }
    if (is_new || d.stream_timeout_ms_supplied ||
        d.stream_timeout_ms.has_value()) {
        profile.stream_timeout_ms = d.stream_timeout_ms.has_value() &&
                                    *d.stream_timeout_ms > 0
            ? d.stream_timeout_ms
            : std::nullopt;
    }
    if (is_new || d.capabilities_supplied || !d.capabilities.empty()) {
        profile.capabilities = d.capabilities;
    }
    if (is_new || d.endpoint_mode_supplied || d.endpoint_mode.has_value()) {
        profile.endpoint_mode = d.endpoint_mode;
    }
    if (is_new || d.max_output_tokens_supplied ||
        d.max_output_tokens.has_value()) {
        profile.max_output_tokens = d.max_output_tokens.has_value() &&
                                    *d.max_output_tokens > 0
            ? d.max_output_tokens
            : std::nullopt;
    }
    if (is_new || d.capabilities_source_supplied ||
        d.capabilities_source.has_value()) {
        profile.capabilities_source = d.capabilities_source;
    }
    if (is_new || d.reasoning_supplied || d.reasoning.has_value()) {
        profile.reasoning = d.reasoning;
    }
    if (is_new || d.request_headers_supplied || !d.request_headers.empty()) {
        profile.request_headers = d.request_headers;
    }

    if (profile.provider == "copilot") {
        // Preserve catalog capabilities while clearing connection/runtime
        // customization for the managed provider.
        profile.base_url.clear();
        profile.api_key.clear();
        profile.models_dev_provider_id.reset();
        profile.endpoint_mode.reset();
        profile.max_output_tokens.reset();
        profile.capabilities_source.reset();
        profile.reasoning.reset();
        profile.request_headers.clear();
    } else if (profile.provider == "grok") {
        // Grok keeps catalog identity/capability metadata, but its connection
        // and request behavior are fully managed by the Coding Plan provider.
        profile.base_url.clear();
        profile.api_key.clear();
        profile.stream_timeout_ms.reset();
        profile.endpoint_mode.reset();
        profile.max_output_tokens.reset();
        profile.reasoning.reset();
        profile.request_headers.clear();
    } else if (profile.endpoint_mode != "full_url") {
        profile.base_url = normalize_model_endpoint_identity(profile.base_url);
    }
    return profile;
}

const ModelProfile* find_profile(const AppConfig& cfg, const std::string& name) {
    const auto it = std::find_if(
        cfg.saved_models.begin(), cfg.saved_models.end(),
        [&](const ModelProfile& profile) { return profile.name == name; });
    return it == cfg.saved_models.end() ? nullptr : &*it;
}

bool credential_identity_matches(const ModelProfile& source,
                                 const ModelProfile& target) {
    return source.provider == target.provider &&
           normalize_model_endpoint_identity(source.base_url) ==
               normalize_model_endpoint_identity(target.base_url) &&
           source.models_dev_provider_id == target.models_dev_provider_id;
}

SavedModelEditError apply_credentials(const AppConfig& cfg,
                                      const SavedModelDraft& d,
                                      const ModelProfile* existing,
                                      ModelProfile& candidate) {
    const bool key_supplied = d.api_key_supplied || !d.api_key.empty();
    if (key_supplied) {
        if (d.api_key.empty()) return SavedModelEditError::INVALID_API_KEY;
        candidate.api_key = d.api_key;
    } else if (d.credential_source_name.has_value()) {
        const ModelProfile* source = find_profile(cfg, *d.credential_source_name);
        if (!source || source->api_key.empty() ||
            !credential_identity_matches(*source, candidate)) {
            return SavedModelEditError::INVALID_CREDENTIAL_SOURCE;
        }
        candidate.api_key = source->api_key;
    } else if (d.clear_api_key) {
        candidate.api_key.clear();
        if (!model_profile_allows_no_api_key(candidate)) {
            return SavedModelEditError::INVALID_API_KEY;
        }
    } else if (!existing || existing->provider != candidate.provider) {
        candidate.api_key.clear();
    }

    if (candidate.provider == "copilot" || candidate.provider == "grok") {
        candidate.api_key.clear();
        return SavedModelEditError::OK;
    }
    if (candidate.api_key.empty() && !model_profile_allows_no_api_key(candidate)) {
        return SavedModelEditError::INVALID_API_KEY;
    }
    return SavedModelEditError::OK;
}

SavedModelEditError validate_candidate(const ModelProfile& candidate) {
    if ((candidate.provider == "openai" || candidate.provider == "anthropic") &&
        candidate.base_url.empty()) {
        return SavedModelEditError::MISSING_BASE_URL;
    }
    if (candidate.endpoint_mode.has_value() &&
        *candidate.endpoint_mode != "base_url" &&
        *candidate.endpoint_mode != "full_url") {
        return SavedModelEditError::INVALID_ENDPOINT_MODE;
    }
    if (candidate.max_output_tokens.has_value() &&
        *candidate.max_output_tokens <= 0) {
        return SavedModelEditError::INVALID_MAX_OUTPUT_TOKENS;
    }
    if (candidate.capabilities_source.has_value() &&
        *candidate.capabilities_source != "catalog" &&
        *candidate.capabilities_source != "manual") {
        return SavedModelEditError::INVALID_CAPABILITIES_SOURCE;
    }
    if (candidate.reasoning.has_value() &&
        candidate.capabilities_source.has_value() &&
        has_reasoning_capability(candidate) != candidate.reasoning->supported) {
        return SavedModelEditError::INVALID_REASONING;
    }
    std::string error;
    if (!validate_saved_models({candidate}, "", error)) {
        if (error.find("reasoning") != std::string::npos) {
            return SavedModelEditError::INVALID_REASONING;
        }
        if (error.find("full_url") != std::string::npos ||
            error.find("endpoint") != std::string::npos ||
            error.find("managed provider") != std::string::npos ||
            error.find("unsupported") != std::string::npos) {
            return SavedModelEditError::UNSUPPORTED_MODEL_OPTION;
        }
        return SavedModelEditError::INVALID_CAPABILITY;
    }
    return SavedModelEditError::OK;
}

bool name_exists_except(const AppConfig& cfg,
                        const std::string& name,
                        const ModelProfile* except) {
    for (const auto& profile : cfg.saved_models) {
        if (&profile != except && profile.name == name) return true;
    }
    return false;
}

} // namespace

SavedModelEditError add_saved_model(AppConfig& cfg, const SavedModelDraft& d) {
    if (auto error = validate_draft_shape(d); error != SavedModelEditError::OK) {
        return error;
    }
    if (name_exists_except(cfg, d.name, nullptr)) {
        return SavedModelEditError::NAME_TAKEN;
    }
    ModelProfile candidate = merge_profile(d, nullptr);
    if (auto error = apply_credentials(cfg, d, nullptr, candidate);
        error != SavedModelEditError::OK) {
        return error;
    }
    if (auto error = validate_candidate(candidate);
        error != SavedModelEditError::OK) {
        return error;
    }
    cfg.saved_models.push_back(std::move(candidate));
    return SavedModelEditError::OK;
}

SavedModelEditError update_saved_model(AppConfig& cfg,
                                       const std::string& old_name,
                                       const SavedModelDraft& d) {
    auto it = std::find_if(
        cfg.saved_models.begin(), cfg.saved_models.end(),
        [&](const ModelProfile& profile) { return profile.name == old_name; });
    if (it == cfg.saved_models.end()) return SavedModelEditError::NOT_FOUND;
    if (auto error = validate_draft_shape(d); error != SavedModelEditError::OK) {
        return error;
    }
    if (name_exists_except(cfg, d.name, &*it)) {
        return SavedModelEditError::NAME_TAKEN;
    }

    ModelProfile candidate = merge_profile(d, &*it);
    if (auto error = apply_credentials(cfg, d, &*it, candidate);
        error != SavedModelEditError::OK) {
        return error;
    }
    if (auto error = validate_candidate(candidate);
        error != SavedModelEditError::OK) {
        return error;
    }

    const bool renaming = d.name != old_name;
    *it = std::move(candidate);
    if (renaming && cfg.default_model_name == old_name) {
        cfg.default_model_name = d.name;
    }
    return SavedModelEditError::OK;
}

SavedModelEditError remove_saved_model(AppConfig& cfg, const std::string& name) {
    auto it = std::find_if(
        cfg.saved_models.begin(), cfg.saved_models.end(),
        [&](const ModelProfile& profile) { return profile.name == name; });
    if (it == cfg.saved_models.end()) return SavedModelEditError::NOT_FOUND;
    const bool removing_default = cfg.default_model_name == name;
    cfg.saved_models.erase(it);
    if (removing_default) cfg.default_model_name.clear();
    return SavedModelEditError::OK;
}

} // namespace acecode
