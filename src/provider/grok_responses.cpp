#include "grok_responses.hpp"

#include "auth/xai_auth.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <sstream>

namespace acecode {
namespace {

int json_int(const nlohmann::json& value, const char* key) {
    if (!value.is_object() || !value.contains(key)) return 0;
    const auto& field = value[key];
    if (!field.is_number_integer() && !field.is_number_unsigned()) return 0;
    const auto raw = field.get<long long>();
    return static_cast<int>(std::clamp<long long>(
        raw, 0, std::numeric_limits<int>::max()));
}

std::string string_field(const nlohmann::json& value, const char* key) {
    return value.is_object() && value.contains(key) && value[key].is_string()
        ? value[key].get<std::string>()
        : std::string{};
}

std::string event_error_message(const nlohmann::json& value,
                                const std::string& fallback) {
    if (!value.is_object()) return fallback;
    if (value.contains("error")) {
        const auto& error = value["error"];
        if (error.is_string() && !error.get<std::string>().empty()) {
            return error.get<std::string>();
        }
        if (error.is_object()) {
            for (const char* key : {"message", "detail", "code", "type"}) {
                const std::string message = string_field(error, key);
                if (!message.empty()) return message;
            }
        }
    }
    for (const char* key : {"message", "detail"}) {
        const std::string message = string_field(value, key);
        if (!message.empty()) return message;
    }
    if (value.contains("response") && value["response"].is_object()) {
        return event_error_message(value["response"], fallback);
    }
    return fallback;
}

void merge_grok_usage(TokenUsage& target, const nlohmann::json& usage) {
    if (!usage.is_object()) return;
    target.prompt_tokens = json_int(usage, "input_tokens");
    target.completion_tokens = json_int(usage, "output_tokens");
    target.total_tokens = json_int(usage, "total_tokens");
    if (target.total_tokens == 0) {
        target.total_tokens = target.prompt_tokens + target.completion_tokens;
    }
    if (usage.contains("input_tokens_details") &&
        usage["input_tokens_details"].is_object()) {
        target.cache_read_tokens = json_int(
            usage["input_tokens_details"], "cached_tokens");
    }
    if (usage.contains("output_tokens_details") &&
        usage["output_tokens_details"].is_object()) {
        target.reasoning_tokens = json_int(
            usage["output_tokens_details"], "reasoning_tokens");
    }
    target.has_data = target.prompt_tokens > 0 ||
        target.completion_tokens > 0 || target.total_tokens > 0 ||
        target.cache_read_tokens > 0 || target.reasoning_tokens > 0;
}

nlohmann::json grok_reasoning_part(const nlohmann::json& item) {
    nlohmann::json part{
        {"type", "grok_reasoning"},
        {"summary", item.contains("summary") && item["summary"].is_array()
            ? item["summary"]
            : nlohmann::json::array()},
    };
    const std::string encrypted = string_field(item, "encrypted_content");
    if (!encrypted.empty()) part["encrypted_content"] = encrypted;
    if (item.contains("content") && item["content"].is_array()) {
        part["content"] = item["content"];
    }
    return part;
}

std::string reasoning_text(const nlohmann::json& item) {
    std::string text;
    if (item.contains("content") && item["content"].is_array()) {
        for (const auto& part : item["content"]) {
            if (part.is_object() &&
                part.value("type", std::string{}) == "reasoning_text") {
                text += string_field(part, "text");
            }
        }
    }
    if (!text.empty()) return text;
    if (item.contains("summary") && item["summary"].is_array()) {
        for (const auto& part : item["summary"]) {
            if (part.is_object()) text += string_field(part, "text");
        }
    }
    return text;
}

std::string stable_tool_id(const nlohmann::json& item, int index) {
    std::string id = string_field(item, "call_id");
    if (id.empty()) id = string_field(item, "id");
    if (id.empty()) id = "call_grok_" + std::to_string(index);
    return id;
}

nlohmann::json responses_content(const nlohmann::json& source,
                                 std::string* error) {
    if (source.is_string() || source.is_null()) return source;
    if (!source.is_array()) {
        if (error) *error = "message content must be a string or array";
        return {};
    }
    nlohmann::json parts = nlohmann::json::array();
    for (const auto& source_part : source) {
        if (!source_part.is_object()) continue;
        const std::string type = source_part.value("type", std::string{});
        if (type == "text" || type == "input_text" || type == "output_text") {
            parts.push_back(nlohmann::json{
                {"type", "input_text"},
                {"text", source_part.value("text", std::string{})},
            });
            continue;
        }
        if (type == "image_url" || type == "input_image") {
            std::string url;
            std::string detail = source_part.value("detail", std::string{"auto"});
            if (source_part.contains("image_url") &&
                source_part["image_url"].is_string()) {
                url = source_part["image_url"].get<std::string>();
            } else if (source_part.contains("image_url") &&
                       source_part["image_url"].is_object()) {
                url = source_part["image_url"].value("url", std::string{});
                detail = source_part["image_url"].value("detail", detail);
            } else {
                url = source_part.value("url", std::string{});
            }
            if (url.empty()) {
                if (error) *error = "image_url is missing a URL";
                return {};
            }
            parts.push_back(nlohmann::json{
                {"type", "input_image"},
                {"detail", detail.empty() ? "auto" : detail},
                {"image_url", url},
            });
            continue;
        }
        if (error) *error = "unsupported message content type: " + type;
        return {};
    }
    return parts;
}

std::vector<nlohmann::json> encrypted_reasoning_by_assistant(
        const std::vector<ChatMessage>* messages) {
    std::vector<nlohmann::json> by_assistant;
    if (!messages) return by_assistant;
    for (const auto& message : *messages) {
        if (message.role != "assistant") continue;
        nlohmann::json items = nlohmann::json::array();
        if (message.content_parts.is_array()) {
            for (const auto& part : message.content_parts) {
                if (!part.is_object() ||
                    part.value("type", std::string{}) != "grok_reasoning") {
                    continue;
                }
                const std::string encrypted =
                    string_field(part, "encrypted_content");
                if (encrypted.empty()) continue;
                items.push_back(nlohmann::json{
                    {"type", "reasoning"},
                    {"summary", part.contains("summary") && part["summary"].is_array()
                        ? part["summary"]
                        : nlohmann::json::array()},
                    {"encrypted_content", encrypted},
                });
            }
        }
        by_assistant.push_back(std::move(items));
    }
    return by_assistant;
}

void set_error(std::string* error, const std::string& message) {
    if (error) *error = message;
}

} // namespace

nlohmann::json build_grok_responses_request(
        const nlohmann::json& chat_body,
        const std::vector<ChatMessage>* original_messages,
        std::string* error) {
    if (error) error->clear();
    if (!chat_body.is_object() || !chat_body.contains("messages") ||
        !chat_body["messages"].is_array() || chat_body["messages"].empty()) {
        set_error(error, "messages must be a non-empty array");
        return {};
    }
    const std::string model = chat_body.value("model", std::string{});
    if (model.empty()) {
        set_error(error, "model is required");
        return {};
    }

    nlohmann::json request{
        {"model", model},
        {"input", nlohmann::json::array()},
        {"store", false},
        {"include", nlohmann::json::array({"reasoning.encrypted_content"})},
    };
    request["stream"] = chat_body.value("stream", false);
    const auto native_reasoning = encrypted_reasoning_by_assistant(original_messages);
    std::size_t assistant_index = 0;

    for (const auto& message : chat_body["messages"]) {
        if (!message.is_object()) continue;
        const std::string role = message.value("role", std::string{});
        if (role == "tool") {
            const std::string call_id =
                message.value("tool_call_id", std::string{});
            if (call_id.empty()) {
                set_error(error, "tool message is missing tool_call_id");
                return {};
            }
            nlohmann::json output = message.contains("content")
                ? responses_content(message["content"], error)
                : nlohmann::json("");
            if (error && !error->empty()) return {};
            request["input"].push_back(nlohmann::json{
                {"type", "function_call_output"},
                {"call_id", call_id},
                {"output", std::move(output)},
            });
            continue;
        }

        if (role != "system" && role != "developer" && role != "user" &&
            role != "assistant") {
            continue;
        }
        if (role == "assistant") {
            if (assistant_index < native_reasoning.size()) {
                for (const auto& item : native_reasoning[assistant_index]) {
                    request["input"].push_back(item);
                }
            }
            ++assistant_index;
        }

        if (message.contains("content") && !message["content"].is_null()) {
            nlohmann::json content = responses_content(message["content"], error);
            if (error && !error->empty()) return {};
            const bool empty_array = content.is_array() && content.empty();
            const bool empty_string = content.is_string() &&
                content.get_ref<const std::string&>().empty();
            if (!empty_array && !empty_string) {
                request["input"].push_back(nlohmann::json{
                    {"type", "message"},
                    {"role", role},
                    {"content", std::move(content)},
                });
            }
        }

        if (role == "assistant" && message.contains("tool_calls") &&
            message["tool_calls"].is_array()) {
            for (const auto& tool_call : message["tool_calls"]) {
                if (!tool_call.is_object() ||
                    !tool_call.contains("function") ||
                    !tool_call["function"].is_object()) {
                    set_error(error, "assistant tool call is malformed");
                    return {};
                }
                const auto& fn = tool_call["function"];
                const std::string call_id = tool_call.value("id", std::string{});
                const std::string name = fn.value("name", std::string{});
                if (call_id.empty() || name.empty()) {
                    set_error(error, "assistant tool call is missing id or name");
                    return {};
                }
                request["input"].push_back(nlohmann::json{
                    {"type", "function_call"},
                    {"call_id", call_id},
                    {"name", name},
                    {"arguments", fn.value("arguments", std::string{"{}"})},
                });
            }
        }
    }

    if (request["input"].empty()) {
        set_error(error, "messages do not contain sendable input");
        return {};
    }
    if (chat_body.contains("tools") && chat_body["tools"].is_array()) {
        request["tools"] = nlohmann::json::array();
        for (const auto& tool : chat_body["tools"]) {
            if (!tool.is_object()) continue;
            if (tool.value("type", std::string{}) == "function" &&
                tool.contains("function") && tool["function"].is_object()) {
                nlohmann::json flattened = tool["function"];
                flattened["type"] = "function";
                request["tools"].push_back(std::move(flattened));
            } else {
                request["tools"].push_back(tool);
            }
        }
    }
    if (chat_body.contains("max_tokens")) {
        request["max_output_tokens"] = chat_body["max_tokens"];
    }
    for (const char* key : {
             "temperature", "top_p", "parallel_tool_calls", "metadata",
             "service_tier", "reasoning", "tool_choice"}) {
        if (chat_body.contains(key)) request[key] = chat_body[key];
    }
    return request;
}

ChatResponse parse_grok_responses_response(const nlohmann::json& envelope) {
    ChatResponse response;
    response.content_parts = nlohmann::json::array();
    if (!envelope.is_object()) {
        response.finish_reason = "error";
        response.content = "[Error] Invalid Grok Responses envelope.";
        response.provider_error.kind = ProviderErrorKind::MalformedJson;
        response.provider_error.provider = "grok";
        response.provider_error.display_message = response.content;
        return response;
    }
    if (envelope.contains("error") && !envelope["error"].is_null()) {
        response.finish_reason = "error";
        response.content = redact_grok_auth_diagnostic(
            event_error_message(envelope, "Grok response failed"));
        response.provider_error.kind = ProviderErrorKind::Unknown;
        response.provider_error.provider = "grok";
        response.provider_error.display_message = response.content;
        response.provider_error.body_is_json = true;
        response.provider_error.pretty_json =
            redact_grok_auth_diagnostic(envelope.dump(2));
        return response;
    }

    int tool_index = 0;
    if (envelope.contains("output") && envelope["output"].is_array()) {
        for (const auto& item : envelope["output"]) {
            if (!item.is_object()) continue;
            const std::string type = item.value("type", std::string{});
            if (type == "message" && item.contains("content") &&
                item["content"].is_array()) {
                for (const auto& content : item["content"]) {
                    if (!content.is_object()) continue;
                    const std::string content_type =
                        content.value("type", std::string{});
                    if (content_type == "output_text") {
                        response.content += string_field(content, "text");
                    } else if (content_type == "refusal") {
                        response.content += string_field(content, "refusal");
                    }
                }
            } else if (type == "reasoning") {
                response.reasoning_content += reasoning_text(item);
                if (!string_field(item, "encrypted_content").empty()) {
                    response.content_parts.push_back(grok_reasoning_part(item));
                }
            } else if (type == "function_call") {
                ToolCall call;
                call.id = stable_tool_id(item, tool_index);
                call.function_name = string_field(item, "name");
                call.function_arguments = string_field(item, "arguments");
                if (call.function_arguments.empty()) call.function_arguments = "{}";
                response.tool_calls.push_back(std::move(call));
                ++tool_index;
            }
        }
    }
    if (envelope.contains("usage")) merge_grok_usage(response.usage, envelope["usage"]);
    const std::string status = envelope.value("status", std::string{"completed"});
    if (status == "failed" || status == "cancelled") {
        response.finish_reason = "error";
    } else if (status == "incomplete") {
        response.finish_reason = "length";
    } else if (!response.tool_calls.empty()) {
        response.finish_reason = "tool_calls";
    } else {
        response.finish_reason = "stop";
    }
    return response;
}

GrokResponsesStreamParser::ToolState&
GrokResponsesStreamParser::tool_state_for(const nlohmann::json& event,
                                           const nlohmann::json* item) {
    int index = -1;
    if (event.contains("output_index") && event["output_index"].is_number_integer()) {
        index = event["output_index"].get<int>();
    }
    const std::string item_id = item
        ? string_field(*item, "id")
        : string_field(event, "item_id");
    if (index < 0 && !item_id.empty()) {
        auto found = tool_index_by_item_id_.find(item_id);
        if (found != tool_index_by_item_id_.end()) index = found->second;
    }
    if (index < 0) index = next_tool_index_++;
    next_tool_index_ = std::max(next_tool_index_, index + 1);
    auto [it, inserted] = tools_by_index_.try_emplace(index);
    if (inserted) it->second.index = index;
    if (!item_id.empty()) {
        it->second.item_id = item_id;
        tool_index_by_item_id_[item_id] = index;
    }
    return it->second;
}

void GrokResponsesStreamParser::update_tool_state(
        ToolState& state,
        const nlohmann::json& item) {
    const std::string id = stable_tool_id(item, state.index);
    if (!id.empty()) state.call.id = id;
    const std::string name = string_field(item, "name");
    if (!name.empty()) state.call.function_name = name;
    const std::string arguments = string_field(item, "arguments");
    if (!arguments.empty()) state.call.function_arguments = arguments;
}

void GrokResponsesStreamParser::emit_tool_once(
        ToolState& state,
        std::vector<StreamEvent>& events) {
    if (state.emitted) return;
    if (state.call.id.empty()) {
        state.call.id = "call_grok_" + std::to_string(state.index);
    }
    if (state.call.function_arguments.empty()) state.call.function_arguments = "{}";
    accumulated_.tool_calls.push_back(state.call);
    StreamEvent event;
    event.type = StreamEventType::ToolCall;
    event.tool_call = state.call;
    event.tool_index = state.index;
    events.push_back(std::move(event));
    state.emitted = true;
}

void GrokResponsesStreamParser::capture_reasoning_item(
        const nlohmann::json& item) {
    const std::string encrypted = string_field(item, "encrypted_content");
    if (encrypted.empty()) return;
    if (!accumulated_.content_parts.is_array()) {
        accumulated_.content_parts = nlohmann::json::array();
    }
    for (const auto& existing : accumulated_.content_parts) {
        if (existing.is_object() &&
            string_field(existing, "encrypted_content") == encrypted) {
            return;
        }
    }
    accumulated_.content_parts.push_back(grok_reasoning_part(item));
}

void GrokResponsesStreamParser::merge_final_envelope(
        const nlohmann::json& envelope,
        std::vector<StreamEvent>& events,
        bool incomplete) {
    ChatResponse final = parse_grok_responses_response(envelope);
    if (final.finish_reason == "error") {
        auto failures = fail(envelope, ProviderErrorKind::Unknown,
                             "Grok response failed");
        events.insert(events.end(), failures.begin(), failures.end());
        return;
    }
    for (const auto& call : final.tool_calls) {
        ToolState* matched = nullptr;
        for (auto& [index, state] : tools_by_index_) {
            (void)index;
            if (state.call.id == call.id ||
                (!state.item_id.empty() && state.item_id == call.id)) {
                matched = &state;
                break;
            }
        }
        if (!matched) {
            auto& state = tools_by_index_[next_tool_index_];
            state.index = next_tool_index_++;
            matched = &state;
        }
        matched->call = call;
        emit_tool_once(*matched, events);
    }
    for (const auto& part : final.content_parts) {
        if (part.is_object()) capture_reasoning_item(part);
    }
    if (final.usage.has_data && !usage_emitted_) {
        accumulated_.usage = final.usage;
        StreamEvent usage;
        usage.type = StreamEventType::Usage;
        usage.usage = final.usage;
        events.push_back(std::move(usage));
        usage_emitted_ = true;
    }
    for (auto& [index, state] : tools_by_index_) {
        (void)index;
        if (!state.emitted && !state.call.function_name.empty()) {
            emit_tool_once(state, events);
        }
    }
    accumulated_.finish_reason = incomplete
        ? "length"
        : (!accumulated_.tool_calls.empty() ? "tool_calls" : "stop");
    StreamEvent done;
    done.type = StreamEventType::Done;
    done.finish_reason = accumulated_.finish_reason;
    done.content_parts = accumulated_.content_parts;
    events.push_back(std::move(done));
    terminal_ = true;
}

std::vector<StreamEvent> GrokResponsesStreamParser::fail(
        const nlohmann::json& event,
        ProviderErrorKind kind,
        const std::string& fallback) {
    if (terminal_) return {};
    const std::string message = redact_grok_auth_diagnostic(
        event_error_message(event, fallback));
    accumulated_.finish_reason = "error";
    accumulated_.provider_error.kind = kind;
    accumulated_.provider_error.provider = "grok";
    accumulated_.provider_error.display_message = message;
    accumulated_.provider_error.body_is_json = event.is_object();
    if (event.is_object()) {
        accumulated_.provider_error.pretty_json =
            redact_grok_auth_diagnostic(event.dump(2));
    }
    StreamEvent error;
    error.type = StreamEventType::Error;
    error.error = message;
    error.provider_error = accumulated_.provider_error;
    terminal_ = true;
    return {std::move(error)};
}

std::vector<StreamEvent> GrokResponsesStreamParser::consume(
        const nlohmann::json& event) {
    if (terminal_) return {};
    if (!event.is_object()) {
        return fail(event, ProviderErrorKind::MalformedJson,
                    "Malformed Grok SSE event");
    }
    const std::string type = event.value("type", std::string{});
    std::vector<StreamEvent> events;
    if (type == "response.output_text.delta" ||
        type == "response.refusal.delta") {
        const std::string delta = event.value("delta", std::string{});
        if (!delta.empty()) {
            accumulated_.content += delta;
            StreamEvent output;
            output.type = StreamEventType::Delta;
            output.content = delta;
            events.push_back(std::move(output));
        }
    } else if (type == "response.reasoning_summary_text.delta" ||
               type == "response.reasoning_text.delta") {
        const std::string delta = event.value("delta", std::string{});
        if (!delta.empty()) {
            accumulated_.reasoning_content += delta;
            StreamEvent reasoning;
            reasoning.type = StreamEventType::ReasoningDelta;
            reasoning.content = delta;
            events.push_back(std::move(reasoning));
        }
    } else if (type == "response.output_item.added" &&
               event.contains("item") && event["item"].is_object()) {
        const auto& item = event["item"];
        if (item.value("type", std::string{}) == "function_call") {
            auto& state = tool_state_for(event, &item);
            update_tool_state(state, item);
        } else if (item.value("type", std::string{}) == "reasoning") {
            capture_reasoning_item(item);
        }
    } else if (type == "response.function_call_arguments.delta") {
        auto& state = tool_state_for(event);
        state.call.function_arguments += event.value("delta", std::string{});
        StreamEvent delta;
        delta.type = StreamEventType::ToolCallDelta;
        delta.tool_call = state.call;
        delta.tool_index = state.index;
        delta.tool_call_argument_bytes = state.call.function_arguments.size();
        events.push_back(std::move(delta));
    } else if (type == "response.function_call_arguments.done") {
        auto& state = tool_state_for(event);
        const std::string arguments = event.value("arguments", std::string{});
        if (!arguments.empty()) state.call.function_arguments = arguments;
    } else if (type == "response.output_item.done" &&
               event.contains("item") && event["item"].is_object()) {
        const auto& item = event["item"];
        if (item.value("type", std::string{}) == "function_call") {
            auto& state = tool_state_for(event, &item);
            update_tool_state(state, item);
            emit_tool_once(state, events);
        } else if (item.value("type", std::string{}) == "reasoning") {
            capture_reasoning_item(item);
        }
    } else if (type == "response.completed" ||
               type == "response.incomplete") {
        const nlohmann::json& envelope = event.contains("response") &&
            event["response"].is_object() ? event["response"] : event;
        merge_final_envelope(envelope, events, type == "response.incomplete");
    } else if (type == "response.failed" || type == "error") {
        return fail(event, ProviderErrorKind::Unknown, "Grok response failed");
    }
    return events;
}

std::vector<StreamEvent> GrokResponsesStreamParser::finish() {
    if (terminal_) return {};
    return fail(nlohmann::json{{"message", "Grok stream ended before response.completed"}},
                ProviderErrorKind::MalformedSse,
                "Grok stream ended before response.completed");
}

} // namespace acecode
