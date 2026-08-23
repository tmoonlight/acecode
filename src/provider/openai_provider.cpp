#include "openai_provider.hpp"
#include "dsml_tool_call_recovery.hpp"
#include "session/session_history_recovery.hpp"
#include "image/image_processor.hpp"
#include "config/request_headers.hpp"
#include "session/attachment_prompt_context.hpp"
#include "session/attachment_store.hpp"
#include "utils/logger.hpp"
#include "utils/base64.hpp"
#include "utils/sha1.hpp"
#include "network/proxy_resolver.hpp"
#include <cpr/cpr.h>
#include <cpr/ssl_options.h>
#include <stdexcept>
#include <sstream>
#include <optional>
#include <map>
#include <limits>
#include <unordered_set>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <utility>
#include <vector>

namespace acecode {

OpenAiCompatProvider::OpenAiCompatProvider(const std::string& base_url,
                                           const std::string& api_key,
                                           const std::string& model,
                                           int stream_timeout_ms,
                                           std::map<std::string, std::string> request_headers,
                                           ProviderRequestOptions request_options)
    : base_url_(normalize_endpoint(base_url, request_options.endpoint_mode)),
      api_key_(api_key),
      model_(model),
      request_headers_(std::move(request_headers)),
      request_options_(std::move(request_options)),
      stream_timeout_ms_(stream_timeout_ms > 0
          ? stream_timeout_ms
          : OpenAiConfig::kDefaultStreamTimeoutMs) {}

namespace {

constexpr int kStreamConnectTimeoutCapMs = 15000;

std::int64_t steady_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string ascii_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string provider_error_kind_to_string(ProviderErrorKind kind) {
    switch (kind) {
    case ProviderErrorKind::None:         return "none";
    case ProviderErrorKind::UserCancelled:return "user_cancelled";
    case ProviderErrorKind::Timeout:      return "timeout";
    case ProviderErrorKind::Network:      return "network";
    case ProviderErrorKind::Http:         return "http";
    case ProviderErrorKind::MalformedSse: return "malformed_sse";
    case ProviderErrorKind::MalformedJson:return "malformed_json";
    case ProviderErrorKind::Unknown:      return "unknown";
    }
    return "unknown";
}

std::string synthesize_tool_call_id(int index,
                                    const std::string& name,
                                    const std::string& arguments) {
    std::string fingerprint = std::to_string(index);
    fingerprint.push_back('\n');
    fingerprint.append(name);
    fingerprint.push_back('\n');
    fingerprint.append(arguments);
    return "call_ace_" + sha1_hex(fingerprint).substr(0, 24);
}

std::string json_string_or_empty(const nlohmann::json& value, const char* key) {
    if (!value.is_object() || !value.contains(key) || !value[key].is_string()) {
        return {};
    }
    return value[key].get<std::string>();
}

std::vector<ToolDef> request_tool_defs(const nlohmann::json& body) {
    std::vector<ToolDef> tools;
    if (!body.contains("tools") || !body["tools"].is_array()) return tools;

    for (const auto& item : body["tools"]) {
        if (!item.is_object() || !item.contains("function") ||
            !item["function"].is_object()) {
            continue;
        }
        const auto& function = item["function"];
        const std::string name = json_string_or_empty(function, "name");
        if (name.empty()) continue;

        ToolDef tool;
        tool.name = name;
        tools.push_back(std::move(tool));
    }
    return tools;
}

std::optional<std::size_t> find_complete_json_prefix_end(const std::string& value) {
    std::size_t i = 0;
    while (i < value.size() &&
           std::isspace(static_cast<unsigned char>(value[i]))) {
        ++i;
    }
    if (i >= value.size()) return std::nullopt;

    const char first = value[i];
    if (first != '{' && first != '[') return std::nullopt;

    std::vector<char> stack;
    bool in_string = false;
    bool escaped = false;
    for (; i < value.size(); ++i) {
        const char c = value[i];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }

        if (c == '"') {
            in_string = true;
            continue;
        }
        if (c == '{') {
            stack.push_back('}');
            continue;
        }
        if (c == '[') {
            stack.push_back(']');
            continue;
        }
        if (c == '}' || c == ']') {
            if (stack.empty() || stack.back() != c) return std::nullopt;
            stack.pop_back();
            if (stack.empty()) return i + 1;
        }
    }
    return std::nullopt;
}

std::string normalize_tool_call_arguments_for_request(
    const std::string& arguments,
    const std::string& tool_name,
    const std::string& tool_call_id,
    int& repaired_count) {
    auto parsed = nlohmann::json::parse(arguments, nullptr, false);
    if (!parsed.is_discarded()) return arguments;

    if (auto prefix_end = find_complete_json_prefix_end(arguments)) {
        std::string suffix = arguments.substr(*prefix_end);
        const bool trailing_garbage = std::any_of(
            suffix.begin(), suffix.end(), [](unsigned char c) {
                return !std::isspace(c);
            });
        if (trailing_garbage) {
            auto prefix = nlohmann::json::parse(
                arguments.substr(0, *prefix_end), nullptr, false);
            if (!prefix.is_discarded()) {
                ++repaired_count;
                LOG_WARN("build_request_body: trimmed invalid trailing bytes from "
                         "tool_call arguments id='" + tool_call_id + "' tool='" +
                         tool_name + "' raw=" + log_truncate(arguments, 200));
                return prefix.dump();
            }
        }
    }

    ++repaired_count;
    LOG_WARN("build_request_body: replaced invalid tool_call arguments with {} "
             "id='" + tool_call_id + "' tool='" + tool_name +
             "' raw=" + log_truncate(arguments, 200));
    return "{}";
}

nlohmann::json normalize_tool_calls_for_request(const nlohmann::json& tool_calls,
                                                int& repaired_count,
                                                int& dropped_malformed_count) {
    nlohmann::json tool_call_items;
    if (tool_calls.is_array()) {
        tool_call_items = tool_calls;
    } else if (tool_calls.is_object()) {
        tool_call_items = nlohmann::json::array({tool_calls});
    } else {
        return nlohmann::json::array();
    }

    nlohmann::json out = nlohmann::json::array();
    std::unordered_set<std::string> emitted_ids;
    for (const auto& raw_tc : tool_call_items) {
        if (!raw_tc.is_object()) {
            ++dropped_malformed_count;
            LOG_WARN("build_request_body: dropped malformed tool_call (not object)");
            continue;
        }

        std::string tool_call_id;
        if (raw_tc.contains("id") && raw_tc["id"].is_string()) {
            tool_call_id = raw_tc["id"].get<std::string>();
        }
        if (tool_call_id.empty()) {
            ++dropped_malformed_count;
            LOG_WARN("build_request_body: dropped malformed tool_call with empty id");
            continue;
        }
        if (emitted_ids.count(tool_call_id)) {
            ++dropped_malformed_count;
            LOG_WARN("build_request_body: dropped duplicate tool_call id='" +
                     tool_call_id + "'");
            continue;
        }

        if (!raw_tc.contains("function") || !raw_tc["function"].is_object()) {
            ++dropped_malformed_count;
            LOG_WARN("build_request_body: dropped malformed tool_call without function "
                     "id='" + tool_call_id + "'");
            continue;
        }

        nlohmann::json tc = raw_tc;
        if (!tc.contains("type") || !tc["type"].is_string() ||
            tc["type"].get<std::string>().empty()) {
            tc["type"] = "function";
            ++repaired_count;
            LOG_WARN("build_request_body: inserted missing tool_call type=function "
                     "id='" + tool_call_id + "'");
        }

        auto& fn = tc["function"];
        std::string tool_name;
        if (fn.contains("name") && fn["name"].is_string()) {
            tool_name = fn["name"].get<std::string>();
        }
        if (tool_name.empty()) {
            ++dropped_malformed_count;
            LOG_WARN("build_request_body: dropped malformed tool_call without function.name "
                     "id='" + tool_call_id + "'");
            continue;
        }

        if (fn.contains("arguments") && fn["arguments"].is_string()) {
            fn["arguments"] = normalize_tool_call_arguments_for_request(
                fn["arguments"].get<std::string>(),
                tool_name,
                tool_call_id,
                repaired_count);
        } else {
            ++repaired_count;
            LOG_WARN("build_request_body: inserted missing tool_call arguments {} "
                     "id='" + tool_call_id + "' tool='" + tool_name + "'");
            fn["arguments"] = "{}";
        }
        out.push_back(std::move(tc));
        emitted_ids.insert(tool_call_id);
    }
    return out;
}

std::string system_message_content_for_request(const nlohmann::json& message) {
    if (!message.is_object() || !message.contains("content")) return {};

    const auto& content = message["content"];
    if (content.is_null()) return {};
    if (content.is_string()) return content.get<std::string>();
    if (!content.is_array()) return content.dump();

    std::ostringstream oss;
    bool first = true;
    for (const auto& part : content) {
        std::string text;
        if (part.is_object() && part.value("type", std::string{}) == "text" &&
            part.contains("text") && part["text"].is_string()) {
            text = part["text"].get<std::string>();
        } else if (part.is_string()) {
            text = part.get<std::string>();
        } else if (!part.is_null()) {
            text = part.dump();
        }
        if (text.empty()) continue;
        if (!first) oss << "\n\n";
        first = false;
        oss << text;
    }
    return oss.str();
}

nlohmann::json coalesce_system_messages_at_front(const nlohmann::json& messages,
                                                 int& moved_count,
                                                 int& merged_count) {
    std::vector<nlohmann::json> system_messages;
    std::vector<nlohmann::json> non_system_messages;
    bool seen_non_system = false;

    for (const auto& message : messages) {
        if (message.is_object() && message.value("role", std::string{}) == "system") {
            if (seen_non_system) {
                ++moved_count;
            }
            system_messages.push_back(message);
            continue;
        }

        seen_non_system = true;
        non_system_messages.push_back(message);
    }

    if (system_messages.empty() ||
        (system_messages.size() == 1 && moved_count == 0)) {
        return messages;
    }

    std::string combined_content;
    for (const auto& system_message : system_messages) {
        const std::string content = system_message_content_for_request(system_message);
        if (content.empty()) continue;
        if (!combined_content.empty()) combined_content += "\n\n";
        combined_content += content;
    }

    nlohmann::json normalized = nlohmann::json::array();
    normalized.push_back(nlohmann::json{
        {"role", "system"},
        {"content", combined_content},
    });
    for (auto& message : non_system_messages) {
        normalized.push_back(std::move(message));
    }
    merged_count += static_cast<int>(system_messages.size()) - 1;
    return normalized;
}

bool parse_json_body(const std::string& body,
                     bool& body_is_json,
                     std::string& pretty_json) {
    body_is_json = false;
    pretty_json.clear();
    if (body.empty()) return false;
    try {
        const auto parsed = nlohmann::json::parse(body);
        pretty_json = parsed.dump(2);
        body_is_json = true;
        return true;
    } catch (const nlohmann::json::parse_error&) {
        return false;
    }
}

std::string json_scalar_to_string(const nlohmann::json& value) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_number_integer()) return std::to_string(value.get<long long>());
    if (value.is_number_unsigned()) return std::to_string(value.get<unsigned long long>());
    if (value.is_boolean()) return value.get<bool>() ? "true" : "false";
    if (value.is_null()) return {};
    return value.dump();
}

