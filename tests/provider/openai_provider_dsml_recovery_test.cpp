#include <gtest/gtest.h>

#include "provider/openai_provider.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using acecode::ChatMessage;
using acecode::OpenAiCompatProvider;
using acecode::StreamEvent;
using acecode::StreamEventType;
using acecode::ToolCall;
using acecode::ToolDef;

struct LocalHttpServer {
    httplib::Server server;
    int port = 0;
    std::thread thread;

    explicit LocalHttpServer(std::function<void(httplib::Server&)> setup) {
        setup(server);
        port = server.bind_to_any_port("127.0.0.1");
        thread = std::thread([this] { server.listen_after_bind(); });
        for (int i = 0; i < 50 && !server.is_running(); ++i) {
            std::this_thread::sleep_for(10ms);
        }
    }

    ~LocalHttpServer() {
        server.stop();
        if (thread.joinable()) thread.join();
    }
};

ToolDef bash_tool() {
    ToolDef tool;
    tool.name = "bash";
    tool.description = "Run a command";
    tool.parameters = {
        {"type", "object"},
        {"properties", {{"command", {{"type", "string"}}}}},
        {"required", nlohmann::json::array({"command"})},
    };
    return tool;
}

ChatMessage user_message() {
    ChatMessage message;
    message.role = "user";
    message.content = "inspect";
    return message;
}

std::string dsml_call(std::string command = "git status") {
    return u8"<｜DSML｜tool_calls>\n"
           u8"<｜DSML｜invoke name=\"bash\">\n"
           u8"<｜DSML｜parameter name=\"command\" string=\"true\">" +
           command +
           u8"</｜DSML｜parameter>\n"
           u8"</｜DSML｜invoke>\n"
           u8"</｜DSML｜tool_calls>";
}

std::string content_event(const std::string& content,
                          const std::string& finish_reason = {}) {
    nlohmann::json choice = {{"delta", {{"content", content}}}};
    if (!finish_reason.empty()) choice["finish_reason"] = finish_reason;
    return "data: " + nlohmann::json({{"choices", {choice}}}).dump() + "\n\n";
}

std::string finish_event(const std::string& finish_reason) {
    nlohmann::json payload = {
        {"choices", {{{"delta", nlohmann::json::object()},
                       {"finish_reason", finish_reason}}}},
    };
    return "data: " + payload.dump() + "\n\n";
}

TEST(OpenAiProviderDsmlRecoveryTest, StreamingRecoversSplitDsmlWithoutVisibleMarkers) {
    LocalHttpServer server([](httplib::Server& http) {
        http.Post("/chat/completions", [](const httplib::Request&,
                                           httplib::Response& response) {
            const std::vector<std::string> chunks = {
                u8"I will inspect.\n<｜DS",
                u8"ML｜tool_calls>\n<｜DSML｜invoke name=\"bash\">\n",
                u8"<｜DSML｜parameter name=\"command\" string=\"true\">git ",
                u8"status 2>&1</｜DSML｜parameter>\n</｜DSML｜invoke>\n",
                u8"</｜DSML｜tool_calls>",
            };
            std::string body;
            for (const auto& chunk : chunks) body += content_event(chunk);
            body += finish_event("stop");
            body += "data: [DONE]\n\n";
            response.set_content(body, "text/event-stream");
            response.status = 200;
        });
    });

    OpenAiCompatProvider provider(
        "http://127.0.0.1:" + std::to_string(server.port), "", "test-model");
    std::string visible;
    std::vector<ToolCall> calls;
    std::string done_reason;
    int done_count = 0;
    auto callback = [&](const StreamEvent& event) {
        if (event.type == StreamEventType::Delta) visible += event.content;
        if (event.type == StreamEventType::ToolCall) calls.push_back(event.tool_call);
        if (event.type == StreamEventType::Done) {
            ++done_count;
            done_reason = event.finish_reason;
        }
    };

    provider.chat_stream({user_message()}, {bash_tool()}, callback);

    EXPECT_EQ(visible, "I will inspect.\n");
    EXPECT_EQ(visible.find(u8"<｜DSML｜"), std::string::npos);
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].function_name, "bash");
    EXPECT_EQ(nlohmann::json::parse(calls[0].function_arguments)["command"],
              "git status 2>&1");
    EXPECT_EQ(done_reason, "tool_calls");
    EXPECT_EQ(done_count, 1);
}

