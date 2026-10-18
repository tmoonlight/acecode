#include "provider/openai_responses.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <string>
#include <vector>

namespace {

using Json = nlohmann::json;
using acecode::ChatMessage;
using acecode::OpenAiResponsesStreamParser;
using acecode::StreamEvent;
using acecode::StreamEventType;

Json chat_body() {
    return {{"model", "gpt-6.1-sol"}, {"messages", Json::array({
        Json{{"role", "system"}, {"content", "Be precise."}},
        Json{{"role", "user"}, {"content", "Inspect this repository."}},
    })}};
}

Json message_item(const std::string& text, const std::string& id = "msg_1") {
    return {{"id", id}, {"type", "message"}, {"role", "assistant"},
            {"status", "completed"}, {"phase", "commentary"},
            {"content", Json::array({Json{{"type", "output_text"}, {"text", text},
                                         {"annotations", Json::array()}}})}};
}

Json reasoning_item(const std::string& text = "plan") {
    return {{"id", "rs_1"}, {"type", "reasoning"},
            {"summary", Json::array({Json{{"type", "summary_text"}, {"text", text}}})},
            {"encrypted_content", "complete-cipher"}};
}

Json function_item(const std::string& id = "call_1", const std::string& arguments = R"({"path":"a.cpp"})") {
    return {{"id", "fc_" + id}, {"type", "function_call"}, {"status", "completed"},
            {"call_id", id}, {"name", "read_file"}, {"arguments", arguments}};
}

Json chat_function(const Json& item) {
    return {{"id", item["call_id"]}, {"type", "function"},
            {"function", {{"name", item["name"]}, {"arguments", item["arguments"]}}}};
}

Json envelope(Json output, const std::string& status = "completed") {
    return {{"id", "resp_1"}, {"status", status}, {"output", std::move(output)}};
}

Json terminal(Json output, const std::string& status = "completed") {
    return {{"type", "response." + status}, {"response", envelope(std::move(output), status)}};
}

const StreamEvent* first_event(const std::vector<StreamEvent>& events, StreamEventType type) {
    const auto found = std::find_if(events.begin(), events.end(), [type](const StreamEvent& event) {
        return event.type == type;
    });
    return found == events.end() ? nullptr : &*found;
}

std::size_t count_events(const std::vector<StreamEvent>& events, StreamEventType type) {
    return static_cast<std::size_t>(std::count_if(events.begin(), events.end(),
        [type](const StreamEvent& event) { return event.type == type; }));
}

} // namespace

TEST(OpenAiResponsesTest, ConvertsStatelessImagesToolDefinitionsAndResults) {
    auto chat = chat_body();
    chat["stream"] = true;
    chat["stream_options"] = {{"include_usage", true}};
    chat["max_tokens"] = 4096;
    chat["reasoning_effort"] = "high";
    chat["prompt_cache_key"] = "session-cache";
    chat["tool_choice"] = {{"type", "function"}, {"function", {{"name", "read_file"}}}};
    chat["messages"][1]["content"] = Json::array({
        Json{{"type", "text"}, {"text", "Inspect"}},
        Json{{"type", "image_url"}, {"image_url", {{"url", "data:image/png;base64,AAAA"}, {"detail", "high"}}}},
    });
    chat["messages"].push_back({{"role", "assistant"}, {"content", nullptr},
        {"tool_calls", Json::array({chat_function(function_item())})}});
    chat["messages"].push_back({{"role", "tool"}, {"tool_call_id", "call_1"}, {"content", "source"}});
    chat["tools"] = Json::array({Json{{"type", "function"}, {"function", {
        {"name", "read_file"}, {"description", "Read a file"},
        {"parameters", {{"type", "object"}, {"properties", {{"path", {{"type", "string"}}}}}}},
    }}}});
    std::string error;
    const auto request = acecode::build_openai_responses_request(chat, nullptr, &error);
    ASSERT_TRUE(error.empty()) << error;
    EXPECT_EQ(request["store"], false);
    EXPECT_EQ(request["include"], Json::array({"reasoning.encrypted_content"}));
    EXPECT_EQ(request["max_output_tokens"], 4096);
    EXPECT_EQ(request["reasoning"]["effort"], "high");
    EXPECT_EQ(request["prompt_cache_key"], "session-cache");
    EXPECT_EQ(request["tool_choice"], (Json{{"type", "function"}, {"name", "read_file"}}));
    EXPECT_FALSE(request.contains("messages"));
    EXPECT_FALSE(request.contains("stream_options"));
    EXPECT_FALSE(request.contains("max_tokens"));
    EXPECT_FALSE(request.contains("reasoning_effort"));
    ASSERT_EQ(request["input"].size(), 4u) << request.dump(2);
    EXPECT_EQ(request["input"][1]["content"][0]["type"], "input_text");
    EXPECT_EQ(request["input"][1]["content"][1]["type"], "input_image");
    EXPECT_EQ(request["input"][1]["content"][1]["detail"], "high");
    EXPECT_EQ(request["input"][2]["call_id"], "call_1");
    EXPECT_EQ(request["input"][3]["type"], "function_call_output");
    EXPECT_EQ(request["input"][3]["output"], "source");
    EXPECT_EQ(request["tools"][0]["strict"], false);
    EXPECT_FALSE(request["tools"][0]["parameters"].contains("required"));
}

