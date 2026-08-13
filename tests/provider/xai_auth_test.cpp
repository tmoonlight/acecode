#include "provider/auth/xai_auth.hpp"
#include "utils/utf8_path.hpp"

#include <gtest/gtest.h>
#include <httplib.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>

using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path root;

    explicit TempDir(const std::string& name) {
        root = fs::temp_directory_path() /
            (name + "_" + std::to_string(std::chrono::steady_clock::now()
                .time_since_epoch().count()));
        fs::create_directories(root);
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

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

    std::string url(const std::string& path = "") const {
        return "http://127.0.0.1:" + std::to_string(port) + path;
    }
};

int64_t future_time() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() + 3600;
}

} // namespace

TEST(GrokAuthTest, ParsesDeviceCodeAndOAuthStates) {
    const auto device = acecode::parse_grok_device_code_response(200, R"({
        "device_code":"device-secret",
        "user_code":"ABCD-EFGH",
        "verification_uri":"https://auth.x.ai/activate",
        "verification_uri_complete":"https://auth.x.ai/activate?user_code=ABCD-EFGH",
        "interval":7,
        "expires_in":1200
    })");
    EXPECT_EQ(device.device_code, "device-secret");
    EXPECT_EQ(device.user_code, "ABCD-EFGH");
    EXPECT_EQ(device.interval, 7);
    EXPECT_EQ(device.expires_in, 1200);

    const auto defaults = acecode::parse_grok_device_code_response(200, R"({
        "device_code":"device-secret",
        "user_code":"ABCD-EFGH",
        "verification_uri":"https://auth.x.ai/activate",
        "interval":0,
        "expires_in":-1
    })");
    EXPECT_EQ(defaults.interval, 5);
    EXPECT_EQ(defaults.expires_in, 1800);

    const auto pending = acecode::parse_grok_token_response(
        400, R"({"error":"authorization_pending"})");
    EXPECT_EQ(pending.status, "pending");

    const auto slow = acecode::parse_grok_token_response(
        400, R"({"error":"slow_down"})");
    EXPECT_EQ(slow.status, "slow_down");
    EXPECT_EQ(slow.interval_delta_seconds, 5);

    const auto denied = acecode::parse_grok_token_response(
        400, R"({"error":"access_denied"})");
    EXPECT_EQ(denied.status, "expired");
}

TEST(GrokAuthTest, ParsesRotatedTokenAndIdentityWithoutLeakingSecrets) {
    const auto result = acecode::parse_grok_token_response(
        200,
        R"({"access_token":"access-new","refresh_token":"refresh-new","id_token":"e30.eyJzdWIiOiJ1c2VyLTEiLCJlbWFpbCI6InVAZXhhbXBsZS5jb20ifQ.sig","expires_in":3600})",
        "refresh-old", false);
    ASSERT_EQ(result.status, "authorized");
    EXPECT_EQ(result.tokens.access_token, "access-new");
    EXPECT_EQ(result.tokens.refresh_token, "refresh-new");
    EXPECT_EQ(result.tokens.user_id, "user-1");
    EXPECT_EQ(result.tokens.email, "u@example.com");
    EXPECT_GT(result.tokens.expires_at, future_time() - 120);

    const auto zero_expiry = acecode::parse_grok_token_response(
        200,
        R"({"access_token":"access-new","refresh_token":"refresh-new","expires_in":0})",
        "", false);
    EXPECT_GT(zero_expiry.tokens.expires_at, future_time() - 120);

    const std::string diagnostic = acecode::redact_grok_auth_diagnostic(
        R"({"refresh_token":"very-secret","accessToken":"camel-secret","device_code":"device-secret","email":"private@example.com","user_id":"private-user"} Authorization=BearerToken Bearer access.secret x-email=pair@example.com abcdefghij.klmnopqrst.uvwxyzABCD)");
    EXPECT_EQ(diagnostic.find("very-secret"), std::string::npos);
    EXPECT_EQ(diagnostic.find("device-secret"), std::string::npos);
    EXPECT_EQ(diagnostic.find("access.secret"), std::string::npos);
    EXPECT_EQ(diagnostic.find("private@example.com"), std::string::npos);
    EXPECT_EQ(diagnostic.find("private-user"), std::string::npos);
    EXPECT_EQ(diagnostic.find("pair@example.com"), std::string::npos);
    EXPECT_EQ(diagnostic.find("camel-secret"), std::string::npos);
    EXPECT_EQ(diagnostic.find("abcdefghij.klmnopqrst.uvwxyzABCD"),
              std::string::npos);
    EXPECT_NE(diagnostic.find("[REDACTED]"), std::string::npos);
}

