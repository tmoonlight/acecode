#include "openai_responses.hpp"

#include "retry_policy.hpp"

#include <algorithm>
#include <limits>
#include <set>

namespace acecode {
namespace {

using Json = nlohmann::json;

bool has_string(const Json& value, const char* key) {
    return value.is_object() && value.contains(key) && value[key].is_string();
}

std::string text_field(const Json& value, const char* key) {
    return has_string(value, key) ? value[key].get<std::string>() : std::string{};
}

bool has_array(const Json& value, const char* key) {
    return value.is_object() && value.contains(key) && value[key].is_array();
}

int nonnegative_int(const Json& value, const char* key, int fallback = 0) {
    if (!value.is_object() || !value.contains(key)) return fallback;
    const auto& number = value[key];
    if (number.is_number_unsigned()) {
        return static_cast<int>((std::min)(number.get<std::uint64_t>(),
            static_cast<std::uint64_t>((std::numeric_limits<int>::max)())));
    }
    if (!number.is_number_integer()) return fallback;
    const auto raw = number.get<std::int64_t>();
    if (raw < 0) return fallback;
    return static_cast<int>((std::min)(raw,
        static_cast<std::int64_t>((std::numeric_limits<int>::max)())));
}

bool valid_index(const Json& value, const char* key) {
    if (!value.is_object() || !value.contains(key)) return false;
    const auto& number = value[key];
    return number.is_number_integer() && number >= 0 &&
        number <= (std::numeric_limits<int>::max)();
}

bool prefix_of(const std::string& prefix, const std::string& text) {
    return text.size() >= prefix.size() &&
        text.compare(0, prefix.size(), prefix) == 0;
}

const Json& error_payload(const Json& event) {
    if (event.is_object()) {
        if (event.contains("error") && !event["error"].is_null()) {
            return event["error"];
        }
        if (event.contains("response") && event["response"].is_object()) {
            return error_payload(event["response"]);
        }
    }
    return event;
}

ProviderErrorInfo response_error(const Json& event, ProviderErrorKind kind,
                                 const std::string& fallback) {
    ProviderErrorInfo result;
    result.kind = kind;
    result.provider = "openai";
    const auto& payload = error_payload(event);
    result.display_message = payload.is_string()
        ? payload.get<std::string>() : text_field(payload, "message");
    if (result.display_message.empty()) result.display_message = fallback;
    result.body_is_json = event.is_object();
    result.raw_body = event.dump();
    if (result.body_is_json) result.pretty_json = event.dump(2);
    result.request_id = text_field(event, "request_id");
    if (result.request_id.empty()) result.request_id = text_field(payload, "request_id");
    if (kind == ProviderErrorKind::Unknown) {
        result.status_code = nonnegative_int(payload, "status_code");
        const auto code = text_field(payload, "code");
        if (result.status_code == 0) {
            if (code == "rate_limit_exceeded" || code == "rate_limit_reached") {
                result.status_code = 429;
            } else if (code == "server_error" || code == "internal_server_error") {
                result.status_code = 500;
            } else if (code == "overloaded_error") {
                result.status_code = 503;
            }
        }
        result.retryable = !provider_error_body_has_hard_quota(result.raw_body) &&
            provider_http_error_is_retryable(result.status_code, result.raw_body);
    }
    return result;
}

ChatResponse invalid_response(const Json& envelope, const std::string& message,
                              ProviderErrorKind kind = ProviderErrorKind::MalformedJson) {
    ChatResponse result;
    result.finish_reason = "error";
    result.provider_error = response_error(envelope, kind, message);
    result.content = result.provider_error.display_message;
    return result;
}

void merge_usage(TokenUsage& target, const Json& usage) {
    if (!usage.is_object()) return;
    target.prompt_tokens = nonnegative_int(usage, "input_tokens");
    target.completion_tokens = nonnegative_int(usage, "output_tokens");
    const auto total = static_cast<std::int64_t>(target.prompt_tokens) +
        target.completion_tokens;
    target.total_tokens = nonnegative_int(usage, "total_tokens",
        static_cast<int>((std::min)(total,
            static_cast<std::int64_t>((std::numeric_limits<int>::max)()))));
    if (usage.contains("input_tokens_details")) {
        target.cache_read_tokens = nonnegative_int(usage["input_tokens_details"], "cached_tokens");
    }
    if (usage.contains("output_tokens_details")) {
        target.reasoning_tokens = nonnegative_int(usage["output_tokens_details"], "reasoning_tokens");
    }
    target.has_data = true;
}

bool read_message_text(const Json& item, std::string& text) {
    if (text_field(item, "role") != "assistant" || !has_array(item, "content")) return false;
    for (const auto& part : item["content"]) {
        if (!part.is_object() || !has_string(part, "type")) return false;
        const auto type = text_field(part, "type");
        if (type == "output_text") {
            if (!has_string(part, "text")) return false;
            text += text_field(part, "text");
        } else if (type == "refusal") {
            if (!has_string(part, "refusal")) return false;
            text += text_field(part, "refusal");
        } else {
            return false;
        }
    }
    return !item.contains("phase") || item["phase"].is_null() || item["phase"].is_string();
}

bool read_reasoning_text(const Json& item, std::string& text) {
    if (!has_array(item, "summary")) return false;
    if (item.contains("encrypted_content") && !item["encrypted_content"].is_null() &&
        !item["encrypted_content"].is_string()) return false;
    std::string summary;
    for (const auto& part : item["summary"]) {
        if (text_field(part, "type") != "summary_text" || !has_string(part, "text")) return false;
        summary += text_field(part, "text");
    }
    std::string content;
    if (item.contains("content") && !item["content"].is_null()) {
        if (!item["content"].is_array()) return false;
        for (const auto& part : item["content"]) {
            if (text_field(part, "type") != "reasoning_text" || !has_string(part, "text")) return false;
            content += text_field(part, "text");
        }
    }
    text += content.empty() ? summary : content;
    return true;
}

bool read_function(const Json& item, ToolCall& call, bool require_complete) {
    call.id = text_field(item, "call_id");
    call.function_name = text_field(item, "name");
    call.function_arguments = text_field(item, "arguments");
    if (call.id.empty() || call.function_name.empty() || !has_string(item, "arguments")) return false;
    if (require_complete) {
        if (item.contains("status") && text_field(item, "status") != "completed") return false;
        if (Json::parse(call.function_arguments, nullptr, false).is_discarded()) return false;
    }
    return true;
}

bool calls_equal(const std::vector<ToolCall>& left, const std::vector<ToolCall>& right) {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (left[i].id != right[i].id ||
            left[i].function_name != right[i].function_name ||
            left[i].function_arguments != right[i].function_arguments) return false;
    }
    return true;
}

bool read_chat_calls(const Json& value, std::vector<ToolCall>& calls) {
    if (value.is_null()) return true;
    if (!value.is_array()) return false;
    std::set<std::string> ids;
    for (const auto& item : value) {
        if (!item.is_object() || !item.contains("function") ||
            !item["function"].is_object()) return false;
        ToolCall call{text_field(item, "id"), text_field(item["function"], "name"),
                      text_field(item["function"], "arguments")};
        if (call.id.empty() || call.function_name.empty() ||
            !has_string(item["function"], "arguments") || !ids.insert(call.id).second) return false;
        calls.push_back(std::move(call));
    }
    return true;
}

bool chat_text(const Json& value, std::string& text) {
    if (value.is_null()) return true;
    if (value.is_string()) {
        text = value.get<std::string>();
        return true;
    }
    if (!value.is_array()) return false;
    for (const auto& part : value) {
        const auto type = text_field(part, "type");
        if ((type != "text" && type != "input_text" && type != "output_text") ||
            !has_string(part, "text")) return false;
        text += text_field(part, "text");
    }
    return true;
}

// Replay a native group atomically: never keep its reasoning or a stale call
// while independently synthesizing different canonical assistant content.
Json native_group(const Json& normalized, const ChatMessage* original) {
    if (!original || !original->content_parts.is_array()) return {};
    Json items = Json::array();
    for (const auto& part : original->content_parts) {
        if (text_field(part, "type") != "openai_responses_item") continue;
        if (!part.contains("item") || !part["item"].is_object()) return {};
        const auto& item = part["item"];
        const auto type = text_field(item, "type");
        if (type != "reasoning" && type != "message" && type != "function_call") return {};
        // Storage is disabled, so a bare reasoning ID cannot be resolved later.
        if (type == "reasoning" && text_field(item, "encrypted_content").empty()) return {};
        items.push_back(item);
    }
    if (items.empty()) return {};
    const auto parsed = parse_openai_responses_response({{"status", "completed"}, {"output", items}});
    if (parsed.provider_error.has_error()) return {};
    std::string normalized_text;
    if (!chat_text(normalized.contains("content") ? normalized["content"] : Json(),
                   normalized_text)) return {};
    std::vector<ToolCall> normalized_calls;
    std::vector<ToolCall> original_calls;
    if (!read_chat_calls(normalized.contains("tool_calls") ? normalized["tool_calls"] : Json(),
                         normalized_calls) ||
        !read_chat_calls(original->tool_calls, original_calls)) return {};
    if (parsed.content != normalized_text || parsed.content != original->content ||
        !calls_equal(parsed.tool_calls, normalized_calls) ||
        !calls_equal(parsed.tool_calls, original_calls)) return {};
    return items;
}

Json convert_content(const Json& source, std::string& error) {
    if (source.is_string()) return source;
    if (source.is_null()) return "";
    if (!source.is_array()) {
        error = "message content must be a string or array";
        return {};
    }
    Json parts = Json::array();
    for (const auto& part : source) {
        const auto type = text_field(part, "type");
        if (type == "text" || type == "input_text" || type == "output_text") {
            if (!has_string(part, "text")) {
                error = "text content is missing text";
                return {};
            }
            parts.push_back({{"type", "input_text"}, {"text", part["text"]}});
        } else if (type == "image_url" || type == "input_image") {
            std::string url = text_field(part, "image_url");
            std::string detail = text_field(part, "detail");
            if (part.contains("image_url") && part["image_url"].is_object()) {
                url = text_field(part["image_url"], "url");
                detail = text_field(part["image_url"], "detail");
            }
            if (url.empty()) {
                error = "image content is missing image_url";
                return {};
            }
            parts.push_back({{"type", "input_image"}, {"image_url", url},
                             {"detail", detail.empty() ? "auto" : detail}});
        } else {
            error = "unsupported message content type: " + type;
            return {};
        }
    }
    return parts;
}

} // namespace