TEST(OpenAiResponsesTest, ReplaysOrderedNativeGroupWithoutDuplicatingTextOrCalls) {
    const auto output = Json::array({reasoning_item(), message_item("I will inspect."), function_item()});
    const auto parsed = acecode::parse_openai_responses_response(envelope(output));
    ASSERT_FALSE(parsed.provider_error.has_error());
    ChatMessage original;
    original.role = "assistant";
    original.content = parsed.content;
    original.content_parts = parsed.content_parts;
    original.tool_calls = Json::array({chat_function(function_item())});
    std::vector<ChatMessage> originals{original};
    auto chat = chat_body();
    chat["messages"].push_back({{"role", "assistant"}, {"content", original.content},
                                {"tool_calls", original.tool_calls}});
    const auto request = acecode::build_openai_responses_request(chat, &originals);
    ASSERT_EQ(request["input"].size(), 5u) << request.dump(2);
    for (std::size_t i = 0; i < output.size(); ++i) EXPECT_EQ(request["input"][i + 2], output[i]);
    EXPECT_EQ(request["input"][3]["phase"], "commentary");
    EXPECT_EQ(request.dump().find("openai_responses_item"), std::string::npos);
}

TEST(OpenAiResponsesTest, LimitsAutomaticAndRequiredChoicesToOfferedClientFunctions) {
    for (const std::string mode : {"", "auto", "required"}) {
        auto chat = chat_body();
        chat["tools"] = Json::array({
            Json{{"type", "function"}, {"function", {{"name", "image_generate"}, {"parameters", Json::object()}}}},
            Json{{"type", "function"}, {"function", {{"name", "read"}, {"parameters", Json::object()}}}},
        });
        if (!mode.empty()) chat["tool_choice"] = mode;
        const auto request = acecode::build_openai_responses_request(chat);
        ASSERT_TRUE(request.contains("tool_choice"));
        EXPECT_EQ(request["tool_choice"], (Json{{"type", "allowed_tools"},
            {"mode", mode.empty() ? "auto" : mode}, {"tools", Json::array({
                Json{{"type", "function"}, {"name", "image_generate"}},
                Json{{"type", "function"}, {"name", "read"}},
            })}}));
    }
}

TEST(OpenAiResponsesTest, DisablesToolFreeCallsAndPreservesExplicitNone) {
    auto chat = chat_body();
    for (bool include_empty_tools : {false, true}) {
        if (include_empty_tools) chat["tools"] = Json::array();
        const auto request = acecode::build_openai_responses_request(chat);
        ASSERT_TRUE(request.contains("tool_choice"));
        EXPECT_EQ(request["tool_choice"], "none");
    }
    chat["tools"] = Json::array({Json{{"type", "function"}, {"function", {{"name", "read"}}}}});
    chat["tool_choice"] = "none";
    EXPECT_EQ(acecode::build_openai_responses_request(chat)["tool_choice"], "none");
}

TEST(OpenAiResponsesTest, RejectsChoicesOutsideOfferedToolSet) {
    auto chat = chat_body();
    chat["tool_choice"] = "required";
    EXPECT_TRUE(acecode::build_openai_responses_request(chat).is_null());
    chat["tool_choice"] = "unexpected";
    EXPECT_TRUE(acecode::build_openai_responses_request(chat).is_null());
    chat["tool_choice"] = {{"type", "function"}, {"function", {{"name", "missing"}}}};
    EXPECT_TRUE(acecode::build_openai_responses_request(chat).is_null());
}

