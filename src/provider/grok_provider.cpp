#include "grok_provider.hpp"

#include "grok_responses.hpp"
#include "network/proxy_resolver.hpp"
#include "utils/uuid.hpp"

#include <cpr/cpr.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <sstream>
#include <string_view>

namespace acecode {
namespace {

constexpr int kStreamConnectTimeoutCapMs = 15000;

std::int64_t steady_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string ascii_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) {
                       return static_cast<char>(std::tolower(ch));
                   });
    return value;
}

std::string header_value_ci(const cpr::Header& headers,
                            const std::string& wanted) {
    const std::string normalized = ascii_lower(wanted);
    for (const auto& [name, value] : headers) {
        if (ascii_lower(name) == normalized) return value;
    }
    return {};
}

ProviderErrorKind classify_cpr_error(const cpr::Error& error) {
    if (error.code == cpr::ErrorCode::OPERATION_TIMEDOUT) {
        return ProviderErrorKind::Timeout;
    }
    const std::string message = ascii_lower(error.message);
    return message.find("timeout") != std::string::npos ||
        message.find("timed out") != std::string::npos
        ? ProviderErrorKind::Timeout
        : ProviderErrorKind::Network;
}

std::string compact_uuid_hex() {
    std::string value = generate_uuid();
    value.erase(std::remove(value.begin(), value.end(), '-'), value.end());
    return value;
}

std::string traceparent() {
    const std::string trace = compact_uuid_hex();
    const std::string span = compact_uuid_hex().substr(0, 16);
    return "00-" + trace + "-" + span + "-01";
}

cpr::Header grok_headers(const GrokAuthConfig& config,
                         const GrokAuthTokens& tokens,
                         const std::string& model,
                         const std::string& agent_id,
                         const std::string& session_id,
                         const std::string& request_id,
                         bool streaming) {
    cpr::Header headers{
        {"Accept", streaming ? "text/event-stream" : "application/json"},
        {"Accept-Encoding", streaming ? "identity" : "gzip"},
        {"Content-Type", "application/json"},
        {"Authorization", "Bearer " + tokens.access_token},
        {"X-XAI-Token-Auth", grok_build::kTokenAuth},
        {"x-grok-client-version", config.client_version},
        {"x-grok-client-identifier", grok_build::kClientIdentifier},
        {"x-grok-client-mode", "headless"},
        {"x-authenticateresponse", "authenticate-response"},
        {"x-grok-agent-id", agent_id},
        {"x-grok-session-id", session_id},
        {"x-grok-conv-id", session_id},
        {"x-grok-req-id", request_id},
        {"x-grok-model-override", model},
        {"traceparent", traceparent()},
        {"User-Agent", "grok-shell/" + config.client_version +
             " (linux; x86_64)"},
    };
    if (!tokens.user_id.empty()) headers["x-grok-user-id"] = tokens.user_id;
    return headers;
}

ProviderErrorInfo grok_error_info(ProviderErrorKind kind,
                                  int status_code,
                                  const std::string& model,
                                  const std::string& request_id,
                                  const std::string& message,
                                  const std::string& raw_body,
                                  bool retryable,
                                  const cpr::Header& headers = {}) {
    ProviderErrorInfo info;
    info.kind = kind;
    info.status_code = status_code;
    info.provider = "grok";
    info.model = model;
    info.request_id = request_id;
    info.display_message = redact_grok_auth_diagnostic(message);
    info.raw_body = redact_grok_auth_diagnostic(raw_body);
    info.retryable = retryable;
    if (!info.raw_body.empty()) {
        try {
            info.pretty_json = nlohmann::json::parse(info.raw_body).dump(2);
            info.body_is_json = true;
        } catch (...) {
            // Keep the already-redacted raw body for diagnostics.
        }
    }
    const std::string retry_after = header_value_ci(headers, "retry-after");
    if (!retry_after.empty()) {
        if (const auto parsed = parse_retry_after_ms(retry_after)) {
            info.server_retry_after_ms = *parsed;
        }
    }
    return info;
}

