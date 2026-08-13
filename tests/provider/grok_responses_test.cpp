#include "provider/grok_responses.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using acecode::ChatMessage;
using acecode::GrokResponsesStreamParser;
using acecode::StreamEvent;
using acecode::StreamEventType;

nlohmann::json base_chat_body(bool stream = false) {
    return {
        {"model", "grok-4.5"},
        {"stream", stream},
        {"messages", nlohmann::json::array({
            nlohmann::json{{"role", "system"}, {"content", "Be precise."}},
            nlohmann::json{{"role", "user"}, {"content", "Hello"}},
        })},
    };
}

const StreamEvent* first_event(const std::vector<StreamEvent>& events,
                               StreamEventType type) {
    for (const auto& event : events) {
        if (event.type == type) return &event;
    }
    return nullptr;
}

} // namespace

TEST(GrokResponsesTest, ConvertsTextImagesToolsAndToolResults) {
    auto chat = base_chat_body();
    chat["messages"][1]["content"] = nlohmann::json::array({
        nlohmann::json{{"type", "text"}, {"text", "Inspect"}},
        nlohmann::json{
            {"type", "image_url"},
            {"image_url", nlohmann::json{
                {"url", "data:image/png;base64,AAAA"},
                {"detail", "high"},
            }},
        },
    });
    chat["messages"].push_back(nlohmann::json{
        {"role", "assistant"},
        {"content", nullptr},
        {"tool_calls", nlohmann::json::array({
            nlohmann::json{
                {"id", "call_1"},
                {"type", "function"},
                {"function", nlohmann::json{
                    {"name", "file_read"},
                    {"arguments", R"({"path":"a.cpp"})"},
                }},
            },
        })},
    });
    chat["messages"].push_back(nlohmann::json{
        {"role", "tool"},
        {"tool_call_id", "call_1"},
        {"content", "source"},
    });
    chat["tools"] = nlohmann::json::array({
        nlohmann::json{
            {"type", "function"},
            {"function", nlohmann::json{
                {"name", "file_read"},
                {"description", "Read a file"},
                {"parameters", nlohmann::json{
                    {"type", "object"},
                    {"properties", nlohmann::json::object()},
                }},
            }},
        },
    });
    chat["max_tokens"] = 2048;
    chat["stream_options"] = {{"include_usage", true}};

    std::string error;
    const auto request = acecode::build_grok_responses_request(chat, nullptr, &error);
    ASSERT_TRUE(error.empty()) << error;
    EXPECT_EQ(request["model"], "grok-4.5");
    EXPECT_EQ(request["store"], false);
    EXPECT_EQ(request["max_output_tokens"], 2048);
    EXPECT_FALSE(request.contains("stream_options"));
    ASSERT_EQ(request["include"].size(), 1u);
    EXPECT_EQ(request["include"][0], "reasoning.encrypted_content");

    const auto& input = request["input"];
    ASSERT_EQ(input.size(), 4u) << input.dump(2);
    EXPECT_EQ(input[1]["type"], "message");
    EXPECT_EQ(input[1]["content"][0]["type"], "input_text");
    EXPECT_EQ(input[1]["content"][1]["type"], "input_image");
    EXPECT_EQ(input[1]["content"][1]["detail"], "high");
    EXPECT_EQ(input[2]["type"], "function_call");
    EXPECT_EQ(input[2]["call_id"], "call_1");
    EXPECT_EQ(input[3]["type"], "function_call_output");
    EXPECT_EQ(input[3]["call_id"], "call_1");
    ASSERT_EQ(request["tools"].size(), 1u);
    EXPECT_EQ(request["tools"][0]["type"], "function");
    EXPECT_EQ(request["tools"][0]["name"], "file_read");
    EXPECT_FALSE(request["tools"][0].contains("function"));
}