TEST(OpenAiResponsesTest, UnsupportedOutputCannotSilentlyBecomeTextOnlySuccess) {
    for (const auto* type : {"image_generation_call", "web_search_call", "custom_tool_call", "future_output", "rate limit exceeded"}) {
        const Json unsupported{{"id", "hosted_1"}, {"type", type}, {"status", "completed"},
                               {"result", "PRIVATE_PAYLOAD" + std::string(100000, 'A')}};
        const auto wire = envelope(Json::array({function_item(), unsupported, message_item("Generated.")}));
        const auto response = acecode::parse_openai_responses_response(wire);
        EXPECT_EQ(response.finish_reason, "error") << type;
        EXPECT_TRUE(response.tool_calls.empty());
        EXPECT_TRUE(response.content_parts.empty());
        EXPECT_FALSE(response.provider_error.retryable);
        EXPECT_NE(response.provider_error.display_message.find(type), std::string::npos);
        EXPECT_EQ(response.provider_error.raw_body.find("PRIVATE_PAYLOAD"), std::string::npos);
        EXPECT_LT(response.provider_error.raw_body.size(), 2048u);

        OpenAiResponsesStreamParser parser;
        parser.consume({{"type", "response.output_item.done"}, {"output_index", 0}, {"item", function_item()}});
        parser.consume({{"type", "response.output_item.done"}, {"output_index", 1}, {"item", unsupported}});
        const auto events = parser.consume({{"type", "response.completed"}, {"response", wire}});
        const auto* failure = first_event(events, StreamEventType::Error);
        ASSERT_NE(failure, nullptr) << type;
        EXPECT_EQ(first_event(events, StreamEventType::Done), nullptr);
        EXPECT_EQ(first_event(events, StreamEventType::ToolCall), nullptr);
        EXPECT_TRUE(parser.accumulated().content_parts.empty());
        EXPECT_FALSE(failure->provider_error.retryable);
        EXPECT_EQ(failure->provider_error.raw_body.find("PRIVATE_PAYLOAD"), std::string::npos);
        EXPECT_LT(failure->provider_error.raw_body.size(), 2048u);
    }
}

TEST(OpenAiResponsesTest, KeepsMessagePhaseWhenOmittingNonReplayableReasoning) {
    auto reasoning = reasoning_item();
    reasoning.erase("encrypted_content");
    const auto message = message_item("Working.");
    const auto parsed = acecode::parse_openai_responses_response(envelope(
        Json::array({reasoning, message, function_item()})));
    ChatMessage original;
    original.role = "assistant";
    original.content = parsed.content;
    original.content_parts = parsed.content_parts;
    original.tool_calls = Json::array({chat_function(function_item())});
    std::vector<ChatMessage> originals{original};
    auto chat = chat_body();
    chat["messages"].push_back({{"role", "assistant"}, {"content", original.content},
                                {"tool_calls", original.tool_calls}});
    const auto request = acecode::build_openai_responses_request(chat, &originals);
    ASSERT_EQ(request["input"].size(), 4u);
    EXPECT_EQ(request["input"][2], message);
    EXPECT_EQ(request["input"][3], function_item());
    EXPECT_EQ(request.dump().find("rs_1"), std::string::npos);
}

TEST(OpenAiResponsesTest, MixedReasoningChannelsDoNotInvalidateToolCompletion) {
    for (bool raw_first : {false, true}) {
        OpenAiResponsesStreamParser parser;
        const auto selected_type = raw_first ? "response.reasoning_text.delta" : "response.reasoning_summary_text.delta";
        const auto other_type = raw_first ? "response.reasoning_summary_text.delta" : "response.reasoning_text.delta";
        auto events = parser.consume({{"type", selected_type}, {"output_index", 0}, {"delta", "selected"}});
        ASSERT_NE(first_event(events, StreamEventType::ReasoningDelta), nullptr);
        events = parser.consume({{"type", other_type}, {"output_index", 0}, {"delta", "other"}});
        EXPECT_EQ(first_event(events, StreamEventType::ReasoningDelta), nullptr);
        auto reasoning = reasoning_item(raw_first ? "other" : "selected complete");
        reasoning["content"] = Json::array({Json{{"type", "reasoning_text"},
            {"text", raw_first ? "selected complete" : "other"}}});
        events = parser.consume(terminal(Json::array({reasoning, function_item()})));
        EXPECT_EQ(first_event(events, StreamEventType::Error), nullptr);
        EXPECT_EQ(count_events(events, StreamEventType::ToolCall), 1u);
        ASSERT_NE(first_event(events, StreamEventType::ReasoningDelta), nullptr);
        EXPECT_EQ(first_event(events, StreamEventType::ReasoningDelta)->content, " complete");
        EXPECT_EQ(parser.accumulated().reasoning_content, "selected complete");
    }
}

TEST(OpenAiResponsesTest, RejectsFunctionIdentityChangesBeforeItemCompletion) {
    for (const auto* field : {"name", "call_id"}) {
        for (bool via_done : {false, true}) {
            OpenAiResponsesStreamParser parser;
            auto added = function_item();
            added["status"] = "in_progress";
            added["arguments"] = "";
            parser.consume({{"type", "response.output_item.added"}, {"output_index", 0}, {"item", added}});
            auto changed = function_item();
            changed[field] = "changed_identity";
            const auto events = via_done
                ? parser.consume({{"type", "response.output_item.done"}, {"output_index", 0}, {"item", changed}})
                : parser.consume(terminal(Json::array({changed})));
            EXPECT_NE(first_event(events, StreamEventType::Error), nullptr) << field;
            EXPECT_EQ(first_event(events, StreamEventType::ToolCall), nullptr);
            EXPECT_TRUE(parser.consume(terminal(Json::array({changed}))).empty());
        }
    }
}