std::string first_string_field(const nlohmann::json& object,
                               std::initializer_list<const char*> keys) {
    if (!object.is_object()) return {};
    for (const char* key : keys) {
        auto it = object.find(key);
        if (it != object.end() && it->is_string()) {
            std::string value = it->get<std::string>();
            if (!value.empty()) return value;
        }
    }
    return {};
}

int parse_http_status_int(long long value) {
    return value >= 100 && value <= 599 ? static_cast<int>(value) : 0;
}

int parse_http_status_text(const std::string& text) {
    if (text.empty()) return 0;
    std::string lower = ascii_lower(text);
    for (const std::string& marker : {
             std::string("error code"),
             std::string("status code"),
             std::string("http status"),
             std::string("http "),
         }) {
        size_t pos = lower.find(marker);
        if (pos == std::string::npos) continue;
        pos += marker.size();
        while (pos < lower.size() && !std::isdigit(static_cast<unsigned char>(lower[pos]))) {
            ++pos;
        }
        long long status = 0;
        int digits = 0;
        while (pos < lower.size() && std::isdigit(static_cast<unsigned char>(lower[pos])) && digits < 3) {
            status = status * 10 + (lower[pos] - '0');
            ++pos;
            ++digits;
        }
        if (digits == 3) {
            int parsed = parse_http_status_int(status);
            if (parsed > 0) return parsed;
        }
    }
    return 0;
}

int http_status_from_json_field(const nlohmann::json& object,
                                std::initializer_list<const char*> keys) {
    if (!object.is_object()) return 0;
    for (const char* key : keys) {
        auto it = object.find(key);
        if (it == object.end() || it->is_null()) continue;
        if (it->is_number_integer()) {
            int parsed = parse_http_status_int(it->get<long long>());
            if (parsed > 0) return parsed;
        }
        if (it->is_number_unsigned()) {
            const auto value = it->get<unsigned long long>();
            if (value <= 599) {
                int parsed = parse_http_status_int(static_cast<long long>(value));
                if (parsed > 0) return parsed;
            }
        }
        if (it->is_string()) {
            int parsed = parse_http_status_text(it->get<std::string>());
            if (parsed > 0) return parsed;
        }
    }
    return 0;
}

bool has_stream_error_payload(const nlohmann::json& j) {
    auto it = j.find("error");
    if (it == j.end() || it->is_null()) return false;
    if (it->is_boolean()) return it->get<bool>();
    if (it->is_string()) return !it->get<std::string>().empty();
    if (it->is_object() || it->is_array()) return !it->empty();
    return true;
}

std::string stream_error_message_from_payload(const nlohmann::json& j) {
    auto it = j.find("error");
    if (it == j.end()) return {};
    const auto& error = *it;
    if (error.is_string()) return error.get<std::string>();
    if (error.is_object()) {
        std::string message = first_string_field(error, {"message", "detail", "error_description"});
        std::string type = first_string_field(error, {"type", "code"});
        if (!message.empty() && !type.empty()) return type + ": " + message;
        if (!message.empty()) return message;
        if (!type.empty()) return type;
    }
    return json_scalar_to_string(error);
}

int stream_error_status_from_payload(const nlohmann::json& j,
                                     const std::string& message) {
    int status = http_status_from_json_field(
        j, {"status_code", "status", "http_status", "error_code"});
    if (status > 0) return status;
    auto it = j.find("error");
    if (it != j.end() && it->is_object()) {
        status = http_status_from_json_field(
            *it, {"status_code", "status", "http_status", "error_code", "code"});
        if (status > 0) return status;
    }
    return parse_http_status_text(message);
}

std::string header_value_ci(const cpr::Header& headers, const std::string& key) {
    const std::string wanted = ascii_lower(key);
    for (const auto& [header_key, value] : headers) {
        if (ascii_lower(header_key) == wanted) return value;
    }
    return {};
}