TEST(GrokResponsesTest, ReplaysOnlyEncryptedGrokReasoningBeforeAssistantOutput) {
    auto chat = base_chat_body();
    chat["messages"].push_back(nlohmann::json{
        {"role", "assistant"}, {"content", "Earlier answer"}});
    chat["messages"].push_back(nlohmann::json{
        {"role", "user"}, {"content", "Continue"}});

    ChatMessage system;
    system.role = "system";
    ChatMessage user;
    user.role = "user";
    ChatMessage assistant;
    assistant.role = "assistant";
    assistant.reasoning_content = "plain thought must not be replayed";
    assistant.content_parts = nlohmann::json::array({
        nlohmann::json{
            {"type", "grok_reasoning"},
            {"summary", nlohmann::json::array()},
            {"encrypted_content", "opaque-signature"},
        },
        nlohmann::json{{"type", "text"}, {"text", "ignored"}},
    });
    ChatMessage final_user;
    final_user.role = "user";

    std::vector<ChatMessage> original{
        system, user, assistant, final_user};
    std::string error;
    const auto request = acecode::build_grok_responses_request(
        chat, &original, &error);
    ASSERT_TRUE(error.empty()) << error;
    ASSERT_EQ(request["input"].size(), 5u) << request.dump(2);
    EXPECT_EQ(request["input"][2]["type"], "reasoning");
    EXPECT_EQ(request["input"][2]["encrypted_content"], "opaque-signature");
    EXPECT_TRUE(request["input"][2]["summary"].empty());
    EXPECT_EQ(request.dump().find("plain thought"), std::string::npos);
}

TEST(GrokResponsesTest, ParsesNonStreamingEnvelopeAndUsage) {
    const nlohmann::json envelope = {
        {"id", "resp_1"},
        {"status", "completed"},
        {"output", nlohmann::json::array({
            nlohmann::json{
                {"type", "reasoning"},
                {"summary", nlohmann::json::array({
                    nlohmann::json{{"type", "summary_text"}, {"text", "plan"}},
                })},
                {"encrypted_content", "cipher"},
            },
            nlohmann::json{
                {"type", "message"},
                {"role", "assistant"},
                {"content", nlohmann::json::array({
                    nlohmann::json{{"type", "output_text"}, {"text", "done"}},
                })},
            },
            nlohmann::json{
                {"type", "function_call"},
                {"call_id", "call_1"},
                {"name", "file_read"},
                {"arguments", R"({"path":"a.cpp"})"},
            },
        })},
        {"usage", nlohmann::json{
            {"input_tokens", 100},
            {"output_tokens", 25},
            {"total_tokens", 125},
            {"input_tokens_details", nlohmann::json{{"cached_tokens", 40}}},
            {"output_tokens_details", nlohmann::json{{"reasoning_tokens", 10}}},
        }},
    };

    const auto response = acecode::parse_grok_responses_response(envelope);
    EXPECT_EQ(response.content, "done");
    EXPECT_EQ(response.reasoning_content, "plan");
    EXPECT_EQ(response.finish_reason, "tool_calls");
    ASSERT_EQ(response.tool_calls.size(), 1u);
    EXPECT_EQ(response.tool_calls[0].id, "call_1");
    EXPECT_EQ(response.tool_calls[0].function_name, "file_read");
    ASSERT_EQ(response.content_parts.size(), 1u);
    EXPECT_EQ(response.content_parts[0]["type"], "grok_reasoning");
    EXPECT_EQ(response.content_parts[0]["encrypted_content"], "cipher");
    EXPECT_EQ(response.usage.prompt_tokens, 100);
    EXPECT_EQ(response.usage.completion_tokens, 25);
    EXPECT_EQ(response.usage.cache_read_tokens, 40);
    EXPECT_EQ(response.usage.reasoning_tokens, 10);
    EXPECT_TRUE(response.usage.has_data);
}