TEST(OpenAiResponsesTest, ValidatesHiddenReasoningChannelAndSelectsPerItem) {
    OpenAiResponsesStreamParser changed;
    changed.consume({{"type", "response.reasoning_summary_text.delta"}, {"output_index", 0}, {"delta", "summary"}});
    changed.consume({{"type", "response.reasoning_text.delta"}, {"output_index", 0}, {"delta", "raw"}});
    auto reasoning = reasoning_item("summary");
    reasoning["content"] = Json::array({Json{{"type", "reasoning_text"}, {"text", "changed"}}});
    auto events = changed.consume(terminal(Json::array({reasoning, function_item()})));
    EXPECT_NE(first_event(events, StreamEventType::Error), nullptr);
    EXPECT_EQ(first_event(events, StreamEventType::ToolCall), nullptr);

    OpenAiResponsesStreamParser separate;
    separate.consume({{"type", "response.reasoning_summary_text.delta"}, {"output_index", 0}, {"delta", "summary"}});
    separate.consume({{"type", "response.reasoning_text.delta"}, {"output_index", 1}, {"delta", "raw"}});
    auto raw = reasoning_item();
    raw["id"] = "rs_2";
    raw["content"] = Json::array({Json{{"type", "reasoning_text"}, {"text", "raw"}}});
    events = separate.consume(terminal(Json::array({reasoning_item("summary"), raw, function_item()})));
    EXPECT_NE(first_event(events, StreamEventType::Done), nullptr);
    EXPECT_EQ(separate.accumulated().reasoning_content, "summaryraw");
}

TEST(OpenAiResponsesTest, FunctionIdentityCanArriveLateButCannotBeClearedThenReplaced) {
    const Json preliminary{{"id", "fc_call_1"}, {"type", "function_call"}, {"status", "in_progress"}};
    OpenAiResponsesStreamParser late;
    late.consume({{"type", "response.output_item.added"}, {"output_index", 0}, {"item", preliminary}});
    late.consume({{"type", "response.output_item.done"}, {"output_index", 0}, {"item", function_item()}});
    EXPECT_EQ(count_events(late.consume(terminal(Json::array({function_item()}))), StreamEventType::ToolCall), 1u);

    OpenAiResponsesStreamParser changed;
    auto known = function_item();
    known["status"] = "in_progress";
    changed.consume({{"type", "response.output_item.added"}, {"output_index", 0}, {"item", known}});
    changed.consume({{"type", "response.output_item.added"}, {"output_index", 0}, {"item", preliminary}});
    auto renamed = function_item();
    renamed["name"] = "different";
    const auto events = changed.consume({{"type", "response.output_item.done"}, {"output_index", 0}, {"item", renamed}});
    EXPECT_NE(first_event(events, StreamEventType::Error), nullptr);
}

TEST(OpenAiResponsesTest, UnsupportedOutputDiagnosticHandlesLongUtf8Type) {
    const Json unsupported{{"type", std::string(79, 'A') + "\xc3\xa9"}, {"result", "PRIVATE_PAYLOAD"}};
    const auto wire = envelope(Json::array({unsupported}));
    acecode::ChatResponse response;
    ASSERT_NO_THROW(response = acecode::parse_openai_responses_response(wire));
    EXPECT_EQ(response.finish_reason, "error");
    EXPECT_FALSE(response.provider_error.retryable);
    EXPECT_EQ(response.provider_error.raw_body.find("PRIVATE_PAYLOAD"), std::string::npos);
    OpenAiResponsesStreamParser parser;
    std::vector<StreamEvent> events;
    ASSERT_NO_THROW(events = parser.consume({{"type", "response.completed"}, {"response", wire}}));
    ASSERT_NE(first_event(events, StreamEventType::Error), nullptr);
    EXPECT_FALSE(first_event(events, StreamEventType::Error)->provider_error.retryable);
}

TEST(OpenAiResponsesTest, ReasoningDeltasMustMatchTheirOwnTerminalItemType) {
    OpenAiResponsesStreamParser parser;
    parser.consume({{"type", "response.reasoning_summary_text.delta"}, {"output_index", 0}, {"delta", "selected"}});
    parser.consume({{"type", "response.reasoning_text.delta"}, {"output_index", 0}, {"delta", "other"}});
    const auto events = parser.consume(terminal(Json::array({function_item(), reasoning_item("selected")})));
    EXPECT_NE(first_event(events, StreamEventType::Error), nullptr);
    EXPECT_EQ(first_event(events, StreamEventType::ToolCall), nullptr);
}