TEST(GrokAuthTest, PersistsAndDeletesCredentialsAtomically) {
    TempDir temp("acecode_grok_auth");
    const std::string path = acecode::path_to_utf8(temp.root / "nested" / "grok_auth.json");
    acecode::GrokAuthTokens tokens{
        "access", "refresh", future_time(), "user-id", "user@example.com"};
    std::string error;
    ASSERT_TRUE(acecode::save_grok_auth_tokens(tokens, path, &error)) << error;
    EXPECT_TRUE(acecode::has_saved_grok_auth(path));
    EXPECT_FALSE(fs::exists(temp.root / "nested" / "grok_auth.json.tmp"));

    const auto loaded = acecode::load_grok_auth_tokens(path);
    EXPECT_EQ(loaded.access_token, "access");
    EXPECT_EQ(loaded.refresh_token, "refresh");
    EXPECT_EQ(loaded.user_id, "user-id");
    EXPECT_EQ(loaded.email, "user@example.com");

    {
        std::ofstream leftover(path + ".tmp", std::ios::binary);
        leftover << "temporary-credential-material";
    }
    ASSERT_TRUE(fs::exists(acecode::path_from_utf8(path + ".tmp")));

    ASSERT_TRUE(acecode::delete_grok_auth(path, &error)) << error;
    EXPECT_FALSE(acecode::has_saved_grok_auth(path));
    EXPECT_FALSE(fs::exists(acecode::path_from_utf8(path + ".tmp")));
}

TEST(GrokAuthTest, DeviceFlowUsesGrokHeadersAndPersistsAuthorization) {
    TempDir temp("acecode_grok_device_flow");
    std::atomic<int> device_requests{0};
    std::atomic<int> token_requests{0};
    LocalHttpServer upstream([&](httplib::Server& server) {
        server.Post("/device", [&](const httplib::Request& request,
                                   httplib::Response& response) {
            ++device_requests;
            EXPECT_EQ(request.get_header_value("x-grok-client-version"), "test-version");
            EXPECT_EQ(request.get_header_value("x-grok-client-surface"), "ui");
            EXPECT_EQ(request.get_param_value("client_id"), "test-client");
            EXPECT_EQ(request.get_param_value("referrer"), "grok-build");
            response.set_content(
                R"({"device_code":"device","user_code":"CODE","verification_uri":"https://auth.x.ai/activate","interval":5,"expires_in":1800})",
                "application/json");
        });
        server.Post("/token", [&](const httplib::Request& request,
                                  httplib::Response& response) {
            ++token_requests;
            EXPECT_EQ(request.get_header_value("x-grok-client-version"), "test-version");
            EXPECT_EQ(request.get_header_value("x-grok-client-surface"), "ui");
            EXPECT_EQ(request.get_param_value("device_code"), "device");
            response.set_content(
                R"({"access_token":"access","refresh_token":"refresh","expires_in":3600})",
                "application/json");
        });
    });

    acecode::GrokAuthConfig config;
    config.client_id = "test-client";
    config.client_version = "test-version";
    config.device_url = upstream.url("/device");
    config.token_url = upstream.url("/token");
    config.credential_path = acecode::path_to_utf8(temp.root / "grok_auth.json");

    const auto device = acecode::request_grok_device_code(config);
    ASSERT_TRUE(device.error.empty()) << device.message;
    EXPECT_EQ(device.device_code, "device");
    const auto poll = acecode::poll_grok_device_code_once(device.device_code, config);
    ASSERT_EQ(poll.status, "authorized") << poll.message;
    EXPECT_TRUE(acecode::has_saved_grok_auth(config.credential_path));
    EXPECT_EQ(device_requests.load(), 1);
    EXPECT_EQ(token_requests.load(), 1);
}