TEST(OpenAiProviderDsmlRecoveryTest, NonStreamingRecoversDsmlWithSameSemantics) {
    LocalHttpServer server([](httplib::Server& http) {
        http.Post("/chat/completions", [](const httplib::Request&,
                                           httplib::Response& response) {
            nlohmann::json payload = {
                {"choices", {{{"message", {{"role", "assistant"},
                                             {"content", "I will inspect.\n" +
                                                             dsml_call("pwd")}}},
                               {"finish_reason", "stop"}}}},
                {"usage", {{"prompt_tokens", 3},
                            {"completion_tokens", 4},
                            {"total_tokens", 7}}},
            };
            response.set_content(payload.dump(), "application/json");
            response.status = 200;
        });
    });

    OpenAiCompatProvider provider(
        "http://127.0.0.1:" + std::to_string(server.port), "", "test-model");
    const auto response = provider.chat({user_message()}, {bash_tool()});

    EXPECT_EQ(response.content, "I will inspect.\n");
    ASSERT_EQ(response.tool_calls.size(), 1u);
    EXPECT_EQ(response.tool_calls[0].function_name, "bash");
    EXPECT_EQ(nlohmann::json::parse(
                  response.tool_calls[0].function_arguments)["command"], "pwd");
    EXPECT_EQ(response.finish_reason, "tool_calls");
    EXPECT_EQ(response.usage.total_tokens, 7);
}

TEST(OpenAiProviderDsmlRecoveryTest, NativeStructuredCallWinsWithoutDuplicate) {
    LocalHttpServer server([](httplib::Server& http) {
        http.Post("/chat/completions", [](const httplib::Request&,
                                           httplib::Response& response) {
            nlohmann::json native_delta = {
                {"choices", {{{"delta", {{"tool_calls", {{{"index", 0},
                                                              {"id", "call_native"},
                                                              {"type", "function"},
                                                              {"function", {
                                                                  {"name", "bash"},
                                                                  {"arguments", R"({"command":"native"})"},
                                                              }}}}}}}}}},
            };
            std::string body = content_event(dsml_call("recovered"));
            body += "data: " + native_delta.dump() + "\n\n";
            body += finish_event("tool_calls");
            body += "data: [DONE]\n\n";
            response.set_content(body, "text/event-stream");
            response.status = 200;
        });
    });

    OpenAiCompatProvider provider(
        "http://127.0.0.1:" + std::to_string(server.port), "", "test-model");
    std::string visible;
    std::vector<ToolCall> calls;
    auto callback = [&](const StreamEvent& event) {
        if (event.type == StreamEventType::Delta) visible += event.content;
        if (event.type == StreamEventType::ToolCall) calls.push_back(event.tool_call);
    };
    provider.chat_stream({user_message()}, {bash_tool()}, callback);

    EXPECT_TRUE(visible.empty());
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].id, "call_native");
    EXPECT_EQ(nlohmann::json::parse(calls[0].function_arguments)["command"],
              "native");
}

