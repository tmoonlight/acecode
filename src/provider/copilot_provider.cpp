#include "copilot_provider.hpp"
#include "utils/logger.hpp"
#include "network/proxy_resolver.hpp"
#include <cpr/cpr.h>
#include <cpr/ssl_options.h>
#include <algorithm>
#include <cctype>
#include <ctime>
#include <map>

namespace acecode {
namespace {

std::string ascii_lower(std::string value) {
    std::transform(
        value.begin(), value.end(), value.begin(),
        [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
    return value;
}

std::string header_value_ci(const cpr::Header& headers,
                            const std::string& key) {
    const std::string wanted = ascii_lower(key);
    for (const auto& [header_key, value] : headers) {
        if (ascii_lower(header_key) == wanted) return value;
    }
    return {};
}

ProviderErrorKind classify_cpr_error(const cpr::Error& error) {
    if (error.code == cpr::ErrorCode::OPERATION_TIMEDOUT) {
        return ProviderErrorKind::Timeout;
    }
    const std::string message = ascii_lower(error.message);
    if (message.find("timed out") != std::string::npos ||
        message.find("timeout") != std::string::npos) {
        return ProviderErrorKind::Timeout;
    }
    return ProviderErrorKind::Network;
}

ChatResponse make_copilot_error(
    ProviderErrorKind kind,
    int status_code,
    const std::string& model,
    const std::string& message,
    const std::string& raw_body,
    bool retryable,
    const cpr::Header& headers = {}) {
    ProviderErrorInfo info;
    info.kind = kind;
    info.status_code = status_code;
    info.provider = "copilot";
    info.model = model;
    info.display_message = message;
    info.raw_body = raw_body;
    info.retryable = retryable;
    const std::string retry_after = header_value_ci(headers, "retry-after");
    if (!retry_after.empty()) {
        if (const auto parsed = parse_retry_after_ms(retry_after)) {
            info.server_retry_after_ms = *parsed;
        }
    }

    ChatResponse response;
    response.content = "[Error] " + message;
    response.finish_reason = "error";
    response.provider_error = std::move(info);
    return response;
}

} // namespace

static const std::string COPILOT_CHAT_URL = "https://api.githubcopilot.com/chat/completions";

CopilotProvider::CopilotProvider(const std::string& model)
    : OpenAiCompatProvider(COPILOT_CHAT_URL, "", model) {}

bool CopilotProvider::is_authenticated() {
    return !github_token_.empty() && ensure_copilot_token();
}

bool CopilotProvider::try_silent_auth() {
    github_token_ = load_github_token();
    if (github_token_.empty()) {
        return false;
    }
    return ensure_copilot_token();
}

bool CopilotProvider::run_device_flow(std::function<void(const std::string&)> status_callback) {
    device_code_ = request_device_code();
    if (device_code_.device_code.empty()) {
        if (status_callback) status_callback("Failed to request device code.");
        return false;
    }

    // The caller (TUI) should display device_code_.user_code and verification_uri
    // before we start polling.

    github_token_ = poll_for_access_token(
        device_code_.device_code,
        device_code_.interval,
        device_code_.expires_in,
        status_callback
    );

    if (github_token_.empty()) {
        return false;
    }

    save_github_token(github_token_);
    return ensure_copilot_token();
}

bool CopilotProvider::authenticate() {
    if (try_silent_auth()) {
        return true;
    }
    return run_device_flow();
}

bool CopilotProvider::ensure_copilot_token() {
    // Check if we already have a valid (non-expired) copilot token
    if (!copilot_token_.token.empty()) {
        int64_t now = static_cast<int64_t>(std::time(nullptr));
        if (now < copilot_token_.expires_at - 60) { // 60s margin
            LOG_DEBUG("Copilot token still valid, expires_at=" + std::to_string(copilot_token_.expires_at));
            return true;
        }
    }

    LOG_INFO("Exchanging copilot token...");
    // Exchange for a new copilot token
    copilot_token_ = exchange_copilot_token(github_token_);
    LOG_INFO("Copilot token exchange result: " + std::string(copilot_token_.token.empty() ? "FAILED" : "OK"));
    return !copilot_token_.token.empty();
}

ChatResponse CopilotProvider::chat(
    const std::vector<ChatMessage>& messages,
    const std::vector<ToolDef>& tools
) {
    if (!ensure_copilot_token()) {
        return make_copilot_error(
            ProviderErrorKind::Unknown,
            0,
            model_,
            "Copilot session token unavailable. Re-authenticate.",
            std::string{},
            false);
    }

    nlohmann::json body = build_request_body(messages, tools);

    cpr::Header headers = {
        {"Content-Type", "application/json"},
        {"Authorization", "Bearer " + copilot_token_.token},
        {"Editor-Version", "acecode/0.1.0"},
        {"Editor-Plugin-Version", "acecode/0.1.0"},
        {"Copilot-Integration-Id", "vscode-chat"},
        {"Openai-Intent", "conversation-panel"}
    };

    auto proxy_opts = network::proxy_options_for(COPILOT_CHAT_URL);
    cpr::Response r = cpr::Post(
        cpr::Url{COPILOT_CHAT_URL},
        headers,
        cpr::Body{body.dump()},
        network::build_ssl_options(proxy_opts),
        proxy_opts.proxies,
        proxy_opts.auth,
        cpr::Timeout{stream_timeout_ms_}
    );

    if (r.status_code == 401) {
        // Token expired, try refresh once
        copilot_token_ = {};
        if (ensure_copilot_token()) {
            headers["Authorization"] = "Bearer " + copilot_token_.token;
            auto proxy_opts2 = network::proxy_options_for(COPILOT_CHAT_URL);
            r = cpr::Post(
                cpr::Url{COPILOT_CHAT_URL},
                headers,
                cpr::Body{body.dump()},
                network::build_ssl_options(proxy_opts2),
                proxy_opts2.proxies,
                proxy_opts2.auth,
                cpr::Timeout{stream_timeout_ms_}
            );
        }
    }

    if (r.status_code == 0) {
        const ProviderErrorKind kind = classify_cpr_error(r.error);
        return make_copilot_error(
            kind,
            0,
            model_,
            (kind == ProviderErrorKind::Timeout
                 ? "Copilot request timed out: "
                 : "Copilot connection failed: ") +
                r.error.message,
            r.text,
            true,
            r.header);
    }

    if (r.status_code != 200) {
        const int status_code = static_cast<int>(r.status_code);
        return make_copilot_error(
            ProviderErrorKind::Http,
            status_code,
            model_,
            "Copilot HTTP " + std::to_string(status_code) + ": " + r.text,
            r.text,
            provider_http_error_is_retryable(status_code, r.text),
            r.header);
    }

    try {
        nlohmann::json response_json = nlohmann::json::parse(r.text);
        return parse_response(response_json);
    } catch (const nlohmann::json::parse_error& e) {
        return make_copilot_error(
            ProviderErrorKind::MalformedJson,
            200,
            model_,
            "Failed to parse Copilot response: " + std::string(e.what()),
            r.text,
            false,
            r.header);
    }
}

void CopilotProvider::chat_stream(
    const std::vector<ChatMessage>& messages,
    const std::vector<ToolDef>& tools,
    const StreamCallback& callback,
    std::atomic<bool>* abort_flag
) {
    LOG_INFO("CopilotProvider::chat_stream messages=" + std::to_string(messages.size()) + " tools=" + std::to_string(tools.size()));
    if (!ensure_copilot_token()) {
        LOG_ERROR("Copilot token unavailable for streaming");
        StreamEvent evt;
        evt.type = StreamEventType::Error;
        evt.error = "Copilot session token unavailable. Re-authenticate.";
        callback(evt);
        return;
    }

    nlohmann::json body = build_request_body(messages, tools, true);

    std::map<std::string, std::string> extra_headers = {
        {"Authorization", "Bearer " + copilot_token_.token},
        {"Editor-Version", "acecode/0.1.0"},
        {"Editor-Plugin-Version", "acecode/0.1.0"},
        {"Copilot-Integration-Id", "vscode-chat"},
        {"Openai-Intent", "conversation-panel"}
    };

    parse_sse_stream(COPILOT_CHAT_URL, body, extra_headers, callback, abort_flag);
}

} // namespace acecode
