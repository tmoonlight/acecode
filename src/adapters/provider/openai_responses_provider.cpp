#include "openai_responses_provider.hpp"

#include "openai_responses.hpp"
#include "retry_policy.hpp"
#include "stream_diagnostic_capture.hpp"
#include "config/request_headers.hpp"
#include "network/proxy_resolver.hpp"

#include <cpr/cpr.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <limits>
#include <optional>
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
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::string header_value(const cpr::Header& headers, const std::string& key) {
    for (const auto& [name, value] : headers) {
        if (ascii_lower(name) == key) return value;
    }
    return {};
}

void enrich_error(ProviderErrorInfo& info, const std::string& model,
    const cpr::Header& headers) {
    info.provider = "openai";
    info.model = model;
    for (const char* key : {"x-request-id", "request-id", "x-ms-request-id", "cf-ray"}) {
        const auto value = header_value(headers, key);
        if (!value.empty()) {
            info.request_id = value;
            break;
        }
    }
    if (const auto delay = parse_retry_after_ms(header_value(headers, "retry-after"))) {
        info.server_retry_after_ms = *delay;
    }
    if (provider_error_body_has_hard_quota(info.raw_body)) info.retryable = false;
}

ProviderErrorInfo request_error(ProviderErrorKind kind, int status,
    const std::string& model, const std::string& message,
    const std::string& raw_body = {}, bool retryable = false,
    const cpr::Header& headers = {}) {
    ProviderErrorInfo info;
    info.kind = kind;
    info.status_code = status;
    info.display_message = message;
    info.raw_body = raw_body;
    info.retryable = retryable;
    const auto parsed = nlohmann::json::parse(raw_body, nullptr, false);
    if (!parsed.is_discarded()) {
        info.body_is_json = true;
        info.pretty_json = parsed.dump(2);
    }
    enrich_error(info, model, headers);
    if (!raw_body.empty()) {
        info.display_message += "\n" +
            (info.body_is_json ? info.pretty_json : raw_body);
    }
    return info;
}

ProviderErrorKind transport_error_kind(const cpr::Error& error) {
    const auto message = ascii_lower(error.message);
    return error.code == cpr::ErrorCode::OPERATION_TIMEDOUT ||
        message.find("timeout") != std::string::npos ||
        message.find("timed out") != std::string::npos
        ? ProviderErrorKind::Timeout : ProviderErrorKind::Network;
}

ChatResponse failed_response(ProviderErrorInfo info) {
    ChatResponse response;
    response.content = "[Error] " + info.display_message;
    response.finish_reason = "error";
    response.provider_error = std::move(info);
    return response;
}

void emit_error(const StreamCallback& callback, const ProviderErrorInfo& info,
    StreamEventType type = StreamEventType::Error) {
    StreamEvent event;
    event.type = type;
    event.provider_error = info;
    event.error = info.display_message;
    callback(event);
}

std::optional<cpr::Header> request_headers(const std::string& api_key,
    const std::map<std::string, std::string>& configured, bool stream,
    std::string& error) {
    const auto resolved = resolve_request_headers(configured, error);
    if (!resolved) return std::nullopt;
    cpr::Header headers{{"Content-Type", "application/json"},
        {"Accept", stream ? "text/event-stream" : "application/json"}};
    if (stream) headers["Accept-Encoding"] = "identity";
    if (!api_key.empty()) headers["Authorization"] = "Bearer " + api_key;
    for (const auto& [name, value] : *resolved) headers[name] = value;
    return headers;
}

bool event_delimiter(const std::string& buffer, std::size_t& position,
    std::size_t& length) {
    const auto lf = buffer.find("\n\n");
    const auto crlf = buffer.find("\r\n\r\n");
    if (lf == std::string::npos && crlf == std::string::npos) return false;
    if (crlf != std::string::npos && (lf == std::string::npos || crlf < lf)) {
        position = crlf;
        length = 4;
    } else {
        position = lf;
        length = 2;
    }
    return true;
}