Json build_openai_responses_request(const Json& chat_body,
                                    const std::vector<ChatMessage>* original_messages,
                                    std::string* error) {
    std::string local_error;
    if (error) error->clear();
    const auto reject = [&](const std::string& message) -> Json {
        if (error) *error = message;
        return {};
    };
    if (!has_array(chat_body, "messages") || chat_body["messages"].empty()) {
        return reject("messages must be a non-empty array");
    }
    if (text_field(chat_body, "model").empty()) return reject("model is required");
    Json request{{"model", chat_body["model"]}, {"input", Json::array()},
                 {"store", false}, {"include", Json::array({"reasoning.encrypted_content"})},
                 {"stream", chat_body.contains("stream") && chat_body["stream"] == true}};
    std::vector<const ChatMessage*> assistants;
    if (original_messages) {
        for (const auto& message : *original_messages) {
            if (message.role == "assistant") assistants.push_back(&message);
        }
    }
    // A repaired history can drop assistant rows. Disable ordinal replay when
    // counts differ instead of attaching another turn's opaque state.
    const auto assistant_count = std::count_if(chat_body["messages"].begin(),
        chat_body["messages"].end(), [](const Json& message) {
            return text_field(message, "role") == "assistant";
        });
    if (assistants.size() != static_cast<std::size_t>(assistant_count)) assistants.clear();
    std::size_t assistant_index = 0;
    for (const auto& message : chat_body["messages"]) {
        const auto role = text_field(message, "role");
        if (role == "tool") {
            const auto id = text_field(message, "tool_call_id");
            if (id.empty()) return reject("tool message is missing tool_call_id");
            auto output = convert_content(message.contains("content") ? message["content"] : Json(),
                                          local_error);
            if (!local_error.empty()) return reject(local_error);
            request["input"].push_back({{"type", "function_call_output"},
                                        {"call_id", id}, {"output", std::move(output)}});
            continue;
        }
        if (role != "system" && role != "developer" && role != "user" && role != "assistant") {
            return reject("unsupported message role: " + role);
        }
        if (role == "assistant") {
            const auto native = native_group(message, assistant_index < assistants.size()
                ? assistants[assistant_index] : nullptr);
            ++assistant_index;
            if (native.is_array()) {
                for (const auto& item : native) request["input"].push_back(item);
                continue;
            }
        }
        auto content = convert_content(message.contains("content") ? message["content"] : Json(),
                                       local_error);
        if (!local_error.empty()) return reject(local_error);
        const bool has_content = content.is_string()
            ? !content.get_ref<const std::string&>().empty() : !content.empty();
        if (has_content) {
            request["input"].push_back({{"type", "message"}, {"role", role},
                                        {"content", std::move(content)}});
        }
        if (role == "assistant" && message.contains("tool_calls")) {
            std::vector<ToolCall> calls;
            if (!read_chat_calls(message["tool_calls"], calls)) return reject("malformed assistant tool calls");
            for (const auto& call : calls) {
                request["input"].push_back({{"type", "function_call"}, {"call_id", call.id},
                    {"name", call.function_name}, {"arguments", call.function_arguments}});
            }
        }
    }
    if (request["input"].empty()) return reject("messages do not contain sendable input");
    if (chat_body.contains("tools")) {
        if (!chat_body["tools"].is_array()) return reject("tools must be an array");
        request["tools"] = Json::array();
        for (const auto& tool : chat_body["tools"]) {
            if (text_field(tool, "type") != "function" || !tool.contains("function") ||
                !tool["function"].is_object() || text_field(tool["function"], "name").empty()) {
                return reject("only function tools are supported");
            }
            auto flattened = tool["function"];
            flattened["type"] = "function";
            flattened["strict"] = false;
            request["tools"].push_back(std::move(flattened));
        }
    }
    if (chat_body.contains("max_completion_tokens")) {
        request["max_output_tokens"] = chat_body["max_completion_tokens"];
    } else if (chat_body.contains("max_tokens")) {
        request["max_output_tokens"] = chat_body["max_tokens"];
    }
    for (const char* key : {"temperature", "top_p", "parallel_tool_calls", "metadata",
                            "service_tier", "prompt_cache_key", "prompt_cache_retention",
                            "safety_identifier", "reasoning"}) {
        if (chat_body.contains(key)) request[key] = chat_body[key];
    }
    if (chat_body.contains("reasoning_effort")) {
        if (request.contains("reasoning") && !request["reasoning"].is_object()) {
            return reject("reasoning must be an object");
        }
        request["reasoning"]["effort"] = chat_body["reasoning_effort"];
    }
    if (chat_body.contains("tool_choice")) {
        const auto& choice = chat_body["tool_choice"];
        if (choice.is_string()) {
            request["tool_choice"] = choice;
        } else if (text_field(choice, "type") == "function" && choice.contains("function") &&
                   !text_field(choice["function"], "name").empty()) {
            request["tool_choice"] = {{"type", "function"}, {"name", choice["function"]["name"]}};
        } else {
            return reject("unsupported tool_choice");
        }
    }
    if (chat_body.contains("response_format")) {
        auto format = chat_body["response_format"];
        if (text_field(format, "type") == "json_schema" && format.contains("json_schema") &&
            format["json_schema"].is_object()) {
            format = format["json_schema"];
            format["type"] = "json_schema";
        }
        request["text"]["format"] = std::move(format);
    }
    return request;
}