ChatResponse grok_error_response(const ProviderErrorInfo& info) {
    ChatResponse response;
    response.content = "[Error] " + info.display_message;
    response.finish_reason = "error";
    response.provider_error = info;
    return response;
}

void emit_grok_error(const StreamCallback& callback,
                     const ProviderErrorInfo& info) {
    StreamEvent event;
    event.type = StreamEventType::Error;
    event.error = info.display_message;
    event.provider_error = info;
    callback(event);
}

bool find_event_delimiter(const std::string& buffer,
                          std::size_t& position,
                          std::size_t& delimiter_length) {
    const std::size_t lf = buffer.find("\n\n");
    const std::size_t crlf = buffer.find("\r\n\r\n");
    if (lf == std::string::npos && crlf == std::string::npos) return false;
    if (crlf != std::string::npos &&
        (lf == std::string::npos || crlf < lf)) {
        position = crlf;
        delimiter_length = 4;
    } else {
        position = lf;
        delimiter_length = 2;
    }
    return true;
}

std::string sse_data(const std::string& block) {
    std::istringstream input(block);
    std::string line;
    std::string data;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.compare(0, 5, "data:") != 0) continue;
        std::size_t start = 5;
        if (start < line.size() && line[start] == ' ') ++start;
        if (!data.empty()) data.push_back('\n');
        data += line.substr(start);
    }
    return data;
}

} // namespace

GrokProvider::GrokProvider(const std::string& model,
                           ProviderRequestOptions request_options,
                           GrokAuthConfig auth_config,
                           int stream_timeout_ms)
    : OpenAiCompatProvider(
          auth_config.build_base_url, "", model, stream_timeout_ms, {},
          std::move(request_options)),
      auth_config_(std::move(auth_config)),
      agent_id_(generate_uuid()),
      session_id_(generate_uuid()) {}

std::string GrokProvider::responses_url() const {
    std::string base = auth_config_.build_base_url;
    while (!base.empty() && base.back() == '/') base.pop_back();
    return base + "/responses";
}

bool GrokProvider::is_authenticated() {
    return ensure_grok_access_token(false, "", auth_config_).ok;
}

bool GrokProvider::authenticate() {
    return is_authenticated();
}