std::string extract_request_id(const cpr::Header& headers) {
    for (const std::string& key : {
             "x-request-id",
             "request-id",
             "x-github-request-id",
             "x-ms-request-id",
             "cf-ray",
         }) {
        std::string value = header_value_ci(headers, key);
        if (!value.empty()) return value;
    }
    return {};
}

bool is_retryable_http_status(int status_code, const std::string& body) {
    return provider_http_error_is_retryable(status_code, body);
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

std::string provider_error_prefix(ProviderErrorKind kind, int status_code) {
    switch (kind) {
    case ProviderErrorKind::UserCancelled:
        return "Request cancelled";
    case ProviderErrorKind::Timeout:
        return "Request timed out";
    case ProviderErrorKind::Network:
        return "Connection failed";
    case ProviderErrorKind::Http:
        return "HTTP " + std::to_string(status_code);
    case ProviderErrorKind::MalformedJson:
        return "Malformed JSON in streaming response";
    case ProviderErrorKind::MalformedSse:
        return "Malformed or incomplete streaming response";
    case ProviderErrorKind::Unknown:
        return "Provider request failed";
    case ProviderErrorKind::None:
        return {};
    }
    return "Provider request failed";
}

ProviderErrorInfo make_provider_error(ProviderErrorKind kind,
                                      int status_code,
                                      const std::string& provider,
                                      const std::string& model,
                                      const std::string& request_id,
                                      const std::string& raw_body,
                                      const std::string& transport_message,
                                      bool retryable) {
    ProviderErrorInfo info;
    info.kind = kind;
    info.status_code = status_code;
    info.provider = provider;
    info.model = model;
    info.request_id = request_id;
    info.raw_body = raw_body;
    info.retryable = retryable;
    parse_json_body(raw_body, info.body_is_json, info.pretty_json);

    if (kind == ProviderErrorKind::UserCancelled) {
        info.display_message = "Request cancelled";
        return info;
    }

    std::ostringstream display;
    display << provider_error_prefix(kind, status_code);
    if (!provider.empty()) display << " from " << provider;
    if (!model.empty()) display << " model " << model;
    if (!request_id.empty()) display << " request_id=" << request_id;
    if (!transport_message.empty()) display << ": " << transport_message;
    const std::string body_for_display = info.body_is_json ? info.pretty_json : raw_body;
    if (!body_for_display.empty()) {
        display << "\n" << body_for_display;
    }
    info.display_message = display.str();
    return info;
}

ChatResponse make_chat_error_response(ProviderErrorInfo info) {
    ChatResponse response;
    response.content = "[Error] " + info.display_message;
    response.finish_reason = "error";
    response.provider_error = std::move(info);
    return response;
}

void emit_provider_error(const StreamCallback& callback, const ProviderErrorInfo& info) {
    StreamEvent evt;
    evt.type = StreamEventType::Error;
    evt.error = info.display_message;
    evt.provider_error = info;
    callback(evt);
}

void emit_retry_event(const StreamCallback& callback, ProviderErrorInfo info) {
    StreamEvent evt;
    evt.type = StreamEventType::Retry;
    evt.provider_error = std::move(info);
    evt.error = evt.provider_error.display_message;
    callback(evt);
}

void emit_retry_resume_event(const StreamCallback& callback,
                             const ProviderErrorInfo& info) {
    StreamEvent evt;
    evt.type = StreamEventType::RetryResume;
    evt.provider_error = info;
    callback(evt);
}

int retry_after_delay_ms(const cpr::Header& headers,
                         std::uint64_t retry_number) {
    std::optional<std::int64_t> server_delay;
    const std::string retry_after = header_value_ci(headers, "retry-after");
    if (!retry_after.empty()) {
        server_delay = parse_retry_after_ms(retry_after);
    }
    return static_cast<int>(
        provider_retry_delay_ms(retry_number, server_delay));
}

void push_openai_text_part(nlohmann::json& parts, const std::string& text) {
    if (text.empty()) return;
    parts.push_back(nlohmann::json{{"type", "text"}, {"text", text}});
}

// 判定一条附件是否应当作为 provider 图片 part 发送。以 MIME 为权威依据:
// SVG(矢量 XML)被排除,误标成 image 但 MIME 非图片的附件返回 false,从而
// 在序列化层兜底降级为文件句柄(route-attachments-by-capability D3)。
bool record_is_vision_image(const AttachmentRecord& record) {
    const std::string mime =
        ascii_lower(attachment_mime_for_name(record.name, record.mime_type));
    if (mime == "image/svg+xml") return false;
    return mime.rfind("image/", 0) == 0;
}

// 非视觉模型收到图片时的聚合 fallback 文本(tasks 1.9)。多张图合并成一段简短
// 句柄列表,并按系统是否还有可用视觉模型给出不同引导。
std::string gated_image_fallback_text(const std::vector<AttachmentRecord>& images,
                                      bool any_vision_model_available) {
    std::ostringstream oss;
    oss << "[Image attachment(s) not sent: the active model cannot inspect images]";
    for (const auto& record : images) {
        oss << "\n- " << record.name << " (" << record.mime_type << ", "
            << record.size_bytes << " bytes";
        if (!record.id.empty()) oss << ", attachment_id=" << record.id;
        oss << ")";
    }
    if (any_vision_model_available) {
        oss << "\nUse the vision_analyze tool (pass attachment_id or image_path) to "
               "inspect the image(s) with a vision-capable model.";
    } else {
        oss << "\nNo saved model is tagged with the 'vision' capability, so the "
               "image(s) cannot be analyzed. Configure a saved model with the vision "
               "capability to enable image analysis.";
    }
    return oss.str();
}

} // namespace

nlohmann::json openai_content_for_message(const ChatMessage& msg,
                                          bool model_has_vision,
                                          bool any_vision_model_available) {
    if (msg.content_parts.is_null() || !msg.content_parts.is_array() ||
        msg.content_parts.empty()) {
        return msg.content;
    }

    nlohmann::json parts = nlohmann::json::array();
    bool saw_text_part = false;
    // 被能力 gate 剥掉的图片附件,循环结束后聚合成一段句柄文本(tasks 1.9)。
    std::vector<AttachmentRecord> gated_images;
    for (const auto& part : msg.content_parts) {
        if (!part.is_object()) continue;
        const std::string type = part.value("type", std::string{});
        if (type == "text") {
            const std::string text = part.value("text", std::string{});
            if (!text.empty()) {
                saw_text_part = true;
                push_openai_text_part(parts, text);
            }
            continue;
        }

        if (type == "image") {
            auto record = part.contains("attachment")
                ? attachment_from_json(part["attachment"])
                : std::optional<AttachmentRecord>{};
            if (!record.has_value()) {
                push_openai_text_part(parts, "[Attached image unavailable: invalid metadata]");
                continue;
            }
            // D3 兜底:误标成 image 的非图片(含 SVG)按文件句柄处理,绝不发图片 payload。
            if (!record_is_vision_image(*record)) {
                push_openai_text_part(
                    parts, file_attachment_reference_text(*record));
                continue;
            }
            // D2/D5 能力 gate:active 模型不能看图时,聚合成 fallback 句柄文本而非发图。
            if (!model_has_vision) {
                gated_images.push_back(*record);
                continue;
            }

            std::string error;
            auto bytes = read_attachment_bytes(*record, kMaxAttachmentBytes, &error);
            if (!bytes.has_value()) {
                push_openai_text_part(parts,
                    "[Attached image unavailable: " +
                    (error.empty() ? record->name : error) + "]");
                continue;
            }

            std::string provider_mime = record->mime_type;
            std::string provider_bytes = *bytes;
            auto normalized = image::normalize_image_bytes(provider_bytes, provider_mime);
            if (normalized.attempted) {
                LOG_INFO("[provider] image normalization"
                         " name=" + record->name +
                         " ok=" + std::string(normalized.ok ? "1" : "0") +
                         " changed=" + std::string(normalized.changed ? "1" : "0") +
                         " original_size=" + std::to_string(bytes->size()) +
                         " reason=" + normalized.reason +
                         " error=" + normalized.error);
                if (normalized.ok && normalized.changed) {
                    provider_bytes = std::move(normalized.bytes);
                    if (!normalized.mime_type.empty()) {
                        provider_mime = normalized.mime_type;
                    }
                } else if (!normalized.ok) {
                    push_openai_text_part(parts,
                        "[Attached image unavailable: image normalization failed: " +
                        (normalized.error.empty() ? normalized.reason : normalized.error) + "]");
                    continue;
                }
            }

            parts.push_back(nlohmann::json{
                {"type", "image_url"},
                {"image_url", {
                    {"url", "data:" + provider_mime + ";base64," +
                            base64_encode(provider_bytes)}
                }},
            });
            continue;
        }

        if (type == "file") {
            auto record = part.contains("attachment")
                ? attachment_from_json(part["attachment"])
                : std::optional<AttachmentRecord>{};
            push_openai_text_part(parts, record.has_value()
                ? file_attachment_reference_text(*record)
                : std::string{"[Attached file unavailable: invalid metadata]"});
            continue;
        }

        if (type == "browser_context") {
            const auto ctx = part.contains("context") ? part["context"] : nlohmann::json::object();
            push_openai_text_part(parts, "[Browser context]\n" + ctx.dump(2));
        }
    }

    if (!gated_images.empty()) {
        saw_text_part = true;
        push_openai_text_part(parts,
            gated_image_fallback_text(gated_images, any_vision_model_available));
    }

    if (!saw_text_part && !msg.content.empty()) {
        parts.insert(parts.begin(), nlohmann::json{{"type", "text"}, {"text", msg.content}});
    }

    return parts.empty() ? nlohmann::json(msg.content) : parts;
}