ChatResponse parse_openai_responses_response(const Json& envelope) {
    if (!envelope.is_object()) return invalid_response(envelope, "Invalid OpenAI Responses envelope");
    const auto status = text_field(envelope, "status");
    if ((envelope.contains("error") && !envelope["error"].is_null()) ||
        status == "failed" || status == "cancelled") {
        return invalid_response(envelope, "OpenAI response failed", ProviderErrorKind::Unknown);
    }
    if ((status != "completed" && status != "incomplete") || !has_array(envelope, "output")) {
        return invalid_response(envelope, "OpenAI response is missing terminal status or output");
    }
    ChatResponse result;
    std::set<std::string> calls;
    std::set<std::string> item_ids;
    for (const auto& item : envelope["output"]) {
        if (!item.is_object() || !has_string(item, "type")) {
            return invalid_response(envelope, "Malformed OpenAI output item");
        }
        const auto id = text_field(item, "id");
        if (!id.empty() && !item_ids.insert(id).second) {
            return invalid_response(envelope, "Duplicate OpenAI output item ID");
        }
        const auto type = text_field(item, "type");
        if (type == "message") {
            if (!read_message_text(item, result.content)) {
                return invalid_response(envelope, "Malformed OpenAI message output");
            }
        } else if (type == "reasoning") {
            if (!read_reasoning_text(item, result.reasoning_content)) {
                return invalid_response(envelope, "Malformed OpenAI reasoning output");
            }
        } else if (type == "function_call") {
            // Partial calls are neither executable nor replayable. They may
            // legitimately lack a name, call_id, or complete JSON arguments.
            if (status == "incomplete") continue;
            ToolCall call;
            if (!read_function(item, call, true) || !calls.insert(call.id).second) {
                return invalid_response(envelope, "Malformed or duplicate OpenAI function call");
            }
            result.tool_calls.push_back(std::move(call));
        } else {
            // Future output types are never turned into local executable calls.
            continue;
        }
        result.content_parts.push_back({{"type", "openai_responses_item"}, {"item", item}});
    }
    if (envelope.contains("usage")) merge_usage(result.usage, envelope["usage"]);
    if (status == "incomplete") {
        const auto reason = envelope.contains("incomplete_details")
            ? text_field(envelope["incomplete_details"], "reason") : std::string{};
        result.finish_reason = reason == "content_filter" ? "content_filter" : "length";
    } else {
        result.finish_reason = result.tool_calls.empty() ? "stop" : "tool_calls";
    }
    return result;
}