TEST(OpenAiProviderDsmlRecoveryTest, RetryDoesNotJoinCandidateAcrossAttempts) {
    std::atomic<int> requests{0};
    LocalHttpServer server([&](httplib::Server& http) {
        http.Post("/chat/completions", [&](const httplib::Request&,
                                            httplib::Response& response) {
            const int request = ++requests;
            if (request == 1) {
                response.set_header("Retry-After", "0");
                response.set_content(content_event(u8"<｜DSML｜tool_"),
                                     "text/event-stream");
                response.status = 200;
                return;
            }
            response.set_content(
                content_event("normal after retry", "stop") +
                    "data: [DONE]\n\n",
                "text/event-stream");
            response.status = 200;
        });
    });

    OpenAiCompatProvider provider(
        "http://127.0.0.1:" + std::to_string(server.port), "", "test-model");
    std::string visible;
    int retry_events = 0;
    int tool_calls = 0;
    auto callback = [&](const StreamEvent& event) {
        if (event.type == StreamEventType::Delta) visible += event.content;
        if (event.type == StreamEventType::Retry) ++retry_events;
        if (event.type == StreamEventType::ToolCall) ++tool_calls;
    };
    provider.chat_stream({user_message()}, {bash_tool()}, callback);

    EXPECT_EQ(requests.load(), 2);
    EXPECT_EQ(retry_events, 1);
    EXPECT_EQ(visible, "normal after retry");
    EXPECT_EQ(tool_calls, 0);
}

TEST(OpenAiProviderDsmlRecoveryTest, MalformedDsmlIsHiddenWhenNativeCallExists) {
    LocalHttpServer server([](httplib::Server& http) {
        http.Post("/chat/completions", [](const httplib::Request&,
                                           httplib::Response& response) {
            nlohmann::json native_delta = {
                {"choices", {{{"delta", {{"tool_calls", {{{"index", 0},
                                                              {"id", "call_native"},
                                                              {"type", "function"},
                                                              {"function", {
                                                                  {"name", "bash"},
                                                                  {"arguments", R"({"command":"native"})"},
                                                              }}}}}}}}}},
            };
            std::string body = content_event(
                u8"<｜DSML｜tool_calls><｜DSML｜invoke name=\"unknown\">"
                u8"</｜DSML｜invoke></｜DSML｜tool_calls>");
            body += "data: " + native_delta.dump() + "\n\n";
            body += finish_event("tool_calls");
            body += "data: [DONE]\n\n";
            response.set_content(body, "text/event-stream");
            response.status = 200;
        });
    });

    OpenAiCompatProvider provider(
        "http://127.0.0.1:" + std::to_string(server.port), "", "test-model");
    std::string visible;
    std::vector<ToolCall> calls;
    auto callback = [&](const StreamEvent& event) {
        if (event.type == StreamEventType::Delta) visible += event.content;
        if (event.type == StreamEventType::ToolCall) calls.push_back(event.tool_call);
    };
    provider.chat_stream({user_message()}, {bash_tool()}, callback);

    EXPECT_TRUE(visible.empty());
    EXPECT_EQ(visible.find(u8"<｜DSML｜"), std::string::npos);
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].id, "call_native");
}

TEST(OpenAiProviderDsmlRecoveryTest, NonStreamingStripsDsmlWhenNativeCallExists) {
    LocalHttpServer server([](httplib::Server& http) {
        http.Post("/chat/completions", [](const httplib::Request&,
                                           httplib::Response& response) {
            nlohmann::json payload = {
                {"choices", {{{"message", {
                    {"role", "assistant"},
                    {"content", "I will inspect.\n" + dsml_call("pwd")},
                    {"tool_calls", {{{"id", "call_native"},
                                       {"type", "function"},
                                       {"function", {
                                           {"name", "bash"},
                                           {"arguments", R"({"command":"native"})"},
                                       }}}}},
                }},
                               {"finish_reason", "tool_calls"}}}},
            };
            response.set_content(payload.dump(), "application/json");
            response.status = 200;
        });
    });

    OpenAiCompatProvider provider(
        "http://127.0.0.1:" + std::to_string(server.port), "", "test-model");
    const auto response = provider.chat({user_message()}, {bash_tool()});

    EXPECT_EQ(response.content, "I will inspect.\n");
    EXPECT_EQ(response.content.find(u8"<｜DSML｜"), std::string::npos);
    ASSERT_EQ(response.tool_calls.size(), 1u);
    EXPECT_EQ(response.tool_calls[0].id, "call_native");
    EXPECT_EQ(nlohmann::json::parse(
                  response.tool_calls[0].function_arguments)["command"], "native");
}

} // namespace
