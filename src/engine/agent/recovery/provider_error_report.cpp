#include "provider_error_report.hpp"
#include "utils/logger.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <sstream>
#include <utility>

namespace acecode::agent::detail {

std::string provider_error_kind_to_json_string(ProviderErrorKind kind) {
    switch (kind) {
    case ProviderErrorKind::None:          return "none";
    case ProviderErrorKind::UserCancelled: return "user_cancelled";
    case ProviderErrorKind::Timeout:       return "timeout";
    case ProviderErrorKind::Network:       return "network";
    case ProviderErrorKind::Http:          return "http";
    case ProviderErrorKind::MalformedSse:  return "malformed_sse";
    case ProviderErrorKind::MalformedJson: return "malformed_json";
    case ProviderErrorKind::Unknown:       return "unknown";
    }
    return "unknown";
}

nlohmann::json provider_error_to_json(const ProviderErrorInfo& info) {
    nlohmann::json j = {
        {"kind", provider_error_kind_to_json_string(info.kind)},
        {"status_code", info.status_code},
        {"provider", info.provider},
        {"model", info.model},
        {"request_id", info.request_id},
        {"display_message", info.display_message},
        {"raw_body", info.raw_body},
        {"body_is_json", info.body_is_json},
        {"pretty_json", info.pretty_json},
        {"retryable", info.retryable},
        {"retry_attempt", info.retry_attempt},
        {"retry_max_attempts", info.retry_max_attempts},
        {"retry_delay_ms", info.retry_delay_ms},
        {"server_retry_after_ms", info.server_retry_after_ms},
    };
    return j;
}

std::string provider_error_summary_for_log(const ProviderErrorInfo& info) {
    std::string message = info.display_message;
    if (message.empty()) message = info.pretty_json;
    if (message.empty()) message = info.raw_body;

    std::ostringstream oss;
    oss << "kind=" << provider_error_kind_to_json_string(info.kind)
        << " status=" << info.status_code
        << " provider=" << info.provider
        << " model=" << info.model
        << " request_id=" << info.request_id
        << " retryable=" << (info.retryable ? "true" : "false")
        << " retry_attempt=" << info.retry_attempt
        << " retry_max_attempts=" << info.retry_max_attempts
        << " retry_delay_ms=" << info.retry_delay_ms
        << " raw_body_bytes=" << info.raw_body.size()
        << " pretty_json_bytes=" << info.pretty_json.size()
        << " message=" << log_truncate(message, 300);
    return oss.str();
}

} // namespace acecode::agent::detail
