#include "provider_request_options.hpp"

#include "llm/llm_provider.hpp"
#include "utils/sha256.hpp"
#include "utils/text.hpp"
#include "utils/uuid.hpp"
#include "version.hpp"

#include <algorithm>

namespace acecode {
namespace {

bool is_opencode_go_endpoint(const std::string& url) {
    const auto endpoint = normalize_model_endpoint_identity(url);
    return endpoint.rfind("https://opencode.ai/inference/go/", 0) == 0 ||
        endpoint.rfind("https://opencode.ai/zen/go/", 0) == 0;
}

bool has_header(const RequestHeaders& headers, const char* lower_name) {
    return std::any_of(headers.begin(), headers.end(), [lower_name](const auto& header) {
        return utils::ascii_lower(header.first) == lower_name;
    });
}

} // namespace

std::optional<RequestHeaders> ProviderRequestOptions::resolve_headers(
    const RequestHeaders& configured, const std::string& url,
    const ChatRequestOptions* call, std::string& error) const {
    auto headers = resolve_request_headers(configured, error);
    if (!headers || (!opencode_go && !is_opencode_go_endpoint(url))) return headers;

    if (!has_header(*headers, "x-opencode-session")) {
        // The engine's cache key identifies the conversation across turns and resume.
        // Hash it to keep arbitrary caller-supplied bytes out of an HTTP header.
        (*headers)["x-opencode-session"] = call && !call->prompt_cache_key.empty()
            ? sha256_hex(call->prompt_cache_key) : generate_uuid_v7();
    }
    if (!has_header(*headers, "user-agent")) {
        (*headers)["User-Agent"] = "ACECode/" ACECODE_VERSION;
    }
    return headers;
}

void append_sampling_request_fields(nlohmann::json& body,
                                    const ModelSamplingOptions& sampling,
                                    bool supports_min_p) {
    if (sampling.temperature.has_value()) body["temperature"] = *sampling.temperature;
    if (sampling.top_p.has_value()) body["top_p"] = *sampling.top_p;
    if (sampling.top_k.has_value()) body["top_k"] = *sampling.top_k;
    if (supports_min_p && sampling.min_p.has_value()) body["min_p"] = *sampling.min_p;
}

} // namespace acecode