TEST(OpenAiResponsesTest, StaleNativeCallsCannotUndoCanonicalHistoryRepair) {
    const auto parsed = acecode::parse_openai_responses_response(envelope(
        Json::array({reasoning_item(), message_item("answer"), function_item()})));
    ChatMessage original;
    original.role = "assistant";
    original.content = "answer";
    original.content_parts = parsed.content_parts;
    original.tool_calls = Json::array({chat_function(function_item())});
    std::vector<ChatMessage> originals{original};
    auto chat = chat_body();
    chat["messages"].push_back({{"role", "assistant"}, {"content", "answer"}});
    auto request = acecode::build_openai_responses_request(chat, &originals);
    ASSERT_EQ(request["input"].size(), 3u);
    EXPECT_EQ(request["input"][2]["content"], "answer");
    EXPECT_EQ(request.dump().find("complete-cipher"), std::string::npos);
    EXPECT_EQ(request.dump().find("function_call"), std::string::npos);

    chat["messages"][2]["content"] = "edited answer";
    chat["messages"][2]["tool_calls"] = original.tool_calls;
    request = acecode::build_openai_responses_request(chat, &originals);
    ASSERT_EQ(request["input"].size(), 4u);
    EXPECT_EQ(request["input"][2]["content"], "edited answer");
    EXPECT_EQ(request["input"][3]["type"], "function_call");
    EXPECT_EQ(request.dump().find("complete-cipher"), std::string::npos);
}

TEST(OpenAiResponsesTest, MalformedNativeGroupFallsBackAtomicallyAndDroppedRowsDoNotShiftReplay) {
    ChatMessage original;
    original.role = "assistant";
    original.content = "answer";
    original.content_parts = Json::array({
        Json{{"type", "openai_responses_item"}, {"item", reasoning_item()}},
        Json{{"type", "openai_responses_item"}, {"item", "broken"}},
    });
    std::vector<ChatMessage> originals{original};
    auto chat = chat_body();
    chat["messages"].push_back({{"role", "assistant"}, {"content", "answer"}});
    auto request = acecode::build_openai_responses_request(chat, &originals);
    ASSERT_EQ(request["input"].size(), 3u);
    EXPECT_EQ(request.dump().find("cipher"), std::string::npos);
    original.content_parts = acecode::parse_openai_responses_response(envelope(
        Json::array({reasoning_item(), message_item("answer")}))).content_parts;
    originals = {original, original};
    request = acecode::build_openai_responses_request(chat, &originals);
    EXPECT_EQ(request.dump().find("cipher"), std::string::npos);
}

TEST(OpenAiResponsesTest, RejectsInvalidRequestsWithoutRequiringErrorPointer) {
    auto chat = chat_body();
    chat["messages"][1]["content"] = Json::array({Json{{"type", "image_url"}, {"image_url", Json::object()}}});
    EXPECT_TRUE(acecode::build_openai_responses_request(chat).is_null());
    std::string error;
    EXPECT_TRUE(acecode::build_openai_responses_request(chat, nullptr, &error).is_null());
    EXPECT_FALSE(error.empty());
    chat = chat_body();
    chat["messages"].push_back({{"role", "tool"}, {"content", "missing ID"}});
    EXPECT_TRUE(acecode::build_openai_responses_request(chat).is_null());
    chat = chat_body();
    chat["tools"] = Json::array({Json{{"type", "web_search"}}});
    EXPECT_TRUE(acecode::build_openai_responses_request(chat).is_null());
}

TEST(OpenAiResponsesTest, ParsesTextRefusalReasoningParallelCallsAndUsage) {
    auto message = message_item("Answer. ");
    message["content"].push_back({{"type", "refusal"}, {"refusal", "Cannot do that."}});
    auto wire = envelope(Json::array({reasoning_item(), message, function_item(), function_item("call_2", "{}")}));
    wire["usage"] = {{"input_tokens", 100}, {"output_tokens", 25}, {"total_tokens", 125},
                      {"input_tokens_details", {{"cached_tokens", 40}}},
                      {"output_tokens_details", {{"reasoning_tokens", 10}}}};
    const auto result = acecode::parse_openai_responses_response(wire);
    EXPECT_EQ(result.content, "Answer. Cannot do that.");
    EXPECT_EQ(result.reasoning_content, "plan");
    EXPECT_EQ(result.finish_reason, "tool_calls");
    ASSERT_EQ(result.tool_calls.size(), 2u);
    EXPECT_EQ(result.tool_calls[1].id, "call_2");
    ASSERT_EQ(result.content_parts.size(), 4u);
    EXPECT_EQ(result.content_parts[0]["item"]["encrypted_content"], "complete-cipher");
    EXPECT_EQ(result.content_parts[1]["item"]["phase"], "commentary");
    EXPECT_TRUE(result.usage.has_data);
    EXPECT_EQ(result.usage.prompt_tokens, 100);
    EXPECT_EQ(result.usage.cache_read_tokens, 40);
    EXPECT_EQ(result.usage.reasoning_tokens, 10);
}