nlohmann::json OpenAiCompatProvider::build_request_body(
    const std::vector<ChatMessage>& messages,
    const std::vector<ToolDef>& tools,
    bool stream
) const {
    nlohmann::json body;
    body["model"] = model_;
    if (stream) {
        body["stream"] = true;
        body["stream_options"] = {{"include_usage", true}};
    }
    if (request_options_.max_output_tokens.has_value()) {
        body["max_tokens"] = *request_options_.max_output_tokens;
    }
    if (request_options_.reasoning_protocol == ReasoningWireProtocol::OpenRouter &&
        request_options_.reasoning.has_value() &&
        request_options_.reasoning->supported) {
        const auto& reasoning = *request_options_.reasoning;
        const bool enabled = reasoning.mandatory ||
            reasoning.enabled.value_or(reasoning.default_enabled);
        nlohmann::json wire_reasoning = nlohmann::json::object();
        if (!enabled) {
            wire_reasoning["effort"] = "none";
        } else if (reasoning.max_tokens.has_value()) {
            // OpenRouter documents effort and max_tokens as alternative controls.
            // An explicit token budget is more precise and therefore wins.
            wire_reasoning["max_tokens"] = *reasoning.max_tokens;
        } else {
            const auto effort = reasoning.effort.has_value()
                ? reasoning.effort
                : reasoning.default_effort;
            if (effort.has_value()) {
                wire_reasoning["effort"] = *effort;
            } else {
                wire_reasoning["enabled"] = true;
            }
        }
        body["reasoning"] = std::move(wire_reasoning);
    }

    // Build messages array.
    // 最后一道防御:OpenAI/Ark 严格要求 role ∈ {system,user,assistant,tool}。
    // 任何非法 role(例如历史遗留的 UI-only `tool_result`)在此直接丢弃并 warn,
    // 防止 resume/压缩/旧 session 等路径意外污染 messages_ 时把整个请求打挂。
    nlohmann::json msgs_json = nlohmann::json::array();
    int dropped_invalid_role = 0;
    int repaired_tool_arguments = 0;
    int dropped_malformed_tool_calls = 0;
    int dropped_empty_assistant_tool_messages = 0;
    const auto recovered_history = recover_provider_history(messages);
    for (const auto& msg : recovered_history.messages) {
        const bool valid_role = (msg.role == "system" || msg.role == "user" ||
                                 msg.role == "assistant" || msg.role == "tool");
        if (!valid_role) {
            ++dropped_invalid_role;
            LOG_WARN("build_request_body: dropping message with invalid role='" +
                     msg.role + "' content=" + log_truncate(msg.content, 200));
            continue;
        }

        nlohmann::json m;
        m["role"] = msg.role;

        if (msg.role == "assistant" && !msg.tool_calls.is_null() && !msg.tool_calls.empty()) {
            nlohmann::json normalized_tool_calls = normalize_tool_calls_for_request(
                msg.tool_calls, repaired_tool_arguments, dropped_malformed_tool_calls);
            if (!normalized_tool_calls.empty()) {
                // Assistant message with tool calls
                if (!msg.content.empty()) {
                    m["content"] = msg.content;
                } else {
                    m["content"] = nullptr;
                }
                m["tool_calls"] = std::move(normalized_tool_calls);
            } else if (msg.content.empty() && msg.reasoning_content.empty()) {
                ++dropped_empty_assistant_tool_messages;
                LOG_WARN("build_request_body: dropped assistant message whose tool_calls "
                         "were all malformed and whose content was empty");
                continue;
            } else {
                m["content"] = msg.content;
            }
        } else if (msg.role == "tool") {
            m["content"] = msg.content;
            m["tool_call_id"] = msg.tool_call_id;
        } else {
            m["content"] = openai_content_for_message(
                msg, model_has_vision_, any_vision_model_available_);
        }

        // Echo reasoning_content back on assistant messages only. DeepSeek
        // thinking-mode rejects the next request with HTTP 400 if the previous
        // turn produced reasoning_content and we don't include it here.
        // Other OpenAI-compatible servers ignore unknown fields.
        if (msg.role == "assistant" && !msg.reasoning_content.empty()) {
            m["reasoning_content"] = msg.reasoning_content;
        }

        msgs_json.push_back(m);
    }
    if (dropped_invalid_role > 0) {
        LOG_WARN("build_request_body: total " + std::to_string(dropped_invalid_role) +
                 " message(s) dropped due to invalid role");
    }
    if (repaired_tool_arguments > 0) {
        LOG_WARN("build_request_body: repaired " +
                 std::to_string(repaired_tool_arguments) +
                 " tool_call payload field(s)");
    }
    if (dropped_malformed_tool_calls > 0) {
        LOG_WARN("build_request_body: dropped " +
                 std::to_string(dropped_malformed_tool_calls) +
                 " malformed tool_call payload(s)");
    }
    if (dropped_empty_assistant_tool_messages > 0) {
        LOG_WARN("build_request_body: dropped " +
                 std::to_string(dropped_empty_assistant_tool_messages) +
                 " empty assistant message(s) after tool_call normalization");
    }
    int moved_system_messages = 0;
    int merged_system_messages = 0;
    msgs_json = coalesce_system_messages_at_front(
        msgs_json, moved_system_messages, merged_system_messages);
    if (moved_system_messages > 0 || merged_system_messages > 0) {
        LOG_WARN("build_request_body: normalized system message placement "
                 "moved=" + std::to_string(moved_system_messages) +
                 " merged=" + std::to_string(merged_system_messages));
    }

    // Orphan tool_call 防御:OpenAI / Chat Completions 严格要求
    // assistant.tool_calls[*].id 都必须有一条 role=tool, tool_call_id=<id>
    // 紧跟在后面;否则整个请求 400 "No tool output found for function call …"。
    // 历史 session 在以下场景会持久化出 orphan(被打断的工具调用):
    //   - daemon 中途被 kill / 崩溃,assistant 已写盘但 tool result 未来得及落
    //   - 工具执行抛异常路径里 ToolResult 没被 append
    //   - resume 选了一个旧 jsonl,正好截在 assistant 之后
    // 一旦有 orphan,该 session 的下一条 user 消息会让 LLM 端永久作废。我们在
    // 请求出口把 orphan 用占位 tool 消息补齐,让 session 自动复活;同时 WARN
    // 一条供排查。msgs_ 内存与 session jsonl 都不动 — 只在序列化层补洞。
    nlohmann::json patched = nlohmann::json::array();
    int synthesized_stubs = 0;
    int dropped_orphan_tool_messages = 0;
    int dropped_unexpected_tool_messages = 0;
    int dropped_duplicate_tool_messages = 0;
    {
        size_t n = msgs_json.size();
        for (size_t i = 0; i < n; ) {
            const auto& cur = msgs_json[i];
            const std::string role = cur.value("role", std::string{});
            if (role == "tool") {
                ++dropped_orphan_tool_messages;
                const std::string id = cur.value("tool_call_id", std::string{});
                LOG_WARN("build_request_body: dropped standalone tool message "
                         "tool_call_id='" + id + "'");
                ++i;
                continue;
            }

            patched.push_back(cur);
            if (role == "assistant" && cur.contains("tool_calls") &&
                cur["tool_calls"].is_array() && !cur["tool_calls"].empty()) {
                std::vector<std::string> needed_ids;
                std::unordered_set<std::string> needed_set;
                for (const auto& tc : cur["tool_calls"]) {
                    if (tc.contains("id") && tc["id"].is_string()) {
                        std::string id = tc["id"].get<std::string>();
                        if (!id.empty() && !needed_set.count(id)) {
                            needed_ids.push_back(id);
                            needed_set.insert(id);
                        }
                    }
                }
                std::unordered_set<std::string> seen_ids;
                size_t j = i + 1;
                while (j < n && msgs_json[j].value("role", std::string{}) == "tool") {
                    std::string tool_call_id;
                    if (msgs_json[j].contains("tool_call_id") &&
                        msgs_json[j]["tool_call_id"].is_string()) {
                        tool_call_id = msgs_json[j]["tool_call_id"].get<std::string>();
                    }
                    if (tool_call_id.empty() || !needed_set.count(tool_call_id)) {
                        ++dropped_unexpected_tool_messages;
                        LOG_WARN("build_request_body: dropped unexpected tool message "
                                 "tool_call_id='" + tool_call_id + "'");
                    } else if (seen_ids.count(tool_call_id)) {
                        ++dropped_duplicate_tool_messages;
                        LOG_WARN("build_request_body: dropped duplicate tool message "
                                 "tool_call_id='" + tool_call_id + "'");
                    } else {
                        patched.push_back(msgs_json[j]);
                        seen_ids.insert(tool_call_id);
                    }
                    ++j;
                }
                for (const auto& id : needed_ids) {
                    if (!seen_ids.count(id)) {
                        nlohmann::json stub;
                        stub["role"] = "tool";
                        stub["tool_call_id"] = id;
                        stub["content"] =
                            "[Error] Tool execution was interrupted before it could "
                            "produce a result. No output is available for this call.";
                        patched.push_back(std::move(stub));
                        ++synthesized_stubs;
                    }
                }
                i = j;
            } else {
                ++i;
            }
        }
    }
    if (synthesized_stubs > 0) {
        LOG_WARN("build_request_body: synthesized " + std::to_string(synthesized_stubs) +
                 " placeholder tool message(s) for orphan tool_call(s) "
                 "(likely from interrupted prior turns; session will continue)");
    }
    if (dropped_orphan_tool_messages > 0 ||
        dropped_unexpected_tool_messages > 0 ||
        dropped_duplicate_tool_messages > 0) {
        LOG_WARN("build_request_body: dropped invalid tool message(s) "
                 "standalone=" + std::to_string(dropped_orphan_tool_messages) +
                 " unexpected=" + std::to_string(dropped_unexpected_tool_messages) +
                 " duplicate=" + std::to_string(dropped_duplicate_tool_messages));
    }
    body["messages"] = patched;

    // Build tools array
    if (!tools.empty()) {
        nlohmann::json tools_json = nlohmann::json::array();
        for (const auto& tool : tools) {
            nlohmann::json t;
            t["type"] = "function";
            t["function"]["name"] = tool.name;
            t["function"]["description"] = tool.description;
            t["function"]["parameters"] = tool.parameters;
            tools_json.push_back(t);
        }
        body["tools"] = tools_json;
    }

    return body;
}