std::string event_data(const std::string& block) {
    std::istringstream input(block);
    std::string line;
    std::string data;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.compare(0, 5, "data:") != 0) continue;
        const auto start = line.size() > 5 && line[5] == ' ' ? 6u : 5u;
        if (!data.empty()) data.push_back('\n');
        data += line.substr(start);
    }
    return data;
}

} // namespace

std::string OpenAiResponsesProvider::request_url() const {
    return request_options_.endpoint_mode == "full_url"
        ? base_url_ : base_url_ + "/responses";
}

bool OpenAiResponsesProvider::supports_prompt_cache_key() const {
    const auto url = request_url();
    if (ascii_lower(url.substr(0, 8)) != "https://") return false;
    const auto end = url.find('/', 8);
    if (end == std::string::npos) return false;
    auto authority = ascii_lower(url.substr(8, end - 8));
    if (authority.size() > 4 && authority.compare(authority.size() - 4, 4, ":443") == 0) {
        authority.resize(authority.size() - 4);
    }
    return authority == "api.openai.com" &&
        url.substr(end, url.find_first_of("?#", end) - end) == "/v1/responses";
}

nlohmann::json OpenAiResponsesProvider::responses_body(
    const std::vector<ChatMessage>& messages, const std::vector<ToolDef>& tools,
    bool stream, bool for_compaction, const ChatRequestOptions* options,
    std::string& error) const {
    auto normalized = build_request_body(messages, tools, stream, for_compaction, options);
    // Only an explicit profile choice disables reasoning. Legacy profiles with
    // no toggle keep the model's default, including mandatory-reasoning models.
    if (request_options_.reasoning && request_options_.reasoning->supported &&
        !request_options_.reasoning->mandatory &&
        request_options_.reasoning->enabled == std::optional<bool>(false)) {
        normalized["reasoning_effort"] = "none";
    }
    if (request_options_.reasoning && request_options_.reasoning->supported &&
        (request_options_.reasoning->mandatory ||
         request_options_.reasoning->enabled.value_or(request_options_.reasoning->default_enabled))) {
        normalized["reasoning"]["summary"] = "auto";
    }
    return build_openai_responses_request(normalized, &messages, &error);
}

ChatResponse OpenAiResponsesProvider::chat(const std::vector<ChatMessage>& messages,
    const std::vector<ToolDef>& tools) {
    return chat_impl(messages, tools, nullptr, false, nullptr);
}

ChatResponse OpenAiResponsesProvider::chat_cancellable(
    const std::vector<ChatMessage>& messages, const std::vector<ToolDef>& tools,
    const std::atomic<bool>* abort_flag) {
    return chat_impl(messages, tools, abort_flag, false, nullptr);
}

ChatResponse OpenAiResponsesProvider::chat_for_compaction(
    const std::vector<ChatMessage>& messages, const std::vector<ToolDef>& tools,
    const std::atomic<bool>* abort_flag) {
    return chat_impl(messages, tools, abort_flag, true, nullptr);
}

ChatResponse OpenAiResponsesProvider::chat_with_options(
    const std::vector<ChatMessage>& messages, const std::vector<ToolDef>& tools,
    const ChatRequestOptions& options, const std::atomic<bool>* abort_flag) {
    return chat_impl(messages, tools, abort_flag, options.for_compaction, &options);
}

