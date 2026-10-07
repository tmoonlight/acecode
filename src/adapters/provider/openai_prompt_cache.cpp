#include "openai_provider.hpp"
#include "utils/logger.hpp"
#include "config/request_headers.hpp"

#include <algorithm>
#include <cctype>

namespace acecode {
namespace {

std::string ascii_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string json_string_or_empty(const nlohmann::json& value, const char* key) {
    if (!value.is_object() || !value.contains(key) || !value[key].is_string()) return {};
    return value[key].get<std::string>();
}

bool known_cache_key_endpoint(const std::string& url) {
    const auto authority_end = url.find('/', 8);
    if (ascii_lower(url.substr(0, 8)) != "https://" ||
        authority_end == std::string::npos) return false;
    auto authority = ascii_lower(url.substr(8, authority_end - 8));
    if (authority.size() > 4 && authority.compare(authority.size() - 4, 4, ":443") == 0) {
        authority.resize(authority.size() - 4);
    }
    const auto path_end = url.find_first_of("?#", authority_end);
    const auto path = url.substr(authority_end, path_end - authority_end);
    return (authority == "api.openai.com" || authority == "api.mistral.ai") &&
        path == "/v1/chat/completions";
}

bool unsupported_cache_key_message(const std::string& message) {
    // Match an explicit field rejection, not a generic invalid request that
    // happens to echo the entire body (including prompt_cache_key).
    std::string normalized;
    for (unsigned char c : message) {
        if (std::isalnum(c) || c == '_') {
            normalized.push_back(static_cast<char>(std::tolower(c)));
        } else if (!normalized.empty() && normalized.back() != ' ') {
            normalized.push_back(' ');
        }
    }
    normalized = " " + normalized + " ";
    for (const char* pattern : {
             " unknown parameter prompt_cache_key ",
             " unsupported parameter prompt_cache_key ",
             " unrecognized parameter prompt_cache_key ",
             " unknown field prompt_cache_key ",
             " unsupported field prompt_cache_key ",
             " unrecognized field prompt_cache_key ",
             " unrecognized request argument supplied prompt_cache_key ",
             " unexpected keyword argument prompt_cache_key ",
             " prompt_cache_key is not supported ",
             " prompt_cache_key is unsupported ",
             " prompt_cache_key is not allowed "}) {
        if (normalized.find(pattern) != std::string::npos) return true;
    }
    return false;
}

bool explicit_cache_key_rejection(int status, const std::string& body) {
    if (status != 400 && status != 422) return false;
    const auto error = nlohmann::json::parse(body, nullptr, false);
    if (!error.is_object()) return unsupported_cache_key_message(body);
    const auto error_it = error.find("error");
    const auto& detail = error_it != error.end() && error_it->is_object()
        ? *error_it : error;
    const auto parameter = json_string_or_empty(detail, "param");
    const auto code = ascii_lower(json_string_or_empty(detail, "code"));
    if (!parameter.empty() && parameter != "prompt_cache_key") return false;
    if (code == "invalid_value" || code == "invalid_api_key" ||
        code == "insufficient_quota" || code == "context_length_exceeded") return false;
    if (parameter == "prompt_cache_key" &&
        (code == "unsupported_parameter" || code == "unknown_parameter" ||
         code == "unrecognized_parameter")) return true;
    if (unsupported_cache_key_message(json_string_or_empty(detail, "message"))) return true;
    if (error_it != error.end() && error_it->is_string() &&
        unsupported_cache_key_message(error_it->get<std::string>())) return true;
    const auto validation = error.find("detail");
    if (validation != error.end() && validation->is_array()) {
        for (const auto& item : *validation) {
            if (json_string_or_empty(item, "type") != "extra_forbidden") continue;
            const auto loc = item.find("loc");
            if (loc != item.end() && loc->is_array() &&
                ((loc->size() == 1 && (*loc)[0] == "prompt_cache_key") ||
                 (loc->size() == 2 && (*loc)[0] == "body" && (*loc)[1] == "prompt_cache_key"))) return true;
        }
    }
    return false;
}

} // namespace

void OpenAiCompatProvider::apply_call_options(nlohmann::json& body,
    const std::vector<ToolDef>& tools, bool for_compaction,
    const ChatRequestOptions* call_options) const {
    if (supports_prompt_cache_key()) {
        const auto scope = std::make_pair(request_url(), model_);
        std::lock_guard<std::mutex> lock(prompt_cache_mu_);
        const auto& key = call_options ? call_options->prompt_cache_key : prompt_cache_key_;
        if (!key.empty() && unsupported_prompt_cache_scopes_.count(scope) == 0) {
            body["prompt_cache_key"] = key;
        }
    }
    if (for_compaction && !tools.empty() && supports_compaction_tool_choice_none()) {
        body["tool_choice"] = "none";
    }
}

void OpenAiCompatProvider::set_prompt_cache_key(const std::string& key) {
    std::lock_guard<std::mutex> lock(prompt_cache_mu_);
    prompt_cache_key_ = key;
}

bool OpenAiCompatProvider::supports_prompt_cache_key() const {
    return name() == "openai" && known_cache_key_endpoint(request_url());
}

bool OpenAiCompatProvider::supports_compaction_tool_choice_none() const {
    return name() == "openai" && known_cache_key_endpoint(request_url());
}

bool OpenAiCompatProvider::remove_rejected_prompt_cache_key(
    nlohmann::json& body, const std::string& url,
    int status_code, const std::string& error_body) {
    if (!supports_prompt_cache_key() || !body.contains("prompt_cache_key") ||
        !explicit_cache_key_rejection(status_code, error_body)) return false;
    {
        std::lock_guard<std::mutex> lock(prompt_cache_mu_);
        unsupported_prompt_cache_scopes_.emplace(url, json_string_or_empty(body, "model"));
    }
    body.erase("prompt_cache_key");
    LOG_INFO("Retrying request without unsupported prompt_cache_key");
    return true;
}

ChatResponse OpenAiCompatProvider::chat_for_compaction(
    const std::vector<ChatMessage>& messages,
    const std::vector<ToolDef>& tools,
    const std::atomic<bool>* abort_flag) {
    if (!supports_compaction_prefix_reuse()) {
        return LlmProvider::chat_for_compaction(messages, tools, abort_flag);
    }
    return chat_cancellable_impl(messages, tools, abort_flag, true);
}

ChatResponse OpenAiCompatProvider::chat_cancellable(
    const std::vector<ChatMessage>& messages,
    const std::vector<ToolDef>& tools,
    const std::atomic<bool>* abort_flag) {
    return chat_cancellable_impl(messages, tools, abort_flag, false);
}

ChatResponse OpenAiCompatProvider::chat_with_options(
    const std::vector<ChatMessage>& messages,
    const std::vector<ToolDef>& tools,
    const ChatRequestOptions& options,
    const std::atomic<bool>* abort_flag) {
    if (name() != "openai") {
        return LlmProvider::chat_with_options(messages, tools, options, abort_flag);
    }
    return chat_cancellable_impl(messages, tools, abort_flag,
        options.for_compaction, &options);
}

void OpenAiCompatProvider::chat_stream(
    const std::vector<ChatMessage>& messages,
    const std::vector<ToolDef>& tools,
    const StreamCallback& callback,
    std::atomic<bool>* abort_flag
) {
    chat_stream_impl(messages, tools, callback, abort_flag, nullptr);
}

void OpenAiCompatProvider::chat_stream_with_options(
    const std::vector<ChatMessage>& messages,
    const std::vector<ToolDef>& tools,
    const ChatRequestOptions& options,
    const StreamCallback& callback,
    std::atomic<bool>* abort_flag) {
    if (name() != "openai") {
        LlmProvider::chat_stream_with_options(messages, tools, options, callback, abort_flag);
        return;
    }
    chat_stream_impl(messages, tools, callback, abort_flag, &options);
}

void OpenAiCompatProvider::chat_stream_impl(
    const std::vector<ChatMessage>& messages,
    const std::vector<ToolDef>& tools,
    const StreamCallback& callback,
    std::atomic<bool>* abort_flag,
    const ChatRequestOptions* call_options) {
    nlohmann::json body = build_request_body(messages, tools, true,
        call_options && call_options->for_compaction, call_options);
    std::string url = request_url();

    std::map<std::string, std::string> extra_headers;
    if (!api_key_.empty()) {
        extra_headers["Authorization"] = "Bearer " + api_key_;
    }
    std::string header_error;
    auto resolved_headers = resolve_request_headers(request_headers_, header_error);
    if (!resolved_headers.has_value()) {
        LOG_ERROR("OpenAI request_headers resolution failed: " + header_error);
        StreamEvent evt;
        evt.type = StreamEventType::Error;
        evt.error = header_error;
        callback(evt);
        return;
    }
    for (const auto& [k, v] : *resolved_headers) {
        extra_headers[k] = v;
    }

    parse_sse_stream(url, body, extra_headers, callback, abort_flag);
}

} // namespace acecode