ChatResponse OpenAiCompatProvider::parse_response(const nlohmann::json& j) {
    ChatResponse resp;

    if (!j.contains("choices") || j["choices"].empty()) {
        resp.content = "[Error] No choices in API response.";
        resp.finish_reason = "error";
        return resp;
    }

    const auto& choice = j["choices"][0];
    resp.finish_reason = choice.value("finish_reason", "stop");

    const auto& message = choice["message"];

    if (message.contains("content") && !message["content"].is_null()) {
        resp.content = message["content"].get<std::string>();
    }

    // Reasoning-mode chain-of-thought. DeepSeek primary name is
    // `reasoning_content`; OpenRouter/Qwen alias is `reasoning`.
    if (message.contains("reasoning_content") && message["reasoning_content"].is_string()) {
        resp.reasoning_content = message["reasoning_content"].get<std::string>();
    } else if (message.contains("reasoning") && message["reasoning"].is_string()) {
        resp.reasoning_content = message["reasoning"].get<std::string>();
    }

    if (message.contains("tool_calls") && message["tool_calls"].is_array()) {
        int index = 0;
        for (const auto& tc : message["tool_calls"]) {
            ToolCall call;
            call.id = json_string_or_empty(tc, "id");
            if (tc.contains("function") && tc["function"].is_object()) {
                call.function_name = json_string_or_empty(tc["function"], "name");
                call.function_arguments = json_string_or_empty(tc["function"], "arguments");
            }
            if (call.id.empty()) {
                call.id = synthesize_tool_call_id(index, call.function_name, call.function_arguments);
                LOG_WARN("parse_response: synthesized missing tool_call id index=" +
                         std::to_string(index) + " tool='" + call.function_name + "'");
            }
            resp.tool_calls.push_back(call);
            ++index;
        }
    }

    return resp;
}