ChatResponse GrokProvider::chat(
        const std::vector<ChatMessage>& messages,
        const std::vector<ToolDef>& tools) {
    std::string conversion_error;
    nlohmann::json body = build_grok_responses_request(
        build_request_body(messages, tools, false), &messages,
        &conversion_error);
    if (!conversion_error.empty()) {
        return grok_error_response(grok_error_info(
            ProviderErrorKind::Unknown, 0, model_, "",
            "Could not build Grok request: " + conversion_error, "", false));
    }
    body["prompt_cache_key"] = session_id_;

    GrokAccessTokenResult access = ensure_grok_access_token(
        false, "", auth_config_);
    if (!access.ok) {
        return grok_error_response(grok_error_info(
            ProviderErrorKind::Unknown, access.status_code, model_, "",
            access.message.empty() ? access.error : access.message, "", false));
    }

    const std::string url = responses_url();
    std::string request_id = generate_uuid();
    auto make_request = [&](const GrokAuthTokens& tokens) {
        auto proxy = network::proxy_options_for(url);
        return cpr::Post(
            cpr::Url{url},
            grok_headers(auth_config_, tokens, model_, agent_id_,
                         session_id_, request_id, false),
            cpr::Body{body.dump()},
            network::build_ssl_options(proxy),
            proxy.proxies,
            proxy.auth,
            cpr::Timeout{stream_timeout_ms_});
    };

    cpr::Response upstream = make_request(access.tokens);
    if (upstream.status_code == 401) {
        GrokAccessTokenResult refreshed = ensure_grok_access_token(
            true, access.tokens.access_token, auth_config_);
        if (!refreshed.ok) {
            return grok_error_response(grok_error_info(
                ProviderErrorKind::Http, 401, model_, request_id,
                refreshed.message.empty() ? refreshed.error : refreshed.message,
                upstream.text, false, upstream.header));
        }
        access = std::move(refreshed);
        request_id = generate_uuid();
        upstream = make_request(access.tokens);
    }

    if (upstream.status_code == 0) {
        const ProviderErrorKind kind = classify_cpr_error(upstream.error);
        return grok_error_response(grok_error_info(
            kind, 0, model_, request_id,
            upstream.error.message.empty()
                ? "Grok connection failed"
                : upstream.error.message,
            upstream.text, true, upstream.header));
    }
    if (upstream.status_code < 200 || upstream.status_code >= 300) {
        const int status = static_cast<int>(upstream.status_code);
        return grok_error_response(grok_error_info(
            ProviderErrorKind::Http, status, model_, request_id,
            "Grok Build returned HTTP " + std::to_string(status),
            upstream.text,
            provider_http_error_is_retryable(status, upstream.text),
            upstream.header));
    }
    try {
        ChatResponse response = parse_grok_responses_response(
            nlohmann::json::parse(upstream.text));
        if (response.provider_error.has_error()) {
            response.provider_error.model = model_;
            response.provider_error.request_id = request_id;
            response.provider_error.raw_body =
                redact_grok_auth_diagnostic(response.provider_error.raw_body);
            response.provider_error.pretty_json =
                redact_grok_auth_diagnostic(response.provider_error.pretty_json);
            response.provider_error.display_message =
                redact_grok_auth_diagnostic(response.provider_error.display_message);
        }
        return response;
    } catch (const nlohmann::json::parse_error& error) {
        return grok_error_response(grok_error_info(
            ProviderErrorKind::MalformedJson,
            static_cast<int>(upstream.status_code), model_, request_id,
            "Could not parse Grok response: " + std::string(error.what()),
            upstream.text, false, upstream.header));
    }
}