TEST(GrokAuthTest, RefreshRotationAndRejectedTokenRetryAreSingleShot) {
    TempDir temp("acecode_grok_refresh");
    std::atomic<int> refresh_requests{0};
    LocalHttpServer upstream([&](httplib::Server& server) {
        server.Post("/token", [&](const httplib::Request& request,
                                  httplib::Response& response) {
            const int call = ++refresh_requests;
            EXPECT_TRUE(request.get_header_value("x-grok-client-version").empty());
            EXPECT_EQ(request.get_param_value("grant_type"), "refresh_token");
            EXPECT_EQ(request.get_param_value("refresh_token"),
                      call == 1 ? "refresh-0" : "refresh-1");
            response.set_content(
                call == 1
                    ? R"({"access_token":"access-1","refresh_token":"refresh-1","expires_in":3600})"
                    : R"({"access_token":"access-2","refresh_token":"refresh-2","expires_in":3600})",
                "application/json");
        });
    });

    acecode::GrokAuthConfig config;
    config.token_url = upstream.url("/token");
    config.credential_path = acecode::path_to_utf8(temp.root / "grok_auth.json");
    ASSERT_TRUE(acecode::save_grok_auth_tokens(
        {"access-0", "refresh-0", 1, "user", "u@example.com"},
        config.credential_path));

    auto first = acecode::ensure_grok_access_token(false, "", config);
    ASSERT_TRUE(first.ok) << first.message;
    EXPECT_EQ(first.tokens.access_token, "access-1");
    EXPECT_EQ(first.tokens.refresh_token, "refresh-1");

    auto after_401 = acecode::ensure_grok_access_token(
        true, first.tokens.access_token, config);
    ASSERT_TRUE(after_401.ok) << after_401.message;
    EXPECT_EQ(after_401.tokens.access_token, "access-2");

    auto already_rotated = acecode::ensure_grok_access_token(
        true, first.tokens.access_token, config);
    ASSERT_TRUE(already_rotated.ok);
    EXPECT_EQ(already_rotated.tokens.access_token, "access-2");
    EXPECT_EQ(refresh_requests.load(), 2);
}

TEST(GrokAuthTest, LogoutWaitsForInflightRefreshAndRemovesRotatedCredentials) {
    TempDir temp("acecode_grok_refresh_logout");
    std::atomic<bool> refresh_started{false};
    std::atomic<bool> release_refresh{false};
    LocalHttpServer upstream([&](httplib::Server& server) {
        server.Post("/token", [&](const httplib::Request&,
                                  httplib::Response& response) {
            refresh_started.store(true);
            while (!release_refresh.load()) {
                std::this_thread::sleep_for(1ms);
            }
            response.set_content(
                R"({"access_token":"access-new","refresh_token":"refresh-new","expires_in":3600})",
                "application/json");
        });
    });

    acecode::GrokAuthConfig config;
    config.token_url = upstream.url("/token");
    config.credential_path = acecode::path_to_utf8(temp.root / "grok_auth.json");
    ASSERT_TRUE(acecode::save_grok_auth_tokens(
        {"access-old", "refresh-old", 1, "user", "u@example.com"},
        config.credential_path));

    acecode::GrokAccessTokenResult refresh_result;
    std::thread refresh([&] {
        refresh_result = acecode::ensure_grok_access_token(false, "", config);
    });
    for (int i = 0; i < 1000 && !refresh_started.load(); ++i) {
        std::this_thread::sleep_for(1ms);
    }
    ASSERT_TRUE(refresh_started.load());

    std::atomic<bool> logout_started{false};
    std::atomic<bool> logout_done{false};
    bool logout_ok = false;
    std::string logout_error;
    std::thread logout([&] {
        logout_started.store(true);
        logout_ok = acecode::delete_grok_auth(
            config.credential_path, &logout_error);
        logout_done.store(true);
    });
    for (int i = 0; i < 1000 && !logout_started.load(); ++i) {
        std::this_thread::sleep_for(1ms);
    }
    ASSERT_TRUE(logout_started.load());
    std::this_thread::sleep_for(20ms);
    EXPECT_FALSE(logout_done.load());

    release_refresh.store(true);
    refresh.join();
    logout.join();

    ASSERT_TRUE(refresh_result.ok) << refresh_result.message;
    ASSERT_TRUE(logout_ok) << logout_error;
    EXPECT_FALSE(acecode::has_saved_grok_auth(config.credential_path));
}