ChatResponse OpenAiCompatProvider::chat(
    const std::vector<ChatMessage>& messages,
    const std::vector<ToolDef>& tools
) {
    nlohmann::json body = build_request_body(messages, tools, false);

    std::string url = request_url();

    cpr::Header headers = {
        {"Content-Type", "application/json"}
    };
    if (!api_key_.empty()) {
        headers["Authorization"] = "Bearer " + api_key_;
    }
    std::string header_error;
    auto resolved_headers = resolve_request_headers(request_headers_, header_error);
    if (!resolved_headers.has_value()) {
        return make_chat_error_response(make_provider_error(
            ProviderErrorKind::Unknown,
            0,
            name(),
            model_,
            std::string{},
            std::string{},
            header_error,
            false));
    }
    for (const auto& [k, v] : *resolved_headers) {
        headers[k] = v;
    }

    auto proxy_opts = network::proxy_options_for(url);
    cpr::Response r = cpr::Post(
        cpr::Url{url},
        headers,
        cpr::Body{body.dump()},
        network::build_ssl_options(proxy_opts),
        proxy_opts.proxies,
        proxy_opts.auth,
        cpr::Timeout{stream_timeout_ms_}
    );

    if (r.status_code == 0) {
        const ProviderErrorKind kind = classify_cpr_error(r.error);
        return make_chat_error_response(make_provider_error(
            kind,
            0,
            name(),
            model_,
            extract_request_id(r.header),
            r.text,
            r.error.message,
            kind == ProviderErrorKind::Timeout ||
                kind == ProviderErrorKind::Network));
    }

    if (r.status_code != 200) {
        auto info = make_provider_error(
            ProviderErrorKind::Http,
            static_cast<int>(r.status_code),
            name(),
            model_,
            extract_request_id(r.header),
            r.text,
            std::string{},
            is_retryable_http_status(static_cast<int>(r.status_code), r.text));
        const std::string retry_after =
            header_value_ci(r.header, "retry-after");
        if (!retry_after.empty()) {
            if (auto parsed = parse_retry_after_ms(retry_after)) {
                info.server_retry_after_ms = *parsed;
            }
        }
        return make_chat_error_response(std::move(info));
    }

    try {
        nlohmann::json response_json = nlohmann::json::parse(r.text);
        auto resp = parse_response(response_json);
        auto dsml = recover_dsml_tool_calls(resp.content, request_tool_defs(body));
        resp.content = std::move(dsml.visible_text);
        if (!dsml.error.empty()) {
            LOG_WARN("Rejected DSML tool-call candidate: " + dsml.error);
        }
        if (dsml.recovered) {
            if (resp.tool_calls.empty()) {
                resp.tool_calls = std::move(dsml.tool_calls);
                resp.finish_reason = "tool_calls";
                LOG_INFO("Recovered non-streaming DSML tool calls count=" +
                         std::to_string(resp.tool_calls.size()));
            } else {
                LOG_WARN("Ignored recovered DSML calls because native tool_calls exist");
            }
        }
        // Parse usage from non-streaming response
        if (response_json.contains("usage") && response_json["usage"].is_object()) {
            const auto& u = response_json["usage"];
            resp.usage.prompt_tokens = u.value("prompt_tokens", 0);
            resp.usage.completion_tokens = u.value("completion_tokens", 0);
            resp.usage.total_tokens = u.value("total_tokens", 0);
            if (u.contains("prompt_tokens_details") && u["prompt_tokens_details"].is_object()) {
                const auto& d = u["prompt_tokens_details"];
                resp.usage.cache_read_tokens = d.value("cached_tokens", 0);
                resp.usage.cache_write_tokens = d.value("cache_write_tokens", 0);
            }
            if (u.contains("completion_tokens_details") && u["completion_tokens_details"].is_object()) {
                const auto& d = u["completion_tokens_details"];
                resp.usage.reasoning_tokens = d.value("reasoning_tokens", 0);
            }
            resp.usage.has_data = true;
        }
        return resp;
    } catch (const nlohmann::json::parse_error& e) {
        return make_chat_error_response(make_provider_error(
            ProviderErrorKind::MalformedJson,
            200,
            name(),
            model_,
            extract_request_id(r.header),
            r.text,
            e.what(),
            false));
    }
}

// ---- SSE stream parsing ----

