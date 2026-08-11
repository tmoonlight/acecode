// saved_models 实现:纯函数 parse + validate。
// 对应 openspec/changes/model-profiles 任务 1.2。
#include "config/saved_models.hpp"

#include "model_provider_registry.hpp"
#include "request_headers.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <set>
#include <sstream>

namespace acecode {

namespace {

bool is_valid_capability_tag(const std::string& tag) {
    if (tag.empty()) return false;
    for (unsigned char ch : tag) {
        if (std::iscntrl(ch)) return false;
    }
    return true;
}

bool is_valid_reasoning_effort(const std::string& effort) {
    static const std::set<std::string> kEfforts{
        "minimal", "low", "medium", "high", "xhigh", "max"};
    return kEfforts.count(effort) != 0;
}

bool validate_reasoning_options(const ModelReasoningOptions& r,
                                std::string& err) {
    if (!r.supported) {
        if (r.mandatory || r.default_enabled || r.enabled.has_value() ||
            !r.supported_efforts.empty() || r.default_effort.has_value() ||
            r.effort.has_value() || r.supports_max_tokens ||
            r.max_tokens.has_value()) {
            err = "reasoning options are present but reasoning is unsupported";
            return false;
        }
        return true;
    }
    if (r.mandatory && !r.default_enabled) {
        err = "mandatory reasoning must be enabled by default";
        return false;
    }
    if (r.mandatory && r.enabled.has_value() && !*r.enabled) {
        err = "mandatory reasoning cannot be disabled";
        return false;
    }
    std::set<std::string> efforts;
    for (const auto& effort : r.supported_efforts) {
        if (!is_valid_reasoning_effort(effort) ||
            !efforts.insert(effort).second) {
            err = "reasoning supported_efforts contains an invalid value";
            return false;
        }
    }
    auto check_effort = [&](const std::optional<std::string>& effort,
                            const char* field) {
        if (!effort.has_value()) return true;
        if (!is_valid_reasoning_effort(*effort) ||
            efforts.count(*effort) == 0) {
            err = std::string("reasoning ") + field +
                  " is not in supported_efforts";
            return false;
        }
        return true;
    };
    if (!check_effort(r.default_effort, "default_effort") ||
        !check_effort(r.effort, "effort")) {
        return false;
    }
    if (r.max_tokens.has_value() &&
        (!r.supports_max_tokens || *r.max_tokens <= 0)) {
        err = "reasoning max_tokens requires positive supported token budgets";
        return false;
    }
    return true;
}

std::string trim_ascii(std::string value) {
    auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    });
    auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    }).base();
    if (first >= last) return {};
    return std::string(first, last);
}

std::string ascii_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool is_loopback_endpoint(const std::string& endpoint) {
    const std::string normalized = normalize_model_endpoint_identity(endpoint);
    const auto scheme_end = normalized.find("://");
    if (scheme_end == std::string::npos) return false;
    const auto authority_start = scheme_end + 3;
    const auto authority_end = normalized.find_first_of("/?#", authority_start);
    std::string authority = normalized.substr(
        authority_start,
        authority_end == std::string::npos
            ? std::string::npos
            : authority_end - authority_start);
    const auto at = authority.rfind('@');
    if (at != std::string::npos) authority.erase(0, at + 1);
    if (authority == "[::1]" || authority.rfind("[::1]:", 0) == 0) return true;
    const auto colon = authority.find(':');
    const std::string host = colon == std::string::npos
        ? authority
        : authority.substr(0, colon);
    return host == "localhost" || host == "127.0.0.1";
}

bool is_known_local_provider_id(const std::optional<std::string>& provider_id) {
    if (!provider_id.has_value()) return false;
    static const std::set<std::string> kLocalIds{
        "ollama", "lmstudio", "lm-studio", "llamacpp", "llama.cpp",
        "localai", "vllm"};
    return kLocalIds.count(ascii_lower(*provider_id)) != 0;
}

std::vector<std::string> parse_capabilities_array(const nlohmann::json& node) {
    std::vector<std::string> out;
    if (!node.is_array()) return out;
    for (const auto& item : node) {
        if (!item.is_string()) continue;
        std::string tag = item.get<std::string>();
        if (tag.empty()) continue;
        out.push_back(std::move(tag));
    }
    return out;
}