bool OpenAiResponsesStreamParser::identify_output(const Json& event, const Json* item, int& index) {
    if (!valid_index(event, "output_index")) return false;
    if ((event.contains("item_id") && !has_string(event, "item_id")) ||
        (item && item->contains("id") && !has_string(*item, "id"))) return false;
    index = event["output_index"].get<int>();
    const auto id = item ? text_field(*item, "id") : text_field(event, "item_id");
    if (!id.empty()) {
        const auto known = index_by_id_.find(id);
        if (known != index_by_id_.end() && known->second != index) return false;
        const auto previous = output_.find(index);
        if (previous != output_.end()) {
            const auto old_id = text_field(previous->second.item, "id");
            if (!old_id.empty() && old_id != id) return false;
        }
        index_by_id_[id] = index;
        output_[index].item["id"] = id;
    }
    return true;
}

std::vector<StreamEvent> OpenAiResponsesStreamParser::fail(
        const Json& event, ProviderErrorKind kind, const std::string& message, bool retryable) {
    if (terminal_) return {};
    accumulated_.finish_reason = "error";
    accumulated_.tool_calls.clear();
    accumulated_.content_parts = Json::array();
    accumulated_.provider_error = response_error(event, kind, message);
    accumulated_.provider_error.retryable = accumulated_.provider_error.retryable || retryable;
    StreamEvent failure;
    failure.type = StreamEventType::Error;
    failure.error = accumulated_.provider_error.display_message;
    failure.provider_error = accumulated_.provider_error;
    terminal_ = true;
    return {std::move(failure)};
}