ChatResponse OpenAiResponsesProvider::chat_impl(
    const std::vector<ChatMessage>& messages, const std::vector<ToolDef>& tools,
    const std::atomic<bool>* abort_flag, bool for_compaction,
    const ChatRequestOptions* options) {
    const auto cancelled = [abort_flag] { return abort_flag && abort_flag->load(); };
    const auto cancellation = [&] {
        return failed_response(request_error(ProviderErrorKind::UserCancelled,
            0, model_, "Request cancelled"));
    };
    if (cancelled()) return cancellation();
    std::string error;
    auto body = responses_body(messages, tools, false, for_compaction, options, error);
    if (!error.empty()) {
        return failed_response(request_error(ProviderErrorKind::Unknown,
            0, model_, "Could not build Responses request: " + error));
    }
    const auto headers = request_headers(api_key_, request_headers_, false, error);
    if (!headers) {
        return failed_response(request_error(ProviderErrorKind::Unknown, 0, model_, error));
    }
    const auto url = request_url();
    cpr::Response upstream;
    do {
        if (cancelled()) return cancellation();
        const auto proxy = network::proxy_options_for(url);
        upstream = cpr::Post(cpr::Url{url}, *headers, cpr::Body{body.dump()},
            network::build_ssl_options(proxy), proxy.proxies, proxy.auth,
            cpr::Timeout{stream_timeout_ms_},
            cpr::ProgressCallback{[abort_flag](cpr::cpr_off_t, cpr::cpr_off_t,
                cpr::cpr_off_t, cpr::cpr_off_t, intptr_t) {
                return !abort_flag || !abort_flag->load();
            }});
    } while (!cancelled() && remove_rejected_prompt_cache_key(body, url,
        static_cast<int>(upstream.status_code), upstream.text));
    if (cancelled()) return cancellation();
    const auto status = static_cast<int>(upstream.status_code);
    if (status == 0 || (upstream.error && status >= 200 && status < 300)) {
        return failed_response(request_error(transport_error_kind(upstream.error),
            status, model_, "Responses request failed: " + upstream.error.message,
            upstream.text, true, upstream.header));
    }
    if (status < 200 || status >= 300) {
        return failed_response(request_error(ProviderErrorKind::Http, status,
            model_, "Responses returned HTTP " + std::to_string(status),
            upstream.text, provider_http_error_is_retryable(status, upstream.text),
            upstream.header));
    }
    try {
        auto response = parse_openai_responses_response(nlohmann::json::parse(upstream.text));
        if (response.provider_error.has_error()) {
            enrich_error(response.provider_error, model_, upstream.header);
        }
        return response;
    } catch (const nlohmann::json::exception& failure) {
        return failed_response(request_error(ProviderErrorKind::MalformedJson,
            status, model_, "Could not parse Responses response: " +
                std::string(failure.what()), upstream.text, false, upstream.header));
    }
}

void OpenAiResponsesProvider::chat_stream(const std::vector<ChatMessage>& messages,
    const std::vector<ToolDef>& tools, const StreamCallback& callback,
    std::atomic<bool>* abort_flag) {
    stream_impl(messages, tools, callback, abort_flag, nullptr);
}

void OpenAiResponsesProvider::chat_stream_with_options(
    const std::vector<ChatMessage>& messages, const std::vector<ToolDef>& tools,
    const ChatRequestOptions& options, const StreamCallback& callback,
    std::atomic<bool>* abort_flag) {
    stream_impl(messages, tools, callback, abort_flag, &options);
}