// 单条 entry 解析。失败时把错误塞进 err(已带 entry 索引/name 上下文)并返回 nullopt。
std::optional<ModelProfile> parse_one_entry(const nlohmann::json& node, std::size_t idx,
                                          std::string& err) {
    if (!node.is_object()) {
        std::ostringstream oss;
        oss << "saved_models[" << idx << "] is not an object";
        err = oss.str();
        return std::nullopt;
    }

    ModelProfile e;
    auto get_str = [&](const char* key, std::string& out) -> bool {
        if (!node.contains(key)) return false;
        if (!node[key].is_string()) return false;
        out = node[key].get<std::string>();
        return true;
    };

    if (!get_str("name", e.name) || e.name.empty()) {
        std::ostringstream oss;
        oss << "saved_models[" << idx << "] missing required field 'name'";
        err = oss.str();
        return std::nullopt;
    }
    if (!get_str("provider", e.provider) || e.provider.empty()) {
        std::ostringstream oss;
        oss << "saved_models[" << idx << "] (name='" << e.name
            << "') missing required field 'provider'";
        err = oss.str();
        return std::nullopt;
    }
    if (!is_known_model_provider(e.provider)) {
        std::ostringstream oss;
        oss << "saved_models[" << idx << "] (name='" << e.name
            << "') has unknown provider '" << e.provider
            << "' (expected 'openai', 'anthropic', 'copilot', or 'codex')";
        err = oss.str();
        return std::nullopt;
    }
    if (!get_str("model", e.model) || e.model.empty()) {
        std::ostringstream oss;
        oss << "saved_models[" << idx << "] (name='" << e.name
            << "') missing required field 'model'";
        err = oss.str();
        return std::nullopt;
    }

    // base_url / api_key:openai/anthropic 必填;copilot/codex 忽略(允许字段缺失或为空)。
    get_str("base_url", e.base_url);
    get_str("api_key", e.api_key);

    if (node.contains("models_dev_provider_id") &&
        node["models_dev_provider_id"].is_string()) {
        std::string s = node["models_dev_provider_id"].get<std::string>();
        if (!s.empty()) e.models_dev_provider_id = std::move(s);
    }
    if (node.contains("context_window") && node["context_window"].is_number_integer()) {
        int context_window = node["context_window"].get<int>();
        if (context_window <= 0) {
            std::ostringstream oss;
            oss << "saved_models[" << idx << "] (name='" << e.name
                << "') has invalid context_window";
            err = oss.str();
            return std::nullopt;
        }
        e.context_window = context_window;
    }
    if (node.contains("stream_timeout_ms") && node["stream_timeout_ms"].is_number_integer()) {
        int stream_timeout_ms = node["stream_timeout_ms"].get<int>();
        if (stream_timeout_ms <= 0) {
            std::ostringstream oss;
            oss << "saved_models[" << idx << "] (name='" << e.name
                << "') has invalid stream_timeout_ms";
            err = oss.str();
            return std::nullopt;
        }
        e.stream_timeout_ms = stream_timeout_ms;
    }
    if (node.contains("capabilities")) {
        e.capabilities = parse_capabilities_array(node["capabilities"]);
    }
    if (node.contains("endpoint_mode")) {
        if (!node["endpoint_mode"].is_string()) {
            err = "saved_models[" + std::to_string(idx) +
                  "] field 'endpoint_mode' must be string";
            return std::nullopt;
        }
        e.endpoint_mode = node["endpoint_mode"].get<std::string>();
    }
    if (node.contains("max_output_tokens")) {
        if (!node["max_output_tokens"].is_number_integer()) {
            err = "saved_models[" + std::to_string(idx) +
                  "] field 'max_output_tokens' must be integer";
            return std::nullopt;
        }
        const long long value = node["max_output_tokens"].get<long long>();
        if (value <= 0 || value > std::numeric_limits<int>::max()) {
            err = "saved_models[" + std::to_string(idx) +
                  "] has invalid max_output_tokens";
            return std::nullopt;
        }
        e.max_output_tokens = static_cast<int>(value);
    }
    if (node.contains("capabilities_source")) {
        if (!node["capabilities_source"].is_string()) {
            err = "saved_models[" + std::to_string(idx) +
                  "] field 'capabilities_source' must be string";
            return std::nullopt;
        }
        e.capabilities_source = node["capabilities_source"].get<std::string>();
    }
    if (node.contains("reasoning")) {
        std::string reasoning_err;
        auto reasoning = parse_model_reasoning_options(node["reasoning"],
                                                        reasoning_err);
        if (!reasoning.has_value()) {
            err = "saved_models[" + std::to_string(idx) +
                  "] has invalid reasoning: " + reasoning_err;
            return std::nullopt;
        }
        e.reasoning = std::move(*reasoning);
    }
    if (node.contains("request_headers")) {
        std::ostringstream context;
        context << "saved_models[" << idx << "] (name='" << e.name << "')";
        auto parsed = parse_request_headers_json(node["request_headers"], context.str(), err);
        if (!parsed.has_value()) return std::nullopt;
        e.request_headers = std::move(*parsed);
    }
    if (node.contains("readonly") && node["readonly"].is_boolean()) {
        e.readonly = node["readonly"].get<bool>();
    }

    return e;
}

} // namespace