std::vector<StreamEvent> OpenAiResponsesStreamParser::complete(const Json& event) {
    if (!event.contains("response") || !event["response"].is_object()) {
        return fail(event, ProviderErrorKind::MalformedSse, "Terminal OpenAI event is missing response");
    }
    auto envelope = event["response"];
    const std::string expected_status = text_field(event, "type") == "response.completed" ? "completed" : "incomplete";
    if (text_field(envelope, "status") != expected_status || !has_array(envelope, "output")) {
        return fail(event, ProviderErrorKind::MalformedSse, "Malformed terminal OpenAI response");
    }
    for (const auto& entry : output_) {
        const auto index = static_cast<std::size_t>(entry.first);
        const auto& state = entry.second;
        if (index >= envelope["output"].size() || !envelope["output"][index].is_object()) {
            return fail(event, ProviderErrorKind::MalformedSse, "Terminal response omitted a streamed output item");
        }
        auto& final_item = envelope["output"][index];
        const auto id = text_field(state.item, "id");
        const auto type = text_field(state.item, "type");
        if ((!id.empty() && !text_field(final_item, "id").empty() && id != text_field(final_item, "id")) ||
            (!type.empty() && type != text_field(final_item, "type"))) {
            return fail(event, ProviderErrorKind::MalformedSse, "OpenAI output item changed identity");
        }
        if (expected_status == "completed" && state.arguments_seen &&
            !prefix_of(state.arguments, text_field(final_item, "arguments"))) {
            return fail(event, ProviderErrorKind::MalformedSse, "OpenAI function arguments changed during streaming");
        }
        if (state.done) {
            // Some gateways omit opaque fields from the terminal snapshot.
            // Only output_item.done is a safe fallback; added can be partial.
            for (const char* key : {"encrypted_content", "phase"}) {
                if ((!final_item.contains(key) || final_item[key].is_null()) && state.item.contains(key)) {
                    final_item[key] = state.item[key];
                }
            }
            if (expected_status == "completed" && type == "function_call" &&
                (text_field(state.item, "call_id") != text_field(final_item, "call_id") ||
                 text_field(state.item, "name") != text_field(final_item, "name"))) {
                return fail(event, ProviderErrorKind::MalformedSse, "OpenAI function call changed identity");
            }
        }
    }
    auto final = parse_openai_responses_response(envelope);
    if (final.provider_error.has_error()) {
        return fail(event, final.provider_error.kind == ProviderErrorKind::MalformedJson
            ? ProviderErrorKind::MalformedSse : final.provider_error.kind,
            final.provider_error.display_message);
    }
    if (!prefix_of(accumulated_.content, final.content) ||
        !prefix_of(accumulated_.reasoning_content, final.reasoning_content)) {
        return fail(event, ProviderErrorKind::MalformedSse, "OpenAI terminal output disagrees with streamed text");
    }
    std::vector<StreamEvent> events;
    const auto emit_suffix = [&](StreamEventType type, const std::string& previous, const std::string& complete_text) {
        if (complete_text.size() <= previous.size()) return;
        StreamEvent delta;
        delta.type = type;
        delta.content = complete_text.substr(previous.size());
        events.push_back(std::move(delta));
    };
    emit_suffix(StreamEventType::Delta, accumulated_.content, final.content);
    emit_suffix(StreamEventType::ReasoningDelta, accumulated_.reasoning_content, final.reasoning_content);
    for (std::size_t i = 0; i < envelope["output"].size(); ++i) {
        if (expected_status != "completed" ||
            text_field(envelope["output"][i], "type") != "function_call") continue;
        StreamEvent call;
        call.type = StreamEventType::ToolCall;
        read_function(envelope["output"][i], call.tool_call, true);
        call.tool_index = static_cast<int>(i);
        events.push_back(std::move(call));
    }
    if (final.usage.has_data) {
        StreamEvent usage;
        usage.type = StreamEventType::Usage;
        usage.usage = final.usage;
        events.push_back(std::move(usage));
    }
    accumulated_ = std::move(final);
    StreamEvent done;
    done.type = StreamEventType::Done;
    done.finish_reason = accumulated_.finish_reason;
    done.content_parts = accumulated_.content_parts;
    events.push_back(std::move(done));
    terminal_ = true;
    return events;
}