TEST(GrokAuthTest, ModelProbeRefreshesOnceAfterUnauthorized) {
    TempDir temp("acecode_grok_models");
    std::atomic<int> model_requests{0};
    std::atomic<int> refresh_requests{0};
    LocalHttpServer upstream([&](httplib::Server& server) {
        server.Post("/token", [&](const httplib::Request&,
                                  httplib::Response& response) {
            ++refresh_requests;
            response.set_content(
                R"({"access_token":"access-new","refresh_token":"refresh-new","expires_in":3600})",
                "application/json");
        });
        server.Get("/v1/models", [&](const httplib::Request& request,
                                     httplib::Response& response) {
            ++model_requests;
            EXPECT_EQ(request.get_header_value("X-XAI-Token-Auth"), "xai-grok-cli");
            if (request.get_header_value("Authorization") == "Bearer access-old") {
                response.status = 401;
                response.set_content(R"({"error":"expired"})", "application/json");
                return;
            }
            EXPECT_EQ(request.get_header_value("Authorization"), "Bearer access-new");
            response.set_content(
                R"({"data":[{"id":"grok-4.5"},{"id":"grok-code-fast-1"}]})",
                "application/json");
        });
    });

    acecode::GrokAuthConfig config;
    config.token_url = upstream.url("/token");
    config.build_base_url = upstream.url("/v1");
    config.credential_path = acecode::path_to_utf8(temp.root / "grok_auth.json");
    ASSERT_TRUE(acecode::save_grok_auth_tokens(
        {"access-old", "refresh-old", future_time(), "user", "u@example.com"},
        config.credential_path));

    const auto result = acecode::fetch_grok_model_ids(config);
    ASSERT_TRUE(result.error.empty()) << result.message;
    ASSERT_EQ(result.models.size(), 2u);
    EXPECT_EQ(result.models[0], "grok-4.5");
    EXPECT_EQ(model_requests.load(), 2);
    EXPECT_EQ(refresh_requests.load(), 1);
}

TEST(GrokAuthTest, ParsesAndDeduplicatesModelLists) {
    std::string error;
    const auto models = acecode::parse_grok_model_ids(
        R"({"data":[
            {"id":"grok-4.5","model":"must-not-replace-id"},
            {"model":"grok-composer-2.5-fast"},
            {"modelId":"future-model"},
            {"_meta":{"model":"meta-model"}},
            {"_meta":{"modelId":"meta-id-model"}},
            {"id":"hidden-model","hidden":true},
            {"id":"meta-hidden-model","_meta":{"hidden":true}},
            {"id":"grok-4.5"},
            "grok-code-fast-1",
            {"ignored":true}
        ]})",
        &error);
    EXPECT_TRUE(error.empty());
    ASSERT_EQ(models.size(), 6u);
    EXPECT_EQ(models[0], "grok-4.5");
    EXPECT_EQ(models[1], "grok-composer-2.5-fast");
    EXPECT_EQ(models[2], "future-model");
    EXPECT_EQ(models[3], "meta-model");
    EXPECT_EQ(models[4], "meta-id-model");
    EXPECT_EQ(models[5], "grok-code-fast-1");
}