TEST(OpenAiResponsesTest, IncompleteResponsesNeverExposeExecutableOrReplayableCalls) {
    for (const auto& reason : {"max_output_tokens", "content_filter"}) {
        auto wire = envelope(Json::array({message_item("partial"),
            Json{{"type", "function_call"}, {"arguments", "{"}}}), "incomplete");
        wire["incomplete_details"] = {{"reason", reason}};
        wire["usage"] = {{"input_tokens", 0}, {"output_tokens", 3}, {"total_tokens", 3}};
        auto result = acecode::parse_openai_responses_response(wire);
        EXPECT_EQ(result.finish_reason, std::string(reason) == "content_filter" ? "content_filter" : "length");
        EXPECT_TRUE(result.tool_calls.empty());
        ASSERT_EQ(result.content_parts.size(), 1u);
        EXPECT_EQ(result.content, "partial");
        EXPECT_EQ(result.usage.completion_tokens, 3);
        OpenAiResponsesStreamParser parser;
        const auto events = parser.consume({{"type", "response.incomplete"}, {"response", wire}});
        EXPECT_EQ(first_event(events, StreamEventType::ToolCall), nullptr);
        EXPECT_NE(first_event(events, StreamEventType::Done), nullptr);
    }
}

TEST(OpenAiResponsesTest, ValidatesEnvelopesAndNeverSynthesizesMissingCallIds) {
    for (const auto& wire : {
             Json::object(), envelope(Json::object()),
             envelope(Json::array({Json{{"type", "message"}, {"content", 42}}})),
             envelope(Json::array({Json{{"type", "function_call"}, {"name", "shell"}, {"arguments", "{}"}}})),
             envelope(Json::array({function_item(), function_item()})),
             envelope(Json::array({function_item("call_1", "{")})),
         }) {
        const auto result = acecode::parse_openai_responses_response(wire);
        EXPECT_EQ(result.finish_reason, "error") << wire.dump();
        EXPECT_EQ(result.provider_error.kind, acecode::ProviderErrorKind::MalformedJson);
        EXPECT_TRUE(result.tool_calls.empty());
    }
}

TEST(OpenAiResponsesTest, StructuredErrorsPreserveQuotaClassificationAndDiagnosticBody) {
    for (const auto& code : {"rate_limit_exceeded", "server_error", "insufficient_quota", "invalid_prompt"}) {
        const Json wire{{"status", "failed"}, {"error", {{"code", code}, {"message", "upstream failed"}}}};
        const auto result = acecode::parse_openai_responses_response(wire);
        EXPECT_EQ(result.finish_reason, "error");
        EXPECT_EQ(result.provider_error.provider, "openai");
        EXPECT_EQ(result.provider_error.raw_body, wire.dump());
        EXPECT_EQ(result.provider_error.display_message, "upstream failed");
        EXPECT_EQ(result.provider_error.retryable,
                  std::string(code) == "server_error" || std::string(code) == "rate_limit_exceeded");
    }
}

TEST(OpenAiResponsesTest, StreamsToolsOnlyAfterValidatedTerminalAndNeverDuplicatesThem) {
    OpenAiResponsesStreamParser parser;
    auto added = function_item();
    added["arguments"] = "";
    added["status"] = "in_progress";
    EXPECT_TRUE(parser.consume({{"type", "response.output_item.added"}, {"output_index", 0}, {"item", added}}).empty());
    auto events = parser.consume({{"type", "response.function_call_arguments.delta"}, {"output_index", 0},
                                  {"item_id", "fc_call_1"}, {"delta", R"({"path":)"}});
    const auto* progress = first_event(events, StreamEventType::ToolCallDelta);
    ASSERT_NE(progress, nullptr);
    EXPECT_TRUE(progress->tool_call.function_arguments.empty());
    EXPECT_EQ(progress->tool_call_argument_bytes, 8u);
    EXPECT_EQ(progress->tool_call.id, "call_1");
    EXPECT_TRUE(parser.consume({{"type", "response.function_call_arguments.done"}, {"output_index", 0},
                                {"item_id", "fc_call_1"}, {"arguments", function_item()["arguments"]}}).empty());
    events = parser.consume({{"type", "response.output_item.done"}, {"output_index", 0}, {"item", function_item()}});
    EXPECT_EQ(first_event(events, StreamEventType::ToolCall), nullptr);
    EXPECT_TRUE(parser.accumulated().tool_calls.empty());
    events = parser.consume(terminal(Json::array({function_item()})));
    EXPECT_EQ(count_events(events, StreamEventType::ToolCall), 1u);
    EXPECT_EQ(count_events(events, StreamEventType::Done), 1u);
    ASSERT_EQ(parser.accumulated().tool_calls.size(), 1u);
    EXPECT_EQ(parser.accumulated().tool_calls[0].function_arguments, function_item()["arguments"]);
    EXPECT_TRUE(parser.consume(terminal(Json::array({function_item()}))).empty());
    EXPECT_TRUE(parser.finish().empty());
}