TEST(GrokResponsesTest, StreamsTextReasoningToolAndTerminalEnvelopeOnce) {
    GrokResponsesStreamParser parser;
    auto events = parser.consume({
        {"type", "response.output_text.delta"}, {"delta", "hel"}});
    ASSERT_NE(first_event(events, StreamEventType::Delta), nullptr);
    EXPECT_EQ(first_event(events, StreamEventType::Delta)->content, "hel");

    events = parser.consume({
        {"type", "response.reasoning_summary_text.delta"}, {"delta", "think"}});
    ASSERT_NE(first_event(events, StreamEventType::ReasoningDelta), nullptr);

    parser.consume({
        {"type", "response.output_item.added"},
        {"output_index", 1},
        {"item", nlohmann::json{
            {"id", "fc_1"}, {"type", "function_call"},
            {"call_id", "call_1"}, {"name", "file_read"},
        }},
    });
    events = parser.consume({
        {"type", "response.function_call_arguments.delta"},
        {"output_index", 1}, {"item_id", "fc_1"},
        {"delta", R"({"path":)"},
    });
    ASSERT_NE(first_event(events, StreamEventType::ToolCallDelta), nullptr);
    events = parser.consume({
        {"type", "response.output_item.done"},
        {"output_index", 1},
        {"item", nlohmann::json{
            {"id", "fc_1"}, {"type", "function_call"},
            {"call_id", "call_1"}, {"name", "file_read"},
            {"arguments", R"({"path":"a.cpp"})"},
        }},
    });
    ASSERT_NE(first_event(events, StreamEventType::ToolCall), nullptr);
    EXPECT_EQ(first_event(events, StreamEventType::ToolCall)
                  ->tool_call.function_arguments,
              R"({"path":"a.cpp"})");

    parser.consume({
        {"type", "response.output_item.done"},
        {"output_index", 0},
        {"item", nlohmann::json{
            {"id", "rs_1"}, {"type", "reasoning"},
            {"summary", nlohmann::json::array()},
            {"encrypted_content", "stream-cipher"},
        }},
    });
    events = parser.consume({
        {"type", "response.completed"},
        {"response", nlohmann::json{
            {"status", "completed"},
            {"output", nlohmann::json::array({
                nlohmann::json{
                    {"type", "function_call"}, {"call_id", "call_1"},
                    {"name", "file_read"},
                    {"arguments", R"({"path":"a.cpp"})"},
                },
            })},
            {"usage", nlohmann::json{
                {"input_tokens", 10}, {"output_tokens", 3},
                {"total_tokens", 13},
            }},
        }},
    });
    EXPECT_NE(first_event(events, StreamEventType::Usage), nullptr);
    const auto* done = first_event(events, StreamEventType::Done);
    ASSERT_NE(done, nullptr);
    EXPECT_EQ(done->finish_reason, "tool_calls");
    ASSERT_EQ(done->content_parts.size(), 1u);
    EXPECT_EQ(done->content_parts[0]["encrypted_content"], "stream-cipher");
    EXPECT_TRUE(parser.terminal());
    EXPECT_TRUE(parser.consume({{"type", "response.completed"}}).empty());
    EXPECT_TRUE(parser.finish().empty());
    ASSERT_EQ(parser.accumulated().tool_calls.size(), 1u);
}

TEST(GrokResponsesTest, UsesFinalEnvelopeArgumentsWhenDeltasAreMissing) {
    GrokResponsesStreamParser parser;
    const auto events = parser.consume({
        {"type", "response.completed"},
        {"response", nlohmann::json{
            {"status", "completed"},
            {"output", nlohmann::json::array({
                nlohmann::json{
                    {"id", "fc_final"}, {"type", "function_call"},
                    {"call_id", "call_final"}, {"name", "shell"},
                    {"arguments", R"({"command":"git status"})"},
                },
            })},
        }},
    });
    const auto* tool = first_event(events, StreamEventType::ToolCall);
    ASSERT_NE(tool, nullptr);
    EXPECT_EQ(tool->tool_call.id, "call_final");
    EXPECT_EQ(tool->tool_call.function_arguments,
              R"({"command":"git status"})");
    ASSERT_NE(first_event(events, StreamEventType::Done), nullptr);
}

TEST(GrokResponsesTest, IgnoresUnknownEventsAndReportsFailuresOrTruncation) {
    GrokResponsesStreamParser unknown;
    EXPECT_TRUE(unknown.consume({
        {"type", "response.future_event"}, {"payload", 1}}).empty());
    auto events = unknown.finish();
    const auto* truncated = first_event(events, StreamEventType::Error);
    ASSERT_NE(truncated, nullptr);
    EXPECT_EQ(truncated->provider_error.kind,
              acecode::ProviderErrorKind::MalformedSse);
    EXPECT_EQ(truncated->provider_error.provider, "grok");

    GrokResponsesStreamParser failed;
    events = failed.consume({
        {"type", "response.failed"},
        {"response", nlohmann::json{
            {"status", "failed"},
            {"error", nlohmann::json{
                {"code", "server_error"}, {"message", "upstream failed"},
            }},
        }},
    });
    const auto* failure = first_event(events, StreamEventType::Error);
    ASSERT_NE(failure, nullptr);
    EXPECT_EQ(failure->error, "upstream failed");
    EXPECT_EQ(failure->provider_error.provider, "grok");
}