std::optional<std::vector<ModelProfile>> parse_saved_models(const nlohmann::json& node,
                                                          std::string& err) {
    std::vector<ModelProfile> out;
    if (node.is_null()) return out;
    if (!node.is_array()) {
        err = "saved_models must be a JSON array";
        return std::nullopt;
    }
    out.reserve(node.size());
    for (std::size_t i = 0; i < node.size(); ++i) {
        auto entry = parse_one_entry(node[i], i, err);
        if (!entry.has_value()) return std::nullopt;
        out.push_back(std::move(*entry));
    }
    return out;
}

bool validate_saved_models(const std::vector<ModelProfile>& entries,
                           const std::string& default_name,
                           std::string& err) {
    std::set<std::string> seen_names;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        if (e.name.empty()) {
            std::ostringstream oss;
            oss << "saved_models[" << i << "] has empty name";
            err = oss.str();
            return false;
        }
        if (!e.name.empty() && e.name.front() == '(') {
            std::ostringstream oss;
            oss << "saved_models[" << i << "] name '" << e.name
                << "' uses reserved prefix '(' (reserved for ACECode-synthesized "
                   "names like \"(session:...)\")";
            err = oss.str();
            return false;
        }
        if (seen_names.count(e.name)) {
            std::ostringstream oss;
            oss << "saved_models has duplicate name '" << e.name << "'";
            err = oss.str();
            return false;
        }
        seen_names.insert(e.name);

        if (e.provider == "openai" || e.provider == "anthropic") {
            if (e.base_url.empty()) {
                std::ostringstream oss;
                oss << "saved_models entry '" << e.name
                    << "' (provider=" << e.provider << ") requires non-empty base_url";
                err = oss.str();
                return false;
            }
            if (e.api_key.empty() && !model_profile_allows_no_api_key(e)) {
                std::ostringstream oss;
                oss << "saved_models entry '" << e.name
                    << "' (provider=" << e.provider << ") requires non-empty api_key";
                err = oss.str();
                return false;
            }
        } else if (!e.request_headers.empty()) {
            std::ostringstream oss;
            oss << "saved_models entry '" << e.name
                << "' has request_headers but provider is not openai or anthropic";
            err = oss.str();
            return false;
        }
        if (e.provider == "copilot") {
            if (!e.base_url.empty() || !e.api_key.empty()) {
                err = "saved_models entry '" + e.name +
                      "' cannot customize Copilot endpoint or credentials";
                return false;
            }
            if (e.endpoint_mode.has_value() || e.reasoning.has_value() ||
                e.max_output_tokens.has_value()) {
                err = "saved_models entry '" + e.name +
                      "' contains unsupported Copilot runtime options";
                return false;
            }
        }
        if (e.endpoint_mode.has_value()) {
            if (*e.endpoint_mode != "base_url" &&
                *e.endpoint_mode != "full_url") {
                err = "saved_models entry '" + e.name +
                      "' has unknown endpoint_mode";
                return false;
            }
            if (*e.endpoint_mode == "full_url" &&
                (e.provider != "openai" ||
                 e.models_dev_provider_id.has_value())) {
                err = "saved_models entry '" + e.name +
                      "' cannot use full_url for this provider";
                return false;
            }
        }
        if (e.max_output_tokens.has_value() && *e.max_output_tokens <= 0) {
            err = "saved_models entry '" + e.name +
                  "' has invalid max_output_tokens";
            return false;
        }
        if (e.capabilities_source.has_value() &&
            *e.capabilities_source != "catalog" &&
            *e.capabilities_source != "manual") {
            err = "saved_models entry '" + e.name +
                  "' has invalid capabilities_source";
            return false;
        }
        if (e.reasoning.has_value()) {
            std::string reasoning_err;
            if (!validate_reasoning_options(*e.reasoning, reasoning_err)) {
                err = "saved_models entry '" + e.name +
                      "' has invalid reasoning: " + reasoning_err;
                return false;
            }
            if (e.capabilities_source.has_value()) {
                const bool declared = std::find(
                    e.capabilities.begin(), e.capabilities.end(), "reasoning") !=
                    e.capabilities.end();
                if (declared != e.reasoning->supported) {
                    err = "saved_models entry '" + e.name +
                          "' has inconsistent reasoning capability metadata";
                    return false;
                }
            }
            if (e.reasoning->max_tokens.has_value() &&
                e.max_output_tokens.has_value() &&
                *e.max_output_tokens <= *e.reasoning->max_tokens) {
                err = "saved_models entry '" + e.name +
                      "' requires max_output_tokens greater than reasoning max_tokens";
                return false;
            }
            if (e.provider == "anthropic") {
                if (e.reasoning->max_tokens.has_value() &&
                    e.max_output_tokens.value_or(4096) <=
                        *e.reasoning->max_tokens) {
                    err = "saved_models entry '" + e.name +
                          "' requires Anthropic max_output_tokens greater than "
                          "reasoning max_tokens";
                    return false;
                }
            }
        }
        if (e.context_window.has_value() && *e.context_window <= 0) {
            std::ostringstream oss;
            oss << "saved_models entry '" << e.name
                << "' has invalid context_window";
            err = oss.str();
            return false;
        }
        if (e.stream_timeout_ms.has_value() && *e.stream_timeout_ms <= 0) {
            std::ostringstream oss;
            oss << "saved_models entry '" << e.name
                << "' has invalid stream_timeout_ms";
            err = oss.str();
            return false;
        }
        std::set<std::string> seen_capabilities;
        for (const auto& tag : e.capabilities) {
            if (!is_valid_capability_tag(tag)) {
                std::ostringstream oss;
                oss << "saved_models entry '" << e.name
                    << "' has invalid capability tag";
                err = oss.str();
                return false;
            }
            if (!seen_capabilities.insert(tag).second) {
                std::ostringstream oss;
                oss << "saved_models entry '" << e.name
                    << "' has duplicate capability '" << tag << "'";
                err = oss.str();
                return false;
            }
        }
        if (!e.request_headers.empty()) {
            std::string headers_err;
            if (!validate_request_headers(e.request_headers, headers_err)) {
                std::ostringstream oss;
                oss << "saved_models entry '" << e.name << "' invalid "
                    << headers_err;
                err = oss.str();
                return false;
            }
        }
        // copilot/codex:不需要 base_url / api_key,model 已在 parse 阶段检验非空。
    }

    if (!default_name.empty()) {
        if (!seen_names.count(default_name)) {
            std::ostringstream oss;
            oss << "default_model_name '" << default_name
                << "' does not match any saved_models entry";
            err = oss.str();
            return false;
        }
    }
    return true;
}