TEST(OpenAiResponsesTest, ReconcilesFinalOnlyTextAndReasoningWithoutDuplicatingStreamedPrefix) {
    OpenAiResponsesStreamParser parser;
    auto events = parser.consume({{"type", "response.output_text.delta"}, {"output_index", 1}, {"delta", "hel"}});
    ASSERT_NE(first_event(events, StreamEventType::Delta), nullptr);
    events = parser.consume({{"type", "response.reasoning_summary_text.delta"}, {"output_index", 0}, {"delta", "pl"}});
    ASSERT_NE(first_event(events, StreamEventType::ReasoningDelta), nullptr);
    events = parser.consume(terminal(Json::array({reasoning_item(), message_item("hello")})));
    ASSERT_NE(first_event(events, StreamEventType::Delta), nullptr);
    EXPECT_EQ(first_event(events, StreamEventType::Delta)->content, "lo");
    ASSERT_NE(first_event(events, StreamEventType::ReasoningDelta), nullptr);
    EXPECT_EQ(first_event(events, StreamEventType::ReasoningDelta)->content, "an");
    EXPECT_EQ(parser.accumulated().content, "hello");
    EXPECT_EQ(parser.accumulated().reasoning_content, "plan");

    OpenAiResponsesStreamParser final_only;
    events = final_only.consume(terminal(Json::array({message_item("final only")})));
    ASSERT_NE(first_event(events, StreamEventType::Delta), nullptr);
    EXPECT_EQ(first_event(events, StreamEventType::Delta)->content, "final only");
}

TEST(OpenAiResponsesTest, RetainsCompletedEncryptedItemAndPhaseInsteadOfPreliminaryCipher) {
    OpenAiResponsesStreamParser parser;
    auto reasoning = reasoning_item();
    auto preliminary = reasoning;
    preliminary["encrypted_content"] = "partial-cipher";
    parser.consume({{"type", "response.output_item.added"}, {"output_index", 0}, {"item", preliminary}});
    parser.consume({{"type", "response.output_item.done"}, {"output_index", 0}, {"item", reasoning}});
    const auto message = message_item("answer");
    parser.consume({{"type", "response.output_item.done"}, {"output_index", 1}, {"item", message}});
    auto final_reasoning = reasoning;
    final_reasoning.erase("encrypted_content");
    auto final_message = message;
    final_message.erase("phase");
    const auto events = parser.consume(terminal(Json::array({final_reasoning, final_message})));
    const auto* done = first_event(events, StreamEventType::Done);
    ASSERT_NE(done, nullptr);
    ASSERT_EQ(done->content_parts.size(), 2u);
    EXPECT_EQ(done->content_parts[0]["item"]["encrypted_content"], "complete-cipher");
    EXPECT_EQ(done->content_parts[1]["item"]["phase"], "commentary");
    EXPECT_EQ(done->content_parts.dump().find("partial-cipher"), std::string::npos);
}

TEST(OpenAiResponsesTest, TruncationAndLateFailureCannotReleaseCompletedToolItems) {
    for (bool explicit_failure : {false, true}) {
        OpenAiResponsesStreamParser parser;
        parser.consume({{"type", "response.output_item.done"}, {"output_index", 0}, {"item", function_item()}});
        const auto events = explicit_failure
            ? parser.consume({{"type", "response.failed"}, {"response", {
                {"status", "failed"}, {"error", {{"code", "insufficient_quota"}, {"message", "quota"}}}}}})
            : parser.finish();
        const auto* error = first_event(events, StreamEventType::Error);
        ASSERT_NE(error, nullptr);
        EXPECT_EQ(first_event(events, StreamEventType::ToolCall), nullptr);
        EXPECT_EQ(first_event(events, StreamEventType::Done), nullptr);
        EXPECT_EQ(error->provider_error.retryable, !explicit_failure);
        EXPECT_TRUE(parser.accumulated().tool_calls.empty());
        EXPECT_TRUE(parser.accumulated().content_parts.empty());
    }
}