std::vector<StreamEvent> OpenAiResponsesStreamParser::consume(const Json& event) {
    if (terminal_) return {};
    if (!has_string(event, "type") || text_field(event, "type").empty()) {
        return fail(event, ProviderErrorKind::MalformedSse, "Malformed OpenAI SSE event type");
    }
    const auto type = text_field(event, "type");
    if (type == "error" || type == "response.failed" || type == "response.cancelled") {
        return fail(event, ProviderErrorKind::Unknown, "OpenAI response failed");
    }
    if (type == "response.completed" || type == "response.incomplete") return complete(event);
    if (type == "response.created" || type == "response.in_progress" || type == "response.queued") {
        if (!event.contains("response") || !event["response"].is_object()) {
            return fail(event, ProviderErrorKind::MalformedSse, "OpenAI lifecycle event is missing response");
        }
        return {};
    }
    const bool text_delta = type == "response.output_text.delta" || type == "response.refusal.delta";
    const bool reasoning_delta = type == "response.reasoning_summary_text.delta" || type == "response.reasoning_text.delta";
    if (text_delta || reasoning_delta) {
        int index = -1;
        if (!has_string(event, "delta") || !identify_output(event, nullptr, index)) {
            return fail(event, ProviderErrorKind::MalformedSse, "Malformed OpenAI text delta");
        }
        const auto delta = text_field(event, "delta");
        if (delta.empty()) return {};
        if (text_delta) accumulated_.content += delta;
        else accumulated_.reasoning_content += delta;
        StreamEvent result;
        result.type = text_delta ? StreamEventType::Delta : StreamEventType::ReasoningDelta;
        result.content = delta;
        return {std::move(result)};
    }
    if (type == "response.output_item.added" || type == "response.output_item.done") {
        int index = -1;
        if (!event.contains("item") || !event["item"].is_object() ||
            !has_string(event["item"], "type") || !identify_output(event, &event["item"], index)) {
            return fail(event, ProviderErrorKind::MalformedSse, "Malformed OpenAI output item event");
        }
        auto& state = output_[index];
        const auto& item = event["item"];
        const auto item_type = text_field(item, "type");
        const auto previous_type = text_field(state.item, "type");
        for (const char* key : {"call_id", "name", "arguments", "role", "status"}) {
            if (item.contains(key) && !has_string(item, key)) {
                return fail(event, ProviderErrorKind::MalformedSse, "Malformed OpenAI output item field");
            }
        }
        if ((!previous_type.empty() && previous_type != item_type) ||
            (state.done && state.item != item)) {
            return fail(event, ProviderErrorKind::MalformedSse, "OpenAI output item changed after completion");
        }
        if (type == "response.output_item.done") {
            if (item_type == "function_call") {
                ToolCall call;
                if (text_field(item, "status") == "incomplete") {
                    // output_item.done also closes unfinished items when the
                    // response runs out of tokens. Await the terminal status;
                    // no executable call is emitted from this event.
                    state.arguments = text_field(item, "arguments");
                    state.arguments_seen = has_string(item, "arguments");
                } else if (!read_function(item, call, false) ||
                    (state.arguments_seen && !prefix_of(state.arguments, call.function_arguments))) {
                    return fail(event, ProviderErrorKind::MalformedSse, "Malformed completed OpenAI function item");
                } else {
                    state.arguments = call.function_arguments;
                    state.arguments_seen = true;
                }
            } else if (item_type == "message" || item_type == "reasoning") {
                std::string ignored;
                if (!(item_type == "message" ? read_message_text(item, ignored) : read_reasoning_text(item, ignored))) {
                    return fail(event, ProviderErrorKind::MalformedSse, "Malformed completed OpenAI output item");
                }
            }
            state.done = true;
        }
        state.item = item;
        return {};
    }
    if (type == "response.function_call_arguments.delta" || type == "response.function_call_arguments.done") {
        int index = -1;
        const auto key = type == "response.function_call_arguments.delta" ? "delta" : "arguments";
        if (!has_string(event, key) || !identify_output(event, nullptr, index)) {
            return fail(event, ProviderErrorKind::MalformedSse, "Malformed OpenAI function arguments event");
        }
        auto& state = output_[index];
        const auto item_type = text_field(state.item, "type");
        if ((!item_type.empty() && item_type != "function_call") || state.done) {
            return fail(event, ProviderErrorKind::MalformedSse, "OpenAI arguments do not belong to an active function call");
        }
        state.item["type"] = "function_call";
        const auto value = text_field(event, key);
        if (type == "response.function_call_arguments.done") {
            if (!prefix_of(state.arguments, value)) {
                return fail(event, ProviderErrorKind::MalformedSse, "OpenAI completed arguments disagree with deltas");
            }
            state.arguments = value;
            state.arguments_seen = true;
            return {};
        }
        state.arguments += value;
        state.arguments_seen = true;
        StreamEvent progress;
        progress.type = StreamEventType::ToolCallDelta;
        progress.tool_index = index;
        progress.tool_call.id = text_field(state.item, "call_id");
        progress.tool_call.function_name = text_field(state.item, "name");
        progress.tool_call_argument_bytes = state.arguments.size();
        return {std::move(progress)};
    }
    if (type == "response.output_text.done" || type == "response.refusal.done" ||
        type == "response.reasoning_summary_text.done" || type == "response.reasoning_text.done") {
        int index = -1;
        const auto key = type == "response.refusal.done" ? "refusal" : "text";
        if (!has_string(event, key) || !identify_output(event, nullptr, index)) {
            return fail(event, ProviderErrorKind::MalformedSse, "Malformed OpenAI text completion event");
        }
        return {};
    }
    if (type == "response.content_part.added" || type == "response.content_part.done" ||
        type == "response.reasoning_summary_part.added" || type == "response.reasoning_summary_part.done") {
        int index = -1;
        if (!event.contains("part") || !event["part"].is_object() ||
            !has_string(event["part"], "type") || !identify_output(event, nullptr, index)) {
            return fail(event, ProviderErrorKind::MalformedSse, "Malformed OpenAI content part event");
        }
        const auto& part = event["part"];
        const auto part_type = text_field(part, "type");
        if (((part_type == "output_text" || part_type == "summary_text" || part_type == "reasoning_text") &&
             !has_string(part, "text")) ||
            (part_type == "refusal" && !has_string(part, "refusal"))) {
            return fail(event, ProviderErrorKind::MalformedSse, "Malformed OpenAI content part text");
        }
        return {};
    }
    return {}; // Unknown future events are safe to ignore.
}

std::vector<StreamEvent> OpenAiResponsesStreamParser::finish() {
    if (terminal_) return {};
    return fail(Json::object(), ProviderErrorKind::MalformedSse,
                "OpenAI stream ended before a terminal response event", true);
}

} // namespace acecode