ChatResponse OpenAiCompatProvider::parse_sse_stream(
    const std::string& url,
    const nlohmann::json& body,
    const std::map<std::string, std::string>& extra_headers,
    const StreamCallback& callback,
    std::atomic<bool>* abort_flag
) {
    LOG_INFO("parse_sse_stream url=" + url);

    // Build cpr headers
    cpr::Header headers = {{"Content-Type", "application/json"}};
    for (const auto& [k, v] : extra_headers) {
        headers[k] = v;
    }

    std::atomic<std::int64_t> last_stream_activity_ms{steady_now_ms()};
    std::atomic<bool> stream_idle_timed_out{false};
    const int stream_idle_timeout_ms = (std::max)(1, stream_timeout_ms_);

    // libcurl invokes this ~1 Hz regardless of whether bytes are flowing, so it
    // is the only path that can cancel during a model's silent reasoning phase.
    // Do not use cpr::Timeout for streaming: that is a total request duration
    // cap and will kill healthy SSE streams that send keepalive comments such
    // as ": PROCESSING", causing a replay while the server may still be
    // working on the original request.
    auto progress_cb = cpr::ProgressCallback{
        [abort_flag, &last_stream_activity_ms, &stream_idle_timed_out,
         stream_idle_timeout_ms](cpr::cpr_off_t /*dl_total*/,
                                 cpr::cpr_off_t /*dl_now*/,
                                 cpr::cpr_off_t /*ul_total*/,
                                 cpr::cpr_off_t /*ul_now*/,
                                 intptr_t /*userdata*/) -> bool {
            if (abort_flag && abort_flag->load()) {
                return false;
            }
            const std::int64_t idle_ms =
                steady_now_ms() - last_stream_activity_ms.load();
            if (idle_ms >= stream_idle_timeout_ms) {
                stream_idle_timed_out.store(true);
                return false;
            }
            return true;
        }
    };

    struct ToolCallAccumulator {
        std::string id;
        std::string name;
        std::string arguments;
    };

    ChatResponse last_accumulated;
    last_accumulated.finish_reason = "stop";
    const auto dsml_tools = request_tool_defs(body);

    for (std::uint64_t attempt = 1; ;
         attempt = attempt == (std::numeric_limits<std::uint64_t>::max)()
             ? attempt
             : attempt + 1) {
        last_stream_activity_ms.store(steady_now_ms());
        stream_idle_timed_out.store(false);

        ChatResponse accumulated;
        accumulated.finish_reason = "stop";
        // 服务端真实上报的 finish_reason。与 accumulated.finish_reason 分开存:
        // 后者有 "stop" 兜底默认值,区分不了「真 stop」与「网关从未上报」。
        // Done 事件只透传这个原始值,空 = 上游没发(部分兼容网关会省略)。
        std::string reported_finish_reason;
        std::string sse_buffer;
        std::string raw_body_capture;
        std::map<int, ToolCallAccumulator> pending_tools;
        bool saw_done = false;
        bool saw_sse_data = false;
        bool saw_parse_error = false;
        bool saw_payload_error = false;
        std::string payload_error_body;
        std::string payload_error_message;
        int payload_error_status_code = 0;
        DsmlToolCallStreamFilter dsml_filter(dsml_tools);

        auto emit_visible_text = [&](const std::string& text) {
            if (text.empty()) return;
            accumulated.content += text;

            StreamEvent evt;
            evt.type = StreamEventType::Delta;
            evt.content = text;
            callback(evt);
        };

        auto flush_pending_tools = [&]() {
            for (auto& [idx, tc] : pending_tools) {
                ToolCall call;
                call.id = tc.id.empty()
                    ? synthesize_tool_call_id(idx, tc.name, tc.arguments)
                    : tc.id;
                call.function_name = tc.name;
                call.function_arguments = tc.arguments;
                if (tc.id.empty()) {
                    LOG_WARN("chat_stream: synthesized missing tool_call id index=" +
                             std::to_string(idx) + " tool='" + tc.name + "'");
                }
                accumulated.tool_calls.push_back(call);

                StreamEvent evt;
                evt.type = StreamEventType::ToolCall;
                evt.tool_call = call;
                evt.tool_index = idx;
                callback(evt);
            }
            pending_tools.clear();
        };

        auto emit_done = [&]() {
            if (saw_done) return;

            auto dsml = dsml_filter.finish();
            emit_visible_text(dsml.visible_text);
            if (!dsml.error.empty()) {
                LOG_WARN("Rejected DSML tool-call candidate: " + dsml.error);
            }
            if (dsml.recovered) {
                const bool has_native_tool_calls =
                    !pending_tools.empty() || !accumulated.tool_calls.empty();
                if (has_native_tool_calls) {
                    LOG_WARN("Ignored recovered DSML calls because native tool_calls exist");
                } else {
                    accumulated.finish_reason = "tool_calls";
                    reported_finish_reason = "tool_calls";
                    LOG_INFO("Recovered streaming DSML tool calls count=" +
                             std::to_string(dsml.tool_calls.size()));
                    for (std::size_t index = 0; index < dsml.tool_calls.size(); ++index) {
                        const auto& call = dsml.tool_calls[index];
                        accumulated.tool_calls.push_back(call);

                        StreamEvent evt;
                        evt.type = StreamEventType::ToolCall;
                        evt.tool_call = call;
                        evt.tool_index = static_cast<int>(index);
                        callback(evt);
                    }
                }
            }

            flush_pending_tools();
            if (accumulated.usage.has_data) {
                StreamEvent usage_evt;
                usage_evt.type = StreamEventType::Usage;
                usage_evt.usage = accumulated.usage;
                callback(usage_evt);
            }

            StreamEvent done_evt;
            done_evt.type = StreamEventType::Done;
            done_evt.finish_reason = reported_finish_reason;
            callback(done_evt);
            saw_done = true;
        };

        auto find_event_delimiter = [](const std::string& buffer,
                                       size_t& pos,
                                       size_t& delimiter_len) -> bool {
            const size_t lf = buffer.find("\n\n");
            const size_t crlf = buffer.find("\r\n\r\n");
            if (lf == std::string::npos && crlf == std::string::npos) return false;
            if (crlf != std::string::npos &&
                (lf == std::string::npos || crlf < lf)) {
                pos = crlf;
                delimiter_len = 4;
            } else {
                pos = lf;
                delimiter_len = 2;
            }
            return true;
        };

        auto write_cb = cpr::WriteCallback{[&](const std::string_view data, intptr_t) -> bool {
            if (abort_flag && abort_flag->load()) {
                return false; // cancel request
            }

            if (!data.empty()) {
                last_stream_activity_ms.store(steady_now_ms());
            }
            raw_body_capture.append(data.data(), data.size());
            sse_buffer += std::string(data);

            size_t pos = 0;
            size_t delimiter_len = 0;
            while (find_event_delimiter(sse_buffer, pos, delimiter_len)) {
                std::string event_block = sse_buffer.substr(0, pos);
                sse_buffer.erase(0, pos + delimiter_len);

                std::istringstream iss(event_block);
                std::string line;
                std::string event_data;
                while (std::getline(iss, line)) {
                    if (!line.empty() && line.back() == '\r') {
                        line.pop_back();
                    }
                    static constexpr std::string_view kDataPrefix = "data:";
                    if (line.compare(0, kDataPrefix.size(), kDataPrefix) == 0) {
                        size_t value_start = kDataPrefix.size();
                        if (value_start < line.size() && line[value_start] == ' ') {
                            ++value_start;
                        }
                        if (!event_data.empty()) event_data += "\n";
                        event_data += line.substr(value_start);
                    }
                }

                if (event_data.empty()) continue;
                saw_sse_data = true;
                if (event_data == "[DONE]") {
                    LOG_DEBUG("SSE [DONE] received. pending_tools=" + std::to_string(pending_tools.size()));
                    emit_done();
                    continue;
                }

                try {
                    auto j = nlohmann::json::parse(event_data);

                    if (has_stream_error_payload(j)) {
                        saw_payload_error = true;
                        payload_error_body = event_data;
                        payload_error_message = stream_error_message_from_payload(j);
                        payload_error_status_code =
                            stream_error_status_from_payload(j, payload_error_message);
                        LOG_ERROR("SSE payload error: " +
                                  log_truncate(payload_error_message.empty()
                                      ? event_data
                                      : payload_error_message, 500));
                        return false;
                    }

                    if (j.contains("usage") && j["usage"].is_object()) {
                        const auto& u = j["usage"];
                        accumulated.usage.prompt_tokens = u.value("prompt_tokens", 0);
                        accumulated.usage.completion_tokens = u.value("completion_tokens", 0);
                        accumulated.usage.total_tokens = u.value("total_tokens", 0);
                        if (u.contains("prompt_tokens_details") && u["prompt_tokens_details"].is_object()) {
                            const auto& d = u["prompt_tokens_details"];
                            accumulated.usage.cache_read_tokens = d.value("cached_tokens", 0);
                            accumulated.usage.cache_write_tokens = d.value("cache_write_tokens", 0);
                        }
                        if (u.contains("completion_tokens_details") && u["completion_tokens_details"].is_object()) {
                            const auto& d = u["completion_tokens_details"];
                            accumulated.usage.reasoning_tokens = d.value("reasoning_tokens", 0);
                        }
                        accumulated.usage.has_data = true;
                    }

                    if (!j.contains("choices") || j["choices"].empty()) continue;

                    const auto& choice = j["choices"][0];
                    if (choice.contains("finish_reason") && !choice["finish_reason"].is_null()) {
                        accumulated.finish_reason = choice["finish_reason"].get<std::string>();
                        reported_finish_reason = accumulated.finish_reason;
                    }

                    if (!choice.contains("delta")) continue;
                    const auto& delta = choice["delta"];

                    std::string reasoning_token;
                    if (delta.contains("reasoning_content") && delta["reasoning_content"].is_string()) {
                        reasoning_token = delta["reasoning_content"].get<std::string>();
                    } else if (delta.contains("reasoning") && delta["reasoning"].is_string()) {
                        reasoning_token = delta["reasoning"].get<std::string>();
                    }
                    if (!reasoning_token.empty()) {
                        accumulated.reasoning_content += reasoning_token;

                        StreamEvent evt;
                        evt.type = StreamEventType::ReasoningDelta;
                        evt.content = reasoning_token;
                        callback(evt);
                    }

                    if (delta.contains("content") && !delta["content"].is_null()) {
                        std::string token = delta["content"].get<std::string>();
                        emit_visible_text(dsml_filter.push(token));
                    }

                    if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
                        for (const auto& tc_delta : delta["tool_calls"]) {
                            int index = tc_delta.value("index", 0);
                            auto& acc = pending_tools[index];
                            const std::string before_id = acc.id;
                            const std::string before_name = acc.name;
                            const std::size_t before_args_size = acc.arguments.size();

                            // 只在 id / name 为非空字符串时更新。标准 OpenAI 在续传 delta 里
                            // 省略 id/name,但部分网关(DeepSeek / wizard-ai code_pilot)会在
                            // 续传帧里发 "id":"" / "name":""(空字符串,非 null)。旧逻辑只判
                            // !is_null(),会把首帧捕获到的真实 id/name 覆盖成空 → 工具名丢失
                            // → agent_loop 报 "Unknown tool" 并空转。空续传帧不应覆盖已累积值。
                            std::string incoming_id;
                            if (tc_delta.contains("id") && tc_delta["id"].is_string()) {
                                incoming_id = tc_delta["id"].get<std::string>();
                            }
                            const bool has_id = !incoming_id.empty();
                            const bool is_full_snapshot_resend =
                                has_id && !acc.id.empty() && acc.id == incoming_id;

                            if (has_id) {
                                acc.id = incoming_id;
                            }
                            if (tc_delta.contains("function")) {
                                const auto& fn = tc_delta["function"];
                                if (fn.contains("name") && fn["name"].is_string()) {
                                    std::string incoming_name = fn["name"].get<std::string>();
                                    if (!incoming_name.empty()) acc.name = incoming_name;
                                }
                                if (fn.contains("arguments") && !fn["arguments"].is_null()) {
                                    std::string incoming_args = fn["arguments"].get<std::string>();
                                    if (incoming_args.empty()) {
                                        // Keep already accumulated args; some gateways send empty confirmation frames.
                                    } else if (is_full_snapshot_resend &&
                                               incoming_args.size() >= acc.arguments.size() &&
                                               incoming_args.compare(0, acc.arguments.size(),
                                                                     acc.arguments) == 0) {
                                        acc.arguments = incoming_args;
                                    } else {
                                        acc.arguments += incoming_args;
                                    }
                                }
                            }
                            if (acc.id != before_id || acc.name != before_name ||
                                acc.arguments.size() != before_args_size) {
                                StreamEvent progress_evt;
                                progress_evt.type = StreamEventType::ToolCallDelta;
                                progress_evt.tool_index = index;
                                progress_evt.tool_call.id = acc.id;
                                progress_evt.tool_call.function_name = acc.name;
                                progress_evt.tool_call_argument_bytes = acc.arguments.size();
                                callback(progress_evt);
                            }
                        }
                    }

                    if (choice.contains("finish_reason") && !choice["finish_reason"].is_null()) {
                        flush_pending_tools();
                    }
                } catch (const nlohmann::json::parse_error& e) {
                    saw_parse_error = true;
                    LOG_WARN("SSE JSON parse error: " + std::string(e.what()) +
                             " data=" + log_truncate(event_data, 200));
                }
            }
            return true;
        }};

        // Re-resolve routing on every attempt so moving between a VPN,
        // system proxy, and direct networking takes effect after the wait.
        auto proxy_opts = network::proxy_options_for(url);
        cpr::Response r = cpr::Post(
            cpr::Url{url},
            headers,
            cpr::Body{body.dump()},
            cpr::ConnectTimeout{
                (std::min)(stream_idle_timeout_ms, kStreamConnectTimeoutCapMs)},
            network::build_ssl_options(proxy_opts),
            proxy_opts.proxies,
            proxy_opts.auth,
            write_cb,
            progress_cb
        );

        last_accumulated = accumulated;
        const bool user_aborted = abort_flag && abort_flag->load();
        if (user_aborted) {
            if (accumulated.content.empty() && pending_tools.empty()) {
                LOG_WARN("SSE request aborted by user (no-data phase or progress callback)");
            } else {
                LOG_WARN("SSE request aborted by user");
            }
            auto info = make_provider_error(
                ProviderErrorKind::UserCancelled,
                0,
                name(),
                model_,
                std::string{},
                std::string{},
                std::string{},
                false);
            emit_provider_error(callback, info);
            return accumulated;
        }

        ProviderErrorInfo error_info;
        const std::string request_id = extract_request_id(r.header);
        const bool idle_timeout = stream_idle_timed_out.load();
        const bool transport_failed = static_cast<bool>(r.error);
        const ProviderErrorKind transport_kind =
            idle_timeout ? ProviderErrorKind::Timeout : classify_cpr_error(r.error);
        if (saw_payload_error) {
            const ProviderErrorKind kind = payload_error_status_code > 0
                ? ProviderErrorKind::Http
                : ProviderErrorKind::Unknown;
            error_info = make_provider_error(
                kind,
                payload_error_status_code,
                name(),
                model_,
                request_id,
                payload_error_body,
                payload_error_message.empty()
                    ? std::string("stream payload contained an error")
                    : payload_error_message,
                is_retryable_http_status(
                    payload_error_status_code, payload_error_body));
        } else if (idle_timeout ||
                   (transport_failed && transport_kind == ProviderErrorKind::Timeout)) {
            const int status_code = r.status_code == 0 ? 0 : static_cast<int>(r.status_code);
            const std::string timeout_message = idle_timeout
                ? "stream idle timeout after " +
                    std::to_string(stream_idle_timeout_ms) +
                    " ms without data"
                : (r.error.message.empty()
                    ? std::string("request timed out")
                    : r.error.message);
            LOG_ERROR("SSE connection timed out: " + timeout_message +
                      " body=" + log_truncate(raw_body_capture, 2000));
            error_info = make_provider_error(
                ProviderErrorKind::Timeout,
                status_code,
                name(),
                model_,
                request_id,
                raw_body_capture,
                timeout_message,
                true);
        } else if (r.status_code != 0 && r.status_code != 200) {
            const std::string err_body = r.text.empty() ? raw_body_capture : r.text;
            LOG_ERROR("SSE HTTP error: " + std::to_string(r.status_code) +
                      " body=" + log_truncate(err_body, 2000));
            error_info = make_provider_error(
                ProviderErrorKind::Http,
                static_cast<int>(r.status_code),
                name(),
                model_,
                request_id,
                err_body,
                std::string{},
                is_retryable_http_status(static_cast<int>(r.status_code), err_body));
        } else if (r.status_code == 0) {
            LOG_ERROR("SSE connection failed: " + r.error.message);
            error_info = make_provider_error(
                transport_kind,
                0,
                name(),
                model_,
                request_id,
                raw_body_capture,
                r.error.message,
                true);
        } else if (!saw_done) {
            const ProviderErrorKind kind = saw_parse_error
                ? ProviderErrorKind::MalformedJson
                : ProviderErrorKind::MalformedSse;
            const std::string transport_message = saw_sse_data
                ? "stream ended before [DONE]"
                : "response did not contain SSE data";
            LOG_ERROR("SSE malformed response: " + transport_message +
                      " body=" + log_truncate(raw_body_capture, 2000));
            // SSE 中途断开(HTTP 200 但通道在 [DONE] 前断)是不稳定私有模型的典型
            // 形态。工具只会在完整 provider 响应返回后执行,所以 provisional
            // text/reasoning/tool-call state 都可安全丢弃并重发当前请求。
            // MalformedJson(SSE 帧本身解析失败)不重试 — 再发一次大概率仍是 garbage。
            const bool malformed_sse_retryable =
                kind == ProviderErrorKind::MalformedSse;
            error_info = make_provider_error(
                kind,
                200,
                name(),
                model_,
                request_id,
                raw_body_capture,
                transport_message,
                malformed_sse_retryable);
        } else {
            return accumulated;
        }

        if (error_info.retryable) {
            const int delay_ms = retry_after_delay_ms(r.header, attempt);
            error_info.retry_attempt = saturating_retry_attempt(attempt);
            error_info.retry_max_attempts = -1;
            error_info.retry_delay_ms = delay_ms;
            LOG_WARN("Retrying streaming request after " +
                     provider_error_kind_to_string(error_info.kind) +
                     " failure; retry=" +
                     std::to_string(error_info.retry_attempt) +
                     " delay_ms=" + std::to_string(delay_ms) +
                     " max_attempts=unbounded");
            emit_retry_event(callback, error_info);
            if (wait_for_retry(
                    std::chrono::milliseconds(delay_ms), abort_flag)) {
                auto cancel_info = make_provider_error(
                    ProviderErrorKind::UserCancelled,
                    0,
                    name(),
                    model_,
                    std::string{},
                    std::string{},
                    std::string{},
                    false);
                emit_provider_error(callback, cancel_info);
                return accumulated;
            }
            emit_retry_resume_event(callback, error_info);
            continue;
        }

        error_info.retry_attempt =
            saturating_retry_attempt(attempt > 0 ? attempt - 1 : 0);
        error_info.retry_max_attempts = 0;
        emit_provider_error(callback, error_info);
        return accumulated;
    }

    return last_accumulated;
}

void OpenAiCompatProvider::chat_stream(
    const std::vector<ChatMessage>& messages,
    const std::vector<ToolDef>& tools,
    const StreamCallback& callback,
    std::atomic<bool>* abort_flag
) {
    nlohmann::json body = build_request_body(messages, tools, true);
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
