#include <gtest/gtest.h>

#include "provider/openai_provider.hpp"
#include "utils/joining_thread.hpp"

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace acecode;
using Json = nlohmann::json;

class ExposedProvider : public OpenAiCompatProvider {
public:
    using OpenAiCompatProvider::OpenAiCompatProvider;
    using OpenAiCompatProvider::build_request_body;
};

// Exercise the production transport against an explicitly capable local
// endpoint without making an external model request or changing the allowlist.
class LocalCapableProvider : public ExposedProvider {
public:
    using ExposedProvider::ExposedProvider;
protected:
    bool supports_prompt_cache_key() const override { return true; }
    bool supports_compaction_tool_choice_none() const override { return true; }
};

struct RequestRecorder {
    std::mutex mu;
    std::vector<Json> bodies;
    std::size_t record(const httplib::Request& request) {
        std::lock_guard<std::mutex> lock(mu);
        bodies.push_back(Json::parse(request.body));
        return bodies.size();
    }
    std::vector<Json> snapshot() {
        std::lock_guard<std::mutex> lock(mu);
        return bodies;
    }
};

struct LocalHttpServer {
    httplib::Server server;
    int port = 0;
    JoiningThread worker;

    explicit LocalHttpServer(std::function<void(httplib::Server&)> setup) {
        setup(server);
        port = server.bind_to_any_port("127.0.0.1");
        // The owner stops and joins the worker before destroying its server.
        worker = JoiningThread([this] { server.listen_after_bind(); });
        for (int i = 0; i < 50 && !server.is_running(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    ~LocalHttpServer() {
        server.stop();
        worker.join();
    }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port); }
};

std::vector<ChatMessage> messages() {
    ChatMessage user;
    user.role = "user";
    user.content = "test";
    return {user};
}

std::vector<ToolDef> tools() {
    ToolDef tool;
    tool.name = "read_file";
    tool.description = "Read a file";
    tool.parameters = {{"type", "object"},
        {"properties", {{"path", {{"type", "string"}}}}}};
    return {tool};
}

void success(httplib::Response& response, bool stream = false) {
    response.set_content(stream
        ? "data: {\"choices\":[{\"delta\":{\"content\":\"summary\"},\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n"
        : R"({"choices":[{"message":{"role":"assistant","content":"summary"},"finish_reason":"stop"}]})",
        stream ? "text/event-stream" : "application/json");
}

const char* rejection = R"({"error":{"message":"Unsupported parameter: 'prompt_cache_key'","param":"prompt_cache_key","code":"unsupported_parameter"}})";

TEST(OpenAiPromptCache, ExactOfficialEndpointAllowlist) {
    for (const auto& base : {"https://api.openai.com/v1", "https://api.mistral.ai/v1",
                            "https://API.MISTRAL.AI:443/v1/"}) {
        ExposedProvider provider(base, "", "model");
        provider.set_prompt_cache_key("session-1");
        EXPECT_EQ(provider.build_request_body(messages(), {})["prompt_cache_key"], "session-1") << base;
    }
    for (const auto& base : {"http://api.openai.com/v1", "https://api.openai.com.evil.test/v1",
                            "https://api.openai.com@evil.test/v1", "https://evil.test/api.openai.com/v1",
                            "https://api.openai.com:8443/v1", "https://api.openai.com/custom",
                            "https://api.deepseek.com/v1", "https://api.z.ai/api/paas/v4",
                            "https://open.bigmodel.cn/api/paas/v4", "http://127.0.0.1/v1"}) {
        ExposedProvider provider(base, "", "model");
        provider.set_prompt_cache_key("session-1");
        EXPECT_FALSE(provider.build_request_body(messages(), {}).contains("prompt_cache_key")) << base;
    }
    ProviderRequestOptions options;
    options.endpoint_mode = "full_url";
    ExposedProvider provider("https://api.mistral.ai/v1/chat/completions?test=1", "", "model",
        OpenAiConfig::kDefaultStreamTimeoutMs, {}, options);
    provider.set_prompt_cache_key("session-1");
    EXPECT_EQ(provider.build_request_body(messages(), {})["prompt_cache_key"], "session-1");
}

TEST(OpenAiPromptCache, StableIdentityAcrossRequestsReconfigurationAndReconstruction) {
    ExposedProvider provider("https://api.mistral.ai/v1", "", "model");
    EXPECT_FALSE(provider.build_request_body(messages(), {}).contains("prompt_cache_key"));
    provider.set_prompt_cache_key("session-1");
    EXPECT_EQ(provider.build_request_body(messages(), {}, true)["prompt_cache_key"], "session-1");
    provider.reconfigure("https://api.openai.com/v1", "");
    provider.set_model("another-model");
    EXPECT_EQ(provider.build_request_body(messages(), {})["prompt_cache_key"], "session-1");
    ExposedProvider rebuilt("https://api.mistral.ai/v1", "", "model");
    rebuilt.set_prompt_cache_key("session-1");
    EXPECT_EQ(rebuilt.build_request_body(messages(), {})["prompt_cache_key"], "session-1");
    rebuilt.set_prompt_cache_key("session-2");
    EXPECT_EQ(rebuilt.build_request_body(messages(), {})["prompt_cache_key"], "session-2");
    rebuilt.set_prompt_cache_key("");
    EXPECT_FALSE(rebuilt.build_request_body(messages(), {}).contains("prompt_cache_key"));
}

TEST(OpenAiPromptCache, CompactionKeepsSchemasAndDisablesChoiceOnlyWhereVerified) {
    for (const auto& base : {"https://api.mistral.ai/v1", "https://api.openai.com/v1",
                            "https://api.z.ai/api/paas/v4", "https://custom.test/v1"}) {
        ExposedProvider provider(base, "", "model");
        const auto normal = provider.build_request_body(messages(), tools());
        const auto compact = provider.build_request_body(messages(), tools(), false, true);
        EXPECT_TRUE(provider.supports_compaction_prefix_reuse());
        EXPECT_EQ(normal["messages"], compact["messages"]);
        EXPECT_EQ(normal["tools"], compact["tools"]);
        EXPECT_FALSE(normal.contains("tool_choice"));
        const bool official = std::string(base).find("mistral") != std::string::npos ||
            std::string(base).find("openai") != std::string::npos;
        EXPECT_EQ(compact.contains("tool_choice"), official);
        if (official) EXPECT_EQ(compact["tool_choice"], "none");
        EXPECT_FALSE(provider.build_request_body(messages(), {}, false, true).contains("tool_choice"));
    }
}

TEST(OpenAiPromptCache, DerivedProtocolsKeepLegacyCompactionDispatch) {
    class SpecializedProvider final : public ExposedProvider {
    public:
        SpecializedProvider() : ExposedProvider("https://api.openai.com/v1", "", "model") {}
        std::string name() const override { return "specialized"; }
        ChatResponse chat_cancellable(const std::vector<ChatMessage>&,
            const std::vector<ToolDef>& passed_tools, const std::atomic<bool>*) override {
            EXPECT_TRUE(passed_tools.empty());
            ChatResponse result;
            result.content = "legacy";
            return result;
        }
        void chat_stream(const std::vector<ChatMessage>&,
            const std::vector<ToolDef>&, const StreamCallback& callback,
            std::atomic<bool>*) override {
            StreamEvent event;
            event.type = StreamEventType::Done;
            event.content = "legacy-stream";
            callback(event);
        }
    } provider;
    provider.set_prompt_cache_key("session-1");
    EXPECT_FALSE(provider.supports_compaction_prefix_reuse());
    EXPECT_FALSE(provider.build_request_body(messages(), {}).contains("prompt_cache_key"));
    EXPECT_EQ(provider.chat_for_compaction(messages(), tools(), nullptr).content, "legacy");
    ChatRequestOptions options{"session-2", true};
    EXPECT_EQ(provider.chat_with_options(messages(), tools(), options, nullptr).content, "legacy");
    std::string result;
    provider.chat_stream_with_options(messages(), tools(), options,
        [&](const StreamEvent& event) { result = event.content; });
    EXPECT_EQ(result, "legacy-stream");
}

TEST(OpenAiPromptCache, NonStreamingDowngradesOnceAndRemembersPerEndpointModel) {
    RequestRecorder recorder;
    LocalHttpServer server([&](httplib::Server& http) {
        http.Post("/chat/completions", [&](const httplib::Request& request, httplib::Response& response) {
            const auto index = recorder.record(request);
            if (index == 1) {
                response.status = 400;
                response.set_content(rejection, "application/json");
            } else success(response);
        });
    });
    LocalCapableProvider provider(server.url(), "", "model");
    provider.set_prompt_cache_key("session-1");
    EXPECT_EQ(provider.chat(messages(), tools()).content, "summary");
    EXPECT_EQ(provider.chat(messages(), tools()).content, "summary");
    provider.set_model("another-model");
    EXPECT_EQ(provider.chat(messages(), tools()).content, "summary");
    provider.set_model("model");
    EXPECT_EQ(provider.chat(messages(), tools()).content, "summary");
    const auto bodies = recorder.snapshot();
    ASSERT_EQ(bodies.size(), 5u);
    EXPECT_EQ(bodies[0]["prompt_cache_key"], "session-1");
    auto expected_retry = bodies[0];
    expected_retry.erase("prompt_cache_key");
    EXPECT_EQ(bodies[1], expected_retry);
    EXPECT_FALSE(bodies[2].contains("prompt_cache_key"));
    EXPECT_EQ(bodies[3]["prompt_cache_key"], "session-1");
    EXPECT_FALSE(bodies[4].contains("prompt_cache_key"));
    provider.reconfigure(server.url() + "/other", "");
    EXPECT_TRUE(provider.build_request_body(messages(), {}).contains("prompt_cache_key"));
    provider.reconfigure(server.url(), "");
    EXPECT_FALSE(provider.build_request_body(messages(), {}).contains("prompt_cache_key"));
}

TEST(OpenAiPromptCache, StreamingDowngradeIsSilentAndSharedWithNonStreaming) {
    RequestRecorder recorder;
    LocalHttpServer server([&](httplib::Server& http) {
        http.Post("/chat/completions", [&](const httplib::Request& request, httplib::Response& response) {
            const auto index = recorder.record(request);
            if (index == 1) {
                response.status = 422;
                response.set_content(R"({"detail":[{"type":"extra_forbidden","loc":["body","prompt_cache_key"],"msg":"Extra inputs are not permitted"}]})", "application/json");
            } else success(response, Json::parse(request.body).value("stream", false));
        });
    });
    LocalCapableProvider provider(server.url(), "", "model");
    provider.set_prompt_cache_key("session-1");
    std::vector<StreamEvent> events;
    std::atomic<bool> abort{false};
    provider.chat_stream(messages(), tools(), [&](const StreamEvent& event) {
        events.push_back(event);
        if (event.type == StreamEventType::Retry) abort = true;
    }, &abort);
    EXPECT_FALSE(abort.load());
    EXPECT_EQ(provider.chat(messages(), tools()).content, "summary");
    int done = 0;
    for (const auto& event : events) {
        EXPECT_NE(event.type, StreamEventType::Error);
        if (event.type == StreamEventType::Done) ++done;
    }
    EXPECT_EQ(done, 1);
    const auto bodies = recorder.snapshot();
    ASSERT_EQ(bodies.size(), 3u);
    EXPECT_EQ(bodies[0]["prompt_cache_key"], "session-1");
    EXPECT_FALSE(bodies[1].contains("prompt_cache_key"));
    EXPECT_FALSE(bodies[2].contains("prompt_cache_key"));
}

TEST(OpenAiPromptCache, UnrelatedFailuresPreserveOriginalErrorAndDoNotDisableKey) {
    const std::vector<std::pair<int, std::string>> failures = {
        {400, R"({"error":{"message":"Unknown parameter: 'temperature'","param":"temperature","code":"unsupported_parameter"},"request":{"prompt_cache_key":"session-1"}})"},
        {400, R"({"error":{"message":"prompt_cache_key must be shorter","param":"prompt_cache_key","code":"invalid_value"}})"},
        {400, R"({"error":{"message":"prompt_cache_key is not allowed to contain spaces","param":"prompt_cache_key","code":"invalid_value"}})"},
        {400, R"({"error":{"message":"Unsupported parameter: prompt_cache_key","param":"temperature","code":"unsupported_parameter"}})"},
        {422, R"({"detail":[{"type":"extra_forbidden","loc":["body","messages",0,"prompt_cache_key"]}]})"},
        {400, "Unknown parameter: prompt_cache_key_extra"},
        {401, rejection},
        {429, R"({"error":{"code":"insufficient_quota","message":"Unsupported parameter: prompt_cache_key"}})"},
    };
    for (const auto& failure : failures) {
        RequestRecorder recorder;
        LocalHttpServer server([&](httplib::Server& http) {
            http.Post("/chat/completions", [&](const httplib::Request& request, httplib::Response& response) {
                recorder.record(request);
                response.status = failure.first;
                response.set_content(failure.second, "application/json");
            });
        });
        LocalCapableProvider provider(server.url(), "", "model");
        provider.set_prompt_cache_key("session-1");
        const auto response = provider.chat(messages(), {});
        EXPECT_EQ(recorder.snapshot().size(), 1u) << failure.second;
        EXPECT_EQ(response.provider_error.status_code, failure.first);
        EXPECT_EQ(response.provider_error.raw_body, failure.second);
        EXPECT_TRUE(provider.build_request_body(messages(), {}).contains("prompt_cache_key"));
    }
}

TEST(OpenAiPromptCache, SecondRejectionRemainsTerminal) {
    RequestRecorder recorder;
    LocalHttpServer server([&](httplib::Server& http) {
        http.Post("/chat/completions", [&](const httplib::Request& request, httplib::Response& response) {
            recorder.record(request);
            response.status = 400;
            response.set_content(rejection, "application/json");
        });
    });
    LocalCapableProvider provider(server.url(), "", "model");
    provider.set_prompt_cache_key("session-1");
    const auto response = provider.chat(messages(), {});
    EXPECT_EQ(recorder.snapshot().size(), 2u);
    EXPECT_EQ(response.provider_error.status_code, 400);
    EXPECT_EQ(response.provider_error.raw_body, rejection);
}

TEST(OpenAiPromptCache, CancellationDuringRejectionNeverRetriesOrDisablesKey) {
    for (const bool stream : {false, true}) {
        RequestRecorder recorder;
        std::atomic<bool> abort{false};
        LocalHttpServer server([&](httplib::Server& http) {
            http.Post("/chat/completions", [&](const httplib::Request& request, httplib::Response& response) {
                recorder.record(request);
                response.status = 400;
                response.set_content(rejection, "application/json");
                abort = true;
            });
        });
        LocalCapableProvider provider(server.url(), "", "model");
        provider.set_prompt_cache_key("session-1");
        if (stream) {
            ProviderErrorInfo error;
            provider.chat_stream(messages(), {}, [&](const StreamEvent& event) {
                if (event.type == StreamEventType::Error) error = event.provider_error;
            }, &abort);
            EXPECT_EQ(error.kind, ProviderErrorKind::UserCancelled);
        } else {
            EXPECT_EQ(provider.chat_cancellable(messages(), {}, &abort).provider_error.kind,
                ProviderErrorKind::UserCancelled);
        }
        EXPECT_EQ(recorder.snapshot().size(), 1u);
        EXPECT_TRUE(provider.build_request_body(messages(), {}).contains("prompt_cache_key"));
    }
}

TEST(OpenAiPromptCache, CompactionOptionsAreRequestScoped) {
    RequestRecorder recorder;
    LocalHttpServer server([&](httplib::Server& http) {
        http.Post("/chat/completions", [&](const httplib::Request& request, httplib::Response& response) {
            recorder.record(request);
            success(response);
        });
    });
    LocalCapableProvider provider(server.url(), "", "model");
    provider.set_prompt_cache_key("session-1");
    EXPECT_EQ(provider.chat_for_compaction(messages(), tools(), nullptr).content, "summary");
    EXPECT_EQ(provider.chat(messages(), tools()).content, "summary");
    const auto bodies = recorder.snapshot();
    ASSERT_EQ(bodies.size(), 2u);
    EXPECT_EQ(bodies[0]["tool_choice"], "none");
    auto compact = bodies[0];
    compact.erase("tool_choice");
    EXPECT_EQ(compact, bodies[1]);
}

TEST(OpenAiPromptCache, StreamingErrorAfterOutputNeverSilentlyReplays) {
    RequestRecorder recorder;
    LocalHttpServer server([&](httplib::Server& http) {
        http.Post("/chat/completions", [&](const httplib::Request& request, httplib::Response& response) {
            recorder.record(request);
            response.set_content(
                "data: {\"choices\":[{\"delta\":{\"content\":\"partial output\"}}]}\n\n"
                "data: {\"status_code\":400,\"error\":{\"message\":\"Unsupported parameter: prompt_cache_key\"}}\n\n",
                "text/event-stream");
        });
    });
    LocalCapableProvider provider(server.url(), "", "model");
    provider.set_prompt_cache_key("session-1");
    std::atomic<bool> abort{false};
    ProviderErrorInfo error;
    std::string content;
    provider.chat_stream(messages(), {}, [&](const StreamEvent& event) {
        if (event.type == StreamEventType::Delta) content += event.content;
        if (event.type == StreamEventType::Error) error = event.provider_error;
        if (event.type == StreamEventType::Retry) abort = true;
    }, &abort);
    EXPECT_EQ(recorder.snapshot().size(), 1u);
    EXPECT_EQ(content, "partial output");
    EXPECT_EQ(error.status_code, 400);
    EXPECT_FALSE(abort.load());
    EXPECT_TRUE(provider.build_request_body(messages(), {}).contains("prompt_cache_key"));
}

TEST(OpenAiPromptCache, ConcurrentKeyUpdatesProduceWholeSnapshots) {
    ExposedProvider provider("https://api.mistral.ai/v1", "", "model");
    const std::string first(64, 'a');
    const std::string second(64, 'b');
    provider.set_prompt_cache_key(first);
    JoiningThread writer([&provider, first, second] {
        for (int i = 0; i < 500; ++i) provider.set_prompt_cache_key(i % 2 ? first : second);
    });
    for (int i = 0; i < 500; ++i) {
        const auto key = provider.build_request_body(messages(), {})["prompt_cache_key"].get<std::string>();
        EXPECT_TRUE(key == first || key == second);
    }
    writer.join();
}

TEST(OpenAiPromptCache, ConcurrentSessionsKeepPerCallIdentityAndCompactionOptions) {
    RequestRecorder recorder;
    std::mutex arrival_mu;
    std::condition_variable arrival_cv;
    int arrivals = 0;
    std::atomic<bool> overlap_timed_out{false};
    LocalHttpServer server([&](httplib::Server& http) {
        http.Post("/chat/completions", [&](const httplib::Request& request, httplib::Response& response) {
            recorder.record(request);
            {
                std::unique_lock<std::mutex> lock(arrival_mu);
                ++arrivals;
                arrival_cv.notify_all();
                if (!arrival_cv.wait_for(lock, std::chrono::seconds(5), [&] { return arrivals == 2; })) {
                    overlap_timed_out = true;
                    response.status = 400;
                    response.set_content("concurrent request did not arrive", "text/plain");
                    return;
                }
            }
            success(response, Json::parse(request.body).value("stream", false));
        });
    });
    LocalCapableProvider provider(server.url(), "", "model");
    provider.set_prompt_cache_key("legacy-key");
    std::string streamed;
    ChatResponse compacted;
    std::atomic<bool> abort{false};
    JoiningThread first([&] {
        auto input = messages();
        input[0].content = "session-a";
        provider.chat_stream_with_options(input, tools(), {"session-a", false},
            [&](const StreamEvent& event) {
                if (event.type == StreamEventType::Delta) streamed += event.content;
                if (event.type == StreamEventType::Retry) abort = true;
            }, &abort);
    });
    JoiningThread second([&] {
        auto input = messages();
        input[0].content = "session-b";
        compacted = provider.chat_with_options(input, tools(), {"session-b", true}, nullptr);
    });
    first.join();
    second.join();
    EXPECT_FALSE(overlap_timed_out.load());
    EXPECT_FALSE(abort.load());
    EXPECT_EQ(streamed, "summary");
    EXPECT_EQ(compacted.content, "summary");
    const auto bodies = recorder.snapshot();
    ASSERT_EQ(bodies.size(), 2u);
    for (const auto& body : bodies) {
        const auto session = body["messages"][0]["content"].get<std::string>();
        EXPECT_EQ(body["prompt_cache_key"], session);
        EXPECT_EQ(body.contains("tool_choice"), session == "session-b");
    }
    EXPECT_EQ(provider.build_request_body(messages(), {})["prompt_cache_key"], "legacy-key");
    const ChatRequestOptions no_key;
    EXPECT_FALSE(provider.build_request_body(messages(), {}, false, false, &no_key).contains("prompt_cache_key"));
}

TEST(OpenAiPromptCache, DowngradePreservesTheFinalUpstreamFailure) {
    for (const bool stream : {false, true}) {
        RequestRecorder recorder;
        const std::string final_body = R"({"error":{"message":"credential expired","code":"invalid_api_key"}})";
        LocalHttpServer server([&](httplib::Server& http) {
            http.Post("/chat/completions", [&](const httplib::Request& request, httplib::Response& response) {
                const auto index = recorder.record(request);
                response.status = index == 1 ? 400 : 401;
                response.set_header("x-request-id", index == 1 ? "first-rejection" : "final-failure");
                response.set_content(index == 1 ? rejection : final_body, "application/json");
            });
        });
        LocalCapableProvider provider(server.url(), "", "model");
        ProviderErrorInfo error;
        const ChatRequestOptions options{"session-1", false};
        std::atomic<bool> abort{false};
        if (stream) {
            provider.chat_stream_with_options(messages(), {}, options, [&](const StreamEvent& event) {
                if (event.type == StreamEventType::Error) error = event.provider_error;
                if (event.type == StreamEventType::Retry) abort = true;
            }, &abort);
        } else {
            error = provider.chat_with_options(messages(), {}, options, &abort).provider_error;
        }
        EXPECT_EQ(recorder.snapshot().size(), 2u);
        EXPECT_EQ(error.status_code, 401);
        EXPECT_EQ(error.request_id, "final-failure");
        EXPECT_EQ(error.raw_body, final_body);
        EXPECT_FALSE(abort.load());
    }
}

} // namespace