TEST(OpenAiResponsesTest, IncompleteOutputItemCanPrecedeIncompleteTerminal) {
    OpenAiResponsesStreamParser parser;
    const Json partial{{"id", "fc_1"}, {"type", "function_call"},
                       {"status", "incomplete"}, {"arguments", "{"}};
    EXPECT_TRUE(parser.consume({{"type", "response.output_item.done"},
                                {"output_index", 0}, {"item", partial}}).empty());
    const auto events = parser.consume(terminal(Json::array({partial}), "incomplete"));
    const auto* done = first_event(events, StreamEventType::Done);
    ASSERT_NE(done, nullptr);
    EXPECT_EQ(done->finish_reason, "length");
    EXPECT_TRUE(done->content_parts.empty());
    EXPECT_EQ(first_event(events, StreamEventType::ToolCall), nullptr);
}

TEST(OpenAiResponsesTest, KnownMalformedEventsFailWhileUnknownEventsAreIgnored) {
    const std::vector<Json> malformed{
        Json::array(), Json{{"type", 7}},
        Json{{"type", "response.output_text.delta"}, {"output_index", 0}, {"delta", 42}},
        Json{{"type", "response.output_text.delta"}, {"output_index", -1}, {"delta", "bad"}},
        Json{{"type", "response.output_text.delta"}, {"output_index", 0}, {"item_id", 42}, {"delta", "bad"}},
        Json{{"type", "response.output_item.done"}, {"output_index", 0}},
        Json{{"type", "response.output_item.added"}, {"output_index", 0}, {"item", {{"type", "function_call"}, {"name", 42}}}},
        Json{{"type", "response.function_call_arguments.done"}, {"output_index", 0}, {"arguments", Json::object()}},
        Json{{"type", "response.content_part.done"}, {"output_index", 0}, {"part", nullptr}},
        Json{{"type", "response.content_part.done"}, {"output_index", 0}, {"part", {{"type", "output_text"}, {"text", 42}}}},
        Json{{"type", "response.completed"}},
        Json{{"type", "response.completed"}, {"response", {{"status", "completed"}}}},
    };
    for (const auto& event : malformed) {
        OpenAiResponsesStreamParser parser;
        const auto events = parser.consume(event);
        const auto* error = first_event(events, StreamEventType::Error);
        ASSERT_NE(error, nullptr) << event.dump();
        EXPECT_EQ(error->provider_error.kind, acecode::ProviderErrorKind::MalformedSse);
        EXPECT_FALSE(error->provider_error.retryable);
    }
    OpenAiResponsesStreamParser parser;
    EXPECT_TRUE(parser.consume({{"type", "response.future_event"}, {"future", Json::array()}}).empty());
    EXPECT_FALSE(parser.terminal());
}

TEST(OpenAiResponsesTest, RejectsReusedOutputIdsAndContradictoryTerminalPayloads) {
    OpenAiResponsesStreamParser reused;
    reused.consume({{"type", "response.output_item.added"}, {"output_index", 0}, {"item", function_item()}});
    auto events = reused.consume({{"type", "response.function_call_arguments.delta"},
                                  {"output_index", 1}, {"item_id", "fc_call_1"}, {"delta", "{"}});
    EXPECT_NE(first_event(events, StreamEventType::Error), nullptr);

    OpenAiResponsesStreamParser changed;
    changed.consume({{"type", "response.output_text.delta"}, {"output_index", 0}, {"delta", "expected"}});
    events = changed.consume(terminal(Json::array({message_item("different"), function_item()})));
    EXPECT_NE(first_event(events, StreamEventType::Error), nullptr);
    EXPECT_EQ(first_event(events, StreamEventType::ToolCall), nullptr);

    OpenAiResponsesStreamParser missing;
    missing.consume({{"type", "response.output_item.done"}, {"output_index", 0}, {"item", function_item()}});
    events = missing.consume(terminal(Json::array()));
    EXPECT_NE(first_event(events, StreamEventType::Error), nullptr);
}

TEST(OpenAiResponsesTest, UsageCountsSaturateWithoutOverflowAndZeroUsageIsRetained) {
    auto wire = envelope(Json::array());
    wire["usage"] = {{"input_tokens", (std::numeric_limits<std::uint64_t>::max)()},
                      {"output_tokens", (std::numeric_limits<std::uint64_t>::max)()}};
    auto result = acecode::parse_openai_responses_response(wire);
    EXPECT_EQ(result.usage.prompt_tokens, (std::numeric_limits<int>::max)());
    EXPECT_EQ(result.usage.total_tokens, (std::numeric_limits<int>::max)());
    wire["usage"] = {{"input_tokens", 0}, {"output_tokens", 0}, {"total_tokens", 0}};
    result = acecode::parse_openai_responses_response(wire);
    EXPECT_TRUE(result.usage.has_data);
    EXPECT_EQ(result.usage.total_tokens, 0);
}
