#include "speech/speech_client.hpp"
#include "test_support/network/crow_test_server.hpp"
#include "utils/abandonable_call.hpp"
#include <gtest/gtest.h>
#include <future>
#include <mutex>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <iterator>

using namespace acecode;
using namespace acecode::speech;

TEST(SpeechClient, ProtocolPathsAndFields) {
    SpeechRecognitionConfig config;
    config.base_url = "http://speech.internal:9876/";
    EXPECT_EQ(request_plan(config).url, "http://speech.internal:9876/api/transcriptions");
    EXPECT_EQ(request_plan(config).file_field, "audio");
    config.protocol = "openai";
    config.model = "whisper-local";
    EXPECT_EQ(request_plan(config).url, "http://speech.internal:9876/v1/audio/transcriptions");
    config.base_url += "v1";
    EXPECT_EQ(request_plan(config).url, "http://speech.internal:9876/v1/audio/transcriptions");
    config.protocol = "whisper_cpp";
    config.base_url = "http://speech.internal:9888";
    EXPECT_EQ(request_plan(config).url, "http://speech.internal:9888/inference");
    EXPECT_EQ(request_plan(config).file_field, "file");
}

TEST(SpeechClient, ExternalSenseVoiceModel) {
    const char* url = std::getenv("ACECODE_TEST_SPEECH_URL");
    const char* wav = std::getenv("ACECODE_TEST_SPEECH_WAV");
    if (!url || !wav) GTEST_SKIP() << "Set ACECODE_TEST_SPEECH_URL and ACECODE_TEST_SPEECH_WAV for model integration";
    std::ifstream input(wav, std::ios::binary);
    ASSERT_TRUE(input.good());
    std::string audio{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    SpeechRecognitionConfig config;
    config.base_url = url;
    auto cancelled = std::make_shared<std::atomic<bool>>(false); // test and request share cancellation
    const auto result = transcribe(config, std::move(audio), cancelled);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_FALSE(result.text.empty());
    RecordProperty("transcript", result.text);
    if (const char* expected = std::getenv("ACECODE_TEST_SPEECH_EXPECT"))
        EXPECT_NE(result.text.find(expected), std::string::npos) << result.text;
}

TEST(SpeechClient, BoundedWavAndStableErrors) {
    const auto audio = silent_wav();
    EXPECT_TRUE(valid_wav(audio, 1));
    EXPECT_FALSE(valid_wav("broken", 60));
    auto broken = audio;
    broken[24] = 0;
    EXPECT_FALSE(valid_wav(broken, 60));
    EXPECT_FALSE(valid_wav(audio + "extra", 60));
    EXPECT_EQ(parse_response(429, "secret upstream response").error, "SPEECH_BUSY");
    EXPECT_EQ(parse_response(401, "secret upstream response").error, "SPEECH_AUTH_FAILED");
    EXPECT_EQ(parse_response(503, "{}").error, "SPEECH_NOT_READY");
    EXPECT_FALSE(parse_response(200, R"({"error":"secret","text":"ignored"})").ok);
    EXPECT_FALSE(parse_response(200, R"({"text":null})").ok);
    EXPECT_TRUE(parse_response(200, R"({"status":"no_speech","text":""})").ok);
    EXPECT_EQ(parse_response(200, R"({"text":"hello"})").text, "hello");
}

TEST(SpeechClient, MultipartUsesMemoryAndSelectedProtocol) {
    crow::SimpleApp app;
    std::vector<std::pair<std::string, std::string>> received;
    std::mutex mu;
    const auto receive = [&](const crow::request& req) {
        std::lock_guard<std::mutex> lock(mu);
        received.emplace_back(req.url, req.body);
        EXPECT_EQ(req.get_header_value("Authorization"), "Bearer fixture-key");
        EXPECT_NE(req.get_header_value("Content-Type").find("multipart/form-data"), std::string::npos);
        return crow::response(200, R"({"text":"hello"})");
    };
    CROW_ROUTE(app, "/api/transcriptions").methods(crow::HTTPMethod::POST)(receive);
    CROW_ROUTE(app, "/v1/audio/transcriptions").methods(crow::HTTPMethod::POST)(receive);
    CROW_ROUTE(app, "/inference").methods(crow::HTTPMethod::POST)(receive);
    test::CrowTestServer server(app);
    for (const auto& protocol : {"sensevoice", "openai", "whisper_cpp"}) {
        SpeechRecognitionConfig config;
        config.protocol = protocol;
        config.base_url = server.http_base();
        config.model = "whisper-local";
        config.api_key = "fixture-key";
        auto cancelled = std::make_shared<std::atomic<bool>>(false); // test and request share cancellation
        const auto result = transcribe(config, silent_wav(), cancelled);
        EXPECT_TRUE(result.ok) << result.error;
        EXPECT_EQ(result.text, "hello");
    }
    std::lock_guard<std::mutex> lock(mu);
    ASSERT_EQ(received.size(), 3u);
    EXPECT_NE(received[0].second.find("name=\"audio\""), std::string::npos);
    EXPECT_NE(received[1].second.find("name=\"file\""), std::string::npos);
    EXPECT_NE(received[1].second.find("whisper-local"), std::string::npos);
    EXPECT_EQ(received[2].second.find("name=\"model\""), std::string::npos);
    for (const auto& item : received) EXPECT_NE(item.second.find(silent_wav()), std::string::npos);
}

TEST(SpeechClient, CancelledAndShutdownRequestsNeverStart) {
    SpeechRecognitionConfig config;
    config.base_url = "http://127.0.0.1:1";
    auto cancelled = std::make_shared<std::atomic<bool>>(true); // test and request share cancellation
    EXPECT_EQ(transcribe(config, silent_wav(), cancelled).error, "SPEECH_CANCELLED");
    cancelled->store(false);
    std::atomic<bool> shutdown{true};
    EXPECT_EQ(transcribe(config, silent_wav(), cancelled, &shutdown).error, "SPEECH_CANCELLED");
    EXPECT_TRUE(cancelled->load());
}

TEST(SpeechClient, InFlightCancellationReturnsBeforeServerFinishes) {
    crow::SimpleApp app;
    std::promise<void> entered;
    auto waiting = entered.get_future();
    std::mutex mu;
    std::condition_variable cv;
    bool release = false;
    CROW_ROUTE(app, "/api/transcriptions").methods(crow::HTTPMethod::POST)
    ([&](const crow::request&) {
        entered.set_value();
        std::unique_lock<std::mutex> lock(mu);
        cv.wait_for(lock, std::chrono::seconds(3), [&] { return release; });
        return crow::response(200, R"({"text":"late"})");
    });
    test::CrowTestServer server(app);
    SpeechRecognitionConfig config;
    config.base_url = server.http_base();
    auto cancelled = std::make_shared<std::atomic<bool>>(false); // test and request share cancellation
    auto result = std::async(std::launch::async, [config, cancelled] {
        return transcribe(config, silent_wav(), cancelled);
    });
    EXPECT_EQ(waiting.wait_for(std::chrono::seconds(3)), std::future_status::ready);
    cancelled->store(true);
    EXPECT_EQ(result.wait_for(std::chrono::milliseconds(500)), std::future_status::ready);
    EXPECT_EQ(result.get().error, "SPEECH_CANCELLED");
    { std::lock_guard<std::mutex> lock(mu); release = true; }
    cv.notify_all();
    EXPECT_TRUE(wait_for_abandoned_work(std::chrono::seconds(3)));
}
