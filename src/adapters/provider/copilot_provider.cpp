#include "copilot_provider.hpp"
#include "retry_policy.hpp"
#include "utils/logger.hpp"
#include "network/proxy_resolver.hpp"
#include <cpr/cpr.h>
#include <cpr/ssl_options.h>
#include <algorithm>
#include <cctype>
#include <ctime>
#include <map>
#include <utility>

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

CopilotProvider::CopilotProvider(const std::string& model,
                                 ProviderRequestOptions request_options)
    : OpenAiCompatProvider(COPILOT_CHAT_URL, "", model,
                           OpenAiConfig::kDefaultStreamTimeoutMs, {},
                           std::move(request_options)) {}

bool CopilotProvider::is_authenticated() {
    return !copilot_token_snapshot().empty();
}

bool CopilotProvider::try_silent_auth() {
    set_github_token(load_github_token());
    return !copilot_token_snapshot().empty();
}

bool CopilotProvider::run_device_flow(std::function<void(const std::string&)> status_callback) {
    device_code_ = request_device_code();
    if (device_code_.device_code.empty()) {
        if (status_callback) status_callback("Failed to request device code.");
        return false;
    }

    // The caller (TUI) should display device_code_.user_code and verification_uri
    // before we start polling.

    auto github_token = poll_for_access_token(
        device_code_.device_code,
        device_code_.interval,
        device_code_.expires_in,
        status_callback
    );

    set_github_token(github_token);
    if (github_token.empty()) {
        return false;
    }

    save_github_token(github_token);
    return !copilot_token_snapshot().empty();
}

bool CopilotProvider::authenticate() {
    if (try_silent_auth()) {
        return true;
    }
    return run_device_flow();
}

void CopilotProvider::set_github_token(std::string token) {
    std::lock_guard<std::mutex> lock(token_mu_);
    if (github_token_ != token) copilot_token_ = {};
    github_token_ = std::move(token);
}

std::string CopilotProvider::copilot_token_snapshot(const std::string& rejected_token) {
    std::lock_guard<std::mutex> lock(token_mu_);
    if (github_token_.empty()) return {};
    // A late 401 from one request must not invalidate the replacement another
    // request has already installed while that first request was in flight.
    if (!rejected_token.empty() && copilot_token_.token == rejected_token) {
        copilot_token_ = {};
    }
    // Check if we already have a valid (non-expired) copilot token
    if (!copilot_token_.token.empty()) {
        int64_t now = static_cast<int64_t>(std::time(nullptr));
        if (now < copilot_token_.expires_at - 60) { // 60s margin
            LOG_DEBUG("Copilot token still valid, expires_at=" + std::to_string(copilot_token_.expires_at));
            return copilot_token_.token;
        }
    }

    LOG_INFO("Exchanging copilot token...");
    // Exchange for a new copilot token
    copilot_token_ = exchange_copilot_token(github_token_);
    LOG_INFO("Copilot token exchange result: " + std::string(copilot_token_.token.empty() ? "FAILED" : "OK"));
    return copilot_token_.token;
}

ChatResponse CopilotProvider::chat(
    const std::vector<ChatMessage>& messages,
    const std::vector<ToolDef>& tools
) {
    return chat_cancellable(messages, tools, nullptr);
}

ChatResponse CopilotProvider::chat_cancellable(
    const std::vector<ChatMessage>& messages, const std::vector<ToolDef>& tools,
    const std::atomic<bool>* abort_flag) {
    const auto cancelled = [&] { return abort_flag && abort_flag->load(); };
    const auto interrupted = [&] {
        return make_copilot_error(ProviderErrorKind::UserCancelled, 0, model_,
                                 "[Interrupted]", {}, false);
    };
    if (cancelled()) return interrupted();
    const auto token = copilot_token_snapshot();
    if (token.empty()) {
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
        {"Authorization", "Bearer " + token},
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
        cpr::Timeout{stream_timeout_ms_},
        cpr::ProgressCallback{[abort_flag](cpr::cpr_off_t, cpr::cpr_off_t,
                                          cpr::cpr_off_t, cpr::cpr_off_t, intptr_t) {
            return !abort_flag || !abort_flag->load();
        }}
    );

    if (cancelled()) return interrupted();

    if (r.status_code == 401) {
        // Token expired, try refresh once
        const auto refreshed_token = copilot_token_snapshot(token);
        if (!refreshed_token.empty()) {
            headers["Authorization"] = "Bearer " + refreshed_token;
            auto proxy_opts2 = network::proxy_options_for(COPILOT_CHAT_URL);
            r = cpr::Post(
                cpr::Url{COPILOT_CHAT_URL},
                headers,
                cpr::Body{body.dump()},
                network::build_ssl_options(proxy_opts2),
                proxy_opts2.proxies,
                proxy_opts2.auth,
                cpr::Timeout{stream_timeout_ms_},
                cpr::ProgressCallback{[abort_flag](cpr::cpr_off_t, cpr::cpr_off_t,
                                                  cpr::cpr_off_t, cpr::cpr_off_t, intptr_t) {
                    return !abort_flag || !abort_flag->load();
                }}
            );
        }
    }

    if (cancelled()) return interrupted();
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
    const auto token = copilot_token_snapshot();
    if (token.empty()) {
        LOG_ERROR("Copilot token unavailable for streaming");
        StreamEvent evt;
        evt.type = StreamEventType::Error;
        evt.error = "Copilot session token unavailable. Re-authenticate.";
        callback(evt);
        return;
    }

    nlohmann::json body = build_request_body(messages, tools, true);

    std::map<std::string, std::string> extra_headers = {
        {"Authorization", "Bearer " + token},
        {"Editor-Version", "acecode/0.1.0"},
        {"Editor-Plugin-Version", "acecode/0.1.0"},
        {"Copilot-Integration-Id", "vscode-chat"},
        {"Openai-Intent", "conversation-panel"}
    };

    // 走 OpenAiCompatProvider::parse_sse_stream,所以 DSML 与文本形式工具调用
    // 恢复(扣住标记、Done 上报 text_tool_calls 诊断)在 Copilot 流式路径上同样
    // 生效。Copilot 的非流式 chat() 是单独实现,刻意不接:AgentLoop 主循环只走
    // 流式,非流式只用于压缩 / 标题生成这类不带工具的请求,本来就不会构造过滤器。
    parse_sse_stream(COPILOT_CHAT_URL, body, extra_headers, callback, abort_flag);
}

} // namespace acecode