std::optional<ModelReasoningOptions> parse_model_reasoning_options(
    const nlohmann::json& node,
    std::string& err) {
    if (!node.is_object()) {
        err = "reasoning must be an object";
        return std::nullopt;
    }
    ModelReasoningOptions result;
    auto required_bool = [&](const char* key, bool& out) {
        auto it = node.find(key);
        if (it == node.end() || !it->is_boolean()) {
            if (err.empty()) err = std::string("reasoning field '") + key +
                                   "' must be boolean";
            return false;
        }
        out = it->get<bool>();
        return true;
    };
    if (!required_bool("supported", result.supported) ||
        !required_bool("mandatory", result.mandatory) ||
        !required_bool("default_enabled", result.default_enabled) ||
        !required_bool("supports_max_tokens", result.supports_max_tokens)) {
        return std::nullopt;
    }
    if (auto it = node.find("enabled"); it != node.end() && !it->is_null()) {
        if (!it->is_boolean()) {
            err = "reasoning field 'enabled' must be boolean or null";
            return std::nullopt;
        }
        result.enabled = it->get<bool>();
    }
    auto efforts_it = node.find("supported_efforts");
    if (efforts_it == node.end() || !efforts_it->is_array()) {
        err = "reasoning field 'supported_efforts' must be an array";
        return std::nullopt;
    }
    for (const auto& item : *efforts_it) {
        if (!item.is_string()) {
            err = "reasoning supported_efforts must contain strings";
            return std::nullopt;
        }
        result.supported_efforts.push_back(item.get<std::string>());
    }
    auto optional_string = [&](const char* key,
                               std::optional<std::string>& out) {
        auto it = node.find(key);
        if (it == node.end() || it->is_null()) return true;
        if (!it->is_string()) {
            err = std::string("reasoning field '") + key +
                  "' must be string or null";
            return false;
        }
        out = it->get<std::string>();
        return true;
    };
    if (!optional_string("default_effort", result.default_effort) ||
        !optional_string("effort", result.effort)) {
        return std::nullopt;
    }
    if (auto it = node.find("max_tokens"); it != node.end() && !it->is_null()) {
        if (!it->is_number_integer()) {
            err = "reasoning field 'max_tokens' must be integer or null";
            return std::nullopt;
        }
        const long long value = it->get<long long>();
        if (value <= 0 || value > std::numeric_limits<int>::max()) {
            err = "reasoning max_tokens must be positive";
            return std::nullopt;
        }
        result.max_tokens = static_cast<int>(value);
    }
    if (!validate_reasoning_options(result, err)) return std::nullopt;
    return result;
}