void GrokProvider::chat_stream(
        const std::vector<ChatMessage>& messages,
        const std::vector<ToolDef>& tools,
        const StreamCallback& callback,
        std::atomic<bool>* abort_flag) {
    std::string conversion_error;
    nlohmann::json body = build_grok_responses_request(
        build_request_body(messages, tools, true), &messages,
        &conversion_error);
    if (!conversion_error.empty()) {
        emit_grok_error(callback, grok_error_info(
            ProviderErrorKind::Unknown, 0, model_, "",
            "Could not build Grok request: " + conversion_error, "", false));
        return;
    }
    body["prompt_cache_key"] = session_id_;

    GrokAccessTokenResult access = ensure_grok_access_token(
        false, "", auth_config_);
    if (!access.ok) {
        emit_grok_error(callback, grok_error_info(
            ProviderErrorKind::Unknown, access.status_code, model_, "",
            access.message.empty() ? access.error : access.message, "", false));
        return;
    }

    const std::string url = responses_url();
    for (int physical_attempt = 0; physical_attempt < 2; ++physical_attempt) {
        const std::string request_id = generate_uuid();
        GrokResponsesStreamParser parser;
        std::string sse_buffer;
        std::string raw_body;
        bool parse_failed = false;
        std::string parse_error;
        std::atomic<std::int64_t> last_activity{steady_now_ms()};
        std::atomic<bool> idle_timeout{false};
        const int idle_timeout_ms = (std::max)(1, stream_timeout_ms_);

        auto write = cpr::WriteCallback{
            [&](const std::string_view bytes, intptr_t) -> bool {
                if (abort_flag && abort_flag->load()) return false;
                if (!bytes.empty()) last_activity.store(steady_now_ms());
                raw_body.append(bytes.data(), bytes.size());
                sse_buffer.append(bytes.data(), bytes.size());
                std::size_t position = 0;
                std::size_t delimiter = 0;
                while (find_event_delimiter(
                           sse_buffer, position, delimiter)) {
                    const std::string block = sse_buffer.substr(0, position);
                    sse_buffer.erase(0, position + delimiter);
                    const std::string data = sse_data(block);
                    if (data.empty() || data == "[DONE]") continue;
                    try {
                        const auto parsed_event = nlohmann::json::parse(data);
                        for (auto event : parser.consume(parsed_event)) {
                            if (event.type == StreamEventType::Error) {
                                event.provider_error.model = model_;
                                event.provider_error.request_id = request_id;
                            }
                            callback(event);
                        }
                    } catch (const nlohmann::json::parse_error& error) {
                        parse_failed = true;
                        parse_error = error.what();
                        return false;
                    }
                }
                return true;
            }};
        auto progress = cpr::ProgressCallback{
            [&](cpr::cpr_off_t, cpr::cpr_off_t,
                cpr::cpr_off_t, cpr::cpr_off_t, intptr_t) -> bool {
                if (abort_flag && abort_flag->load()) return false;
                if (steady_now_ms() - last_activity.load() >= idle_timeout_ms) {
                    idle_timeout.store(true);
                    return false;
                }
                return true;
            }};

        auto proxy = network::proxy_options_for(url);
        const cpr::Response upstream = cpr::Post(
            cpr::Url{url},
            grok_headers(auth_config_, access.tokens, model_, agent_id_,
                         session_id_, request_id, true),
            cpr::Body{body.dump()},
            cpr::ConnectTimeout{
                (std::min)(idle_timeout_ms, kStreamConnectTimeoutCapMs)},
            network::build_ssl_options(proxy),
            proxy.proxies,
            proxy.auth,
            write,
            progress);

        if (abort_flag && abort_flag->load()) {
            emit_grok_error(callback, grok_error_info(
                ProviderErrorKind::UserCancelled, 0, model_, request_id,
                "Grok request cancelled", "", false));
            return;
        }
        if (upstream.status_code == 401 && physical_attempt == 0) {
            GrokAccessTokenResult refreshed = ensure_grok_access_token(
                true, access.tokens.access_token, auth_config_);
            if (!refreshed.ok) {
                emit_grok_error(callback, grok_error_info(
                    ProviderErrorKind::Http, 401, model_, request_id,
                    refreshed.message.empty() ? refreshed.error : refreshed.message,
                    upstream.text.empty() ? raw_body : upstream.text,
                    false, upstream.header));
                return;
            }
            access = std::move(refreshed);
            continue;
        }
        if (parse_failed) {
            emit_grok_error(callback, grok_error_info(
                ProviderErrorKind::MalformedJson, 200, model_, request_id,
                "Could not parse Grok SSE event: " + parse_error,
                raw_body, false, upstream.header));
            return;
        }
        if (upstream.status_code == 0 || idle_timeout.load()) {
            const ProviderErrorKind kind = idle_timeout.load()
                ? ProviderErrorKind::Timeout
                : classify_cpr_error(upstream.error);
            emit_grok_error(callback, grok_error_info(
                kind, 0, model_, request_id,
                idle_timeout.load()
                    ? "Grok stream idle timeout"
                    : (upstream.error.message.empty()
                        ? "Grok stream connection failed"
                        : upstream.error.message),
                raw_body, true, upstream.header));
            return;
        }
        if (upstream.status_code < 200 || upstream.status_code >= 300) {
            const int status = static_cast<int>(upstream.status_code);
            const std::string body_text = upstream.text.empty()
                ? raw_body
                : upstream.text;
            emit_grok_error(callback, grok_error_info(
                ProviderErrorKind::Http, status, model_, request_id,
                "Grok Build returned HTTP " + std::to_string(status),
                body_text,
                provider_http_error_is_retryable(status, body_text),
                upstream.header));
            return;
        }
        if (!parser.terminal()) {
            for (auto event : parser.finish()) {
                if (event.type == StreamEventType::Error) {
                    event.provider_error.model = model_;
                    event.provider_error.request_id = request_id;
                    event.provider_error.raw_body =
                        redact_grok_auth_diagnostic(raw_body);
                }
                callback(event);
            }
        }
        return;
    }
}

} // namespace acecode