void OpenAiResponsesProvider::stream_impl(const std::vector<ChatMessage>& messages,
    const std::vector<ToolDef>& tools, const StreamCallback& callback,
    std::atomic<bool>* abort_flag, const ChatRequestOptions* options) {
    // Nullable synchronous borrow: the public call does not return until Post
    // and all registered callbacks have finished using this cancellation flag.
    const auto cancelled = [abort_flag] { return abort_flag && abort_flag->load(); };
    const auto emit_cancellation = [&] {
        emit_error(callback, request_error(ProviderErrorKind::UserCancelled,
            0, model_, "Request cancelled"));
    };
    if (cancelled()) {
        emit_cancellation();
        return;
    }
    std::string error;
    auto body = responses_body(messages, tools, true,
        options && options->for_compaction, options, error);
    if (!error.empty()) {
        emit_error(callback, request_error(ProviderErrorKind::Unknown,
            0, model_, "Could not build Responses request: " + error));
        return;
    }
    const auto headers = request_headers(api_key_, request_headers_, true, error);
    if (!headers) {
        emit_error(callback, request_error(ProviderErrorKind::Unknown, 0, model_, error));
        return;
    }
    const auto url = request_url();
    std::uint64_t retry_number = 0;
    for (;;) {
        if (cancelled()) {
            emit_cancellation();
            return;
        }
        OpenAiResponsesStreamParser parser;
        StreamDiagnosticCapture capture;
        std::string buffer;
        ProviderErrorInfo parser_error;
        std::vector<StreamEvent> completion_events;
        bool saw_valid_frame = false;
        bool emitted_provisional_output = false;
        bool malformed_json = false;
        std::string parse_error;
        std::atomic<std::int64_t> last_activity{steady_now_ms()};
        std::atomic<bool> idle_timeout{false};
        const auto timeout_ms = (std::max)(1, stream_timeout_ms_);
        const auto consume = [&parser_error, &completion_events,
            &emitted_provisional_output, callback, cancelled](std::vector<StreamEvent> events) {
            for (auto& event : events) {
                if (event.type == StreamEventType::Error) {
                    parser_error = std::move(event.provider_error);
                } else if (event.type == StreamEventType::ToolCall ||
                           event.type == StreamEventType::Done ||
                           event.type == StreamEventType::Usage) {
                    // Never hand executable calls or successful completion to
                    // the agent until transport integrity is known.
                    completion_events.push_back(std::move(event));
                } else {
                    emitted_provisional_output = true;
                    callback(event);
                }
                if (cancelled()) break;
            }
        };
        cpr::Response upstream;
        {
            // CPR Post invokes these callbacks synchronously. Every reference
            // below borrows this attempt's stack state; the nested Session is
            // destroyed (releasing its callback closures) before that state, on
            // normal return, cancellation, or exception. Session itself is a
            // call-local borrow used only for HTTP status; no callback owns it.
            cpr::Session session;
            const auto write = cpr::WriteCallback{
                [&session, &capture, &buffer, &parser, &parser_error,
                 &saw_valid_frame, &malformed_json, &parse_error, &last_activity,
                 &consume, cancelled](const std::string_view bytes, intptr_t) -> bool {
                    if (cancelled()) return false;
                    if (!bytes.empty()) last_activity.store(steady_now_ms());
                    long status = 0;
                    curl_easy_getinfo(session.GetCurlHolder()->handle,
                        CURLINFO_RESPONSE_CODE, &status);
                    capture.append(bytes, status);
                    // HTTP errors are JSON/text bodies, not malformed SSE.
                    if (status < 200 || status >= 300) return true;
                    buffer.append(bytes.data(), bytes.size());
                    std::size_t position = 0;
                    std::size_t delimiter_length = 0;
                    while (event_delimiter(buffer, position, delimiter_length)) {
                        const auto data = event_data(buffer.substr(0, position));
                        buffer.erase(0, position + delimiter_length);
                        if (data.empty()) continue;
                        if (data == "[DONE]") {
                            consume(parser.finish());
                        } else {
                            try {
                                const auto frame = nlohmann::json::parse(data);
                                saw_valid_frame = true;
                                consume(parser.consume(frame));
                            } catch (const nlohmann::json::exception& failure) {
                                malformed_json = true;
                                parse_error = failure.what();
                                return false;
                            }
                        }
                        if (cancelled() || parser_error.has_error()) return false;
                    }
                    return !cancelled();
                }};
            const auto progress = cpr::ProgressCallback{
                [&last_activity, &idle_timeout, timeout_ms, cancelled](
                    cpr::cpr_off_t, cpr::cpr_off_t, cpr::cpr_off_t,
                    cpr::cpr_off_t, intptr_t) {
                    if (cancelled()) return false;
                    if (steady_now_ms() - last_activity.load() >= timeout_ms) {
                        idle_timeout.store(true);
                        return false;
                    }
                    return true;
                }};
            const auto proxy = network::proxy_options_for(url);
            session.SetOption(cpr::Url{url});
            session.SetOption(*headers);
            session.SetOption(cpr::Body{body.dump()});
            session.SetOption(cpr::ConnectTimeout{(std::min)(timeout_ms, kStreamConnectTimeoutCapMs)});
            session.SetOption(network::build_ssl_options(proxy));
            session.SetOption(proxy.proxies);
            session.SetOption(proxy.auth);
            session.SetOption(write);
            session.SetOption(progress);
            upstream = session.Post();
        }
        if (cancelled()) {
            emit_cancellation();
            return;
        }
        const auto status = static_cast<int>(upstream.status_code);
        const auto raw_body = upstream.text.empty() ? capture.str() : upstream.text;
        ProviderErrorInfo failure;
        if (status != 0 && (status < 200 || status >= 300)) {
            failure = request_error(ProviderErrorKind::Http, status, model_,
                "Responses returned HTTP " + std::to_string(status), raw_body,
                provider_http_error_is_retryable(status, raw_body), upstream.header);
        } else if (parser_error.has_error()) {
            failure = std::move(parser_error);
            enrich_error(failure, model_, upstream.header);
        } else if (malformed_json) {
            failure = request_error(saw_valid_frame ? ProviderErrorKind::MalformedSse :
                ProviderErrorKind::MalformedJson, status, model_,
                "Could not parse Responses SSE event: " + parse_error,
                raw_body, saw_valid_frame && !parser.terminal(), upstream.header);
        } else if (idle_timeout.load() || status == 0 || upstream.error) {
            failure = request_error(idle_timeout.load() ? ProviderErrorKind::Timeout :
                transport_error_kind(upstream.error), status, model_,
                idle_timeout.load() ? "Responses stream idle timeout" :
                    "Responses stream failed: " + upstream.error.message,
                raw_body, true, upstream.header);
        } else {
            consume(parser.finish());
            if (parser_error.has_error()) {
                failure = std::move(parser_error);
                enrich_error(failure, model_, upstream.header);
            } else {
                for (const auto& event : completion_events) {
                    if (cancelled()) {
                        emit_cancellation();
                        return;
                    }
                    callback(event);
                }
                return;
            }
        }
        // finish() has no event payload; retain the bounded stream capture so
        // a truncated response does not produce an unhelpful empty object.
        if (failure.raw_body.empty() || failure.raw_body == "{}") {
            failure.raw_body = raw_body;
            failure.body_is_json = false;
            failure.pretty_json.clear();
        }
        if (!emitted_provisional_output && completion_events.empty() &&
            remove_rejected_prompt_cache_key(body, url,
            failure.status_code, failure.raw_body)) continue;
        if (!failure.retryable) {
            failure.retry_attempt = saturating_retry_attempt(retry_number);
            emit_error(callback, failure);
            return;
        }
        if (retry_number < (std::numeric_limits<std::uint64_t>::max)()) ++retry_number;
        const auto server_delay = failure.server_retry_after_ms >= 0
            ? std::optional<std::int64_t>(failure.server_retry_after_ms) : std::nullopt;
        const auto delay = provider_retry_delay_ms(retry_number, server_delay,
            provider_retry_max_delay_ms(failure.status_code, failure.raw_body));
        failure.retry_attempt = saturating_retry_attempt(retry_number);
        failure.retry_max_attempts = -1;
        failure.retry_delay_ms = static_cast<int>(delay);
        emit_error(callback, failure, StreamEventType::Retry);
        if (wait_for_retry(std::chrono::milliseconds(delay), abort_flag)) {
            emit_cancellation();
            return;
        }
        emit_error(callback, failure, StreamEventType::RetryResume);
    }
}

} // namespace acecode