nlohmann::json model_reasoning_options_to_json(
    const ModelReasoningOptions& options) {
    nlohmann::json result{
        {"supported", options.supported},
        {"mandatory", options.mandatory},
        {"default_enabled", options.default_enabled},
        {"supported_efforts", options.supported_efforts},
        {"supports_max_tokens", options.supports_max_tokens},
    };
    if (options.enabled.has_value()) result["enabled"] = *options.enabled;
    if (options.default_effort.has_value()) {
        result["default_effort"] = *options.default_effort;
    }
    if (options.effort.has_value()) result["effort"] = *options.effort;
    if (options.max_tokens.has_value()) result["max_tokens"] = *options.max_tokens;
    return result;
}

bool model_profile_allows_no_api_key(const ModelProfile& profile) {
    if (profile.provider == "copilot") return true;
    if (profile.provider != "openai") return false;
    return is_known_local_provider_id(profile.models_dev_provider_id) ||
           is_loopback_endpoint(profile.base_url);
}

std::string normalize_model_endpoint_identity(const std::string& value) {
    std::string normalized = trim_ascii(value);
    auto suffix_start = normalized.find_first_of("?#");
    auto path_end = suffix_start == std::string::npos
        ? normalized.size()
        : suffix_start;
    while (path_end > 0 && normalized[path_end - 1] == '/') {
        normalized.erase(path_end - 1, 1);
        --path_end;
    }
    const auto scheme_end = normalized.find("://");
    if (scheme_end == std::string::npos) return normalized;
    const std::string scheme = ascii_lower(normalized.substr(0, scheme_end));
    const auto authority_start = scheme_end + 3;
    const auto authority_end = normalized.find_first_of("/?#", authority_start);
    const auto lower_end = authority_end == std::string::npos
        ? normalized.size()
        : authority_end;
    std::transform(normalized.begin(), normalized.begin() + lower_end,
                   normalized.begin(), [](unsigned char ch) {
                       return static_cast<char>(std::tolower(ch));
                   });
    const std::string default_port = scheme == "http"
        ? ":80"
        : (scheme == "https" ? ":443" : std::string{});
    if (!default_port.empty()) {
        const auto current_authority_end =
            normalized.find_first_of("/?#", authority_start);
        const auto end = current_authority_end == std::string::npos
            ? normalized.size()
            : current_authority_end;
        if (end >= authority_start + default_port.size() &&
            normalized.compare(end - default_port.size(), default_port.size(),
                               default_port) == 0) {
            normalized.erase(end - default_port.size(), default_port.size());
        }
    }
    return normalized;
}

} // namespace acecode
