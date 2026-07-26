#include <gtest/gtest.h>

#include "remote_control/channel_plugin.hpp"

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>
#include <vector>

using acecode::HookCommandSpec;
using acecode::HookProcessResult;
using acecode::rc::ChannelActivationRequest;
using acecode::rc::ChannelPluginHost;
using acecode::rc::ChannelPluginManifest;
using acecode::rc::ChannelPluginStatus;

TEST(ChannelPluginManifest, ParsesStdioLauncher) {
    const auto j = nlohmann::json{
        {"schema", "acecode.channel-plugin.v1"},
        {"name", "chat"},
        {"transport", "stdio"},
        {"launcher",
         nlohmann::json{
             {"command", "chat-channel.exe"},
             {"args", nlohmann::json::array({"--profile", "work"})},
             {"cwd", "C:/plugins/chat"},
         }},
        {"timeout_ms", 15000},
    };

    ChannelPluginManifest manifest;
    std::string error;
    ASSERT_TRUE(acecode::rc::parse_channel_plugin_manifest_json(j, &manifest, &error))
        << error;
    EXPECT_EQ(manifest.name, "chat");
    EXPECT_EQ(manifest.transport, "stdio");
    EXPECT_EQ(manifest.command, "chat-channel.exe");
    ASSERT_EQ(manifest.args.size(), 2u);
    EXPECT_EQ(manifest.args[0], "--profile");
    EXPECT_EQ(manifest.args[1], "work");
    EXPECT_EQ(manifest.cwd, "C:/plugins/chat");
    EXPECT_EQ(manifest.timeout_ms, 15000);
}

TEST(ChannelPluginManifest, RejectsUnsupportedManifest) {
    const auto j = nlohmann::json{
        {"schema", "acecode.channel-plugin.v9"},
        {"name", "chat"},
        {"transport", "stdio"},
        {"command", "chat-channel.exe"},
    };

    ChannelPluginManifest manifest;
    std::string error;
    EXPECT_FALSE(acecode::rc::parse_channel_plugin_manifest_json(j, &manifest, &error));
    EXPECT_NE(error.find("unsupported"), std::string::npos);
}

TEST(ChannelActivationRequest, BuildsNeutralPayload) {
    ChannelActivationRequest request;
    request.session_id = "session-1";
    request.inbound_url = "http://127.0.0.1:28190/rc/send";
    request.token = "tok";
    request.settings = nlohmann::json{{"profile", "work"}};

    const auto j = acecode::rc::channel_activation_request_to_json(request);
    EXPECT_EQ(j["type"], "channel.activate");
    EXPECT_EQ(j["protocol_version"], 1);
    EXPECT_EQ(j["session_id"], "session-1");
    EXPECT_EQ(j["inbound"]["url"], "http://127.0.0.1:28190/rc/send");
    EXPECT_EQ(j["inbound"]["token_header"], "X-ACECode-RC-Token");
    EXPECT_EQ(j["inbound"]["token"], "tok");
    EXPECT_EQ(j["outbound"]["preferred"], "webhook");
    EXPECT_EQ(j["settings"]["profile"], "work");
}

TEST(ChannelPluginStatus, ParsesConnectedWebhookStatus) {
    const auto j = nlohmann::json{
        {"type", "channel.status"},
        {"state", "connected"},
        {"already_running", true},
        {"binding_token", "binding-123"},
        {"future_extension", nlohmann::json{{"ignored", true}}},
        {"outbound",
         nlohmann::json{
             {"mode", "webhook"},
             {"url", "http://127.0.0.1:39001/messages"},
         }},
    };

    ChannelPluginStatus status;
    std::string error;
    ASSERT_TRUE(acecode::rc::parse_channel_plugin_status_json(j, &status, &error))
        << error;
    EXPECT_TRUE(status.connected());
    EXPECT_TRUE(status.already_running);
    EXPECT_EQ(status.outbound_mode, "webhook");
    EXPECT_EQ(status.outbound_url, "http://127.0.0.1:39001/messages");
    ASSERT_TRUE(status.binding_token.has_value());
    EXPECT_EQ(*status.binding_token, "binding-123");
}

TEST(ChannelPluginStatus, AcceptsLegacyStatusWithoutBindingToken) {
    const auto j = nlohmann::json{
        {"type", "channel.status"},
        {"state", "connected"},
        {"outbound",
         nlohmann::json{
             {"mode", "webhook"},
             {"url", "http://127.0.0.1:39001/messages"},
         }},
    };

    ChannelPluginStatus status;
    std::string error;
    ASSERT_TRUE(acecode::rc::parse_channel_plugin_status_json(j, &status, &error))
        << error;
    EXPECT_FALSE(status.binding_token.has_value());
}

TEST(ChannelPluginStatus, RejectsInvalidBindingToken) {
    for (const auto& invalid : {
             nlohmann::json(""),
             nlohmann::json(nullptr),
             nlohmann::json(7),
             nlohmann::json::object(),
         }) {
        const auto j = nlohmann::json{
            {"type", "channel.status"},
            {"state", "connected"},
            {"binding_token", invalid},
        };
        ChannelPluginStatus status;
        std::string error;
        EXPECT_FALSE(acecode::rc::parse_channel_plugin_status_json(
            j, &status, &error));
        EXPECT_NE(error.find("binding_token"), std::string::npos);
    }
}

TEST(ChannelDeactivationRequest, IncludesTokenOnlyForTokenAwareBinding) {
    const auto token_aware =
        acecode::rc::channel_deactivation_request_to_json(
            "session-1", "binding-123");
    EXPECT_EQ(token_aware["type"], "channel.deactivate");
    EXPECT_EQ(token_aware["protocol_version"], 1);
    EXPECT_EQ(token_aware["session_id"], "session-1");
    EXPECT_EQ(token_aware["binding_token"], "binding-123");

    const auto legacy =
        acecode::rc::channel_deactivation_request_to_json("session-1");
    EXPECT_EQ(legacy, nlohmann::json({
                          {"type", "channel.deactivate"},
                          {"protocol_version", 1},
                          {"session_id", "session-1"},
                      }));
}

TEST(ChannelPluginHost, ActivatesAndAcceptsAlreadyRunningRuntime) {
    ChannelPluginManifest manifest;
    manifest.name = "chat";
    manifest.command = "chat-channel.exe";
    manifest.args = {"--stdio"};
    manifest.cwd = "C:/plugins/chat";

    std::string seen_stdin;
    HookCommandSpec seen_command;
    int seen_timeout = 0;
    std::string seen_cwd;
    ChannelPluginHost host([&](const HookCommandSpec& command,
                               const std::string& stdin_text,
                               int timeout_ms,
                               const std::string& cwd) {
        seen_command = command;
        seen_stdin = stdin_text;
        seen_timeout = timeout_ms;
        seen_cwd = cwd;
        HookProcessResult result;
        result.started = true;
        result.exit_code = 0;
        result.stdout_text =
            R"({"type":"channel.status","state":"connected","already_running":true,"binding_token":"binding-123","outbound":{"mode":"webhook","url":"http://127.0.0.1:39001/messages"}})";
        return result;
    });

    ChannelActivationRequest request;
    request.session_id = "session-1";
    request.inbound_url = "http://127.0.0.1:28190/rc/send";
    request.token = "tok";
    request.settings = nlohmann::json{{"profile", "work"}};

    std::string error;
    const auto activation = host.activate(manifest, request, 12345, &error);
    ASSERT_TRUE(activation.ok) << error;
    EXPECT_TRUE(activation.status.already_running);
    EXPECT_EQ(activation.status.outbound_url, "http://127.0.0.1:39001/messages");
    ASSERT_TRUE(activation.status.binding_token.has_value());
    EXPECT_EQ(*activation.status.binding_token, "binding-123");
    EXPECT_EQ(seen_command.command, "chat-channel.exe");
    ASSERT_EQ(seen_command.args.size(), 1u);
    EXPECT_EQ(seen_command.args[0], "--stdio");
    EXPECT_EQ(seen_timeout, 12345);
    EXPECT_EQ(seen_cwd, "C:/plugins/chat");

    const auto sent = nlohmann::json::parse(seen_stdin);
    EXPECT_EQ(sent["type"], "channel.activate");
    EXPECT_EQ(sent["session_id"], "session-1");
    EXPECT_EQ(sent["settings"]["profile"], "work");
}

TEST(ChannelPluginHost, ReportsPluginFailure) {
    ChannelPluginManifest manifest;
    manifest.name = "chat";
    manifest.command = "chat-channel.exe";

    ChannelPluginHost host([](const HookCommandSpec&,
                              const std::string&,
                              int,
                              const std::string&) {
        HookProcessResult result;
        result.started = true;
        result.exit_code = 0;
        result.stdout_text =
            R"({"type":"channel.status","state":"failed","message":"login required"})";
        return result;
    });

    ChannelActivationRequest request;
    request.session_id = "session-1";
    request.inbound_url = "http://127.0.0.1:28190/rc/send";
    request.token = "tok";

    std::string error;
    const auto activation = host.activate(manifest, request, 10000, &error);
    EXPECT_FALSE(activation.ok);
    EXPECT_NE(error.find("login required"), std::string::npos);
}

TEST(ChannelPluginHost, DeactivationEchoesCurrentBindingToken) {
    ChannelPluginManifest manifest;
    manifest.name = "chat";
    manifest.command = "chat-channel.exe";

    std::vector<nlohmann::json> requests;
    ChannelPluginHost host([&](const HookCommandSpec&,
                               const std::string& stdin_text,
                               int,
                               const std::string&) {
        requests.push_back(nlohmann::json::parse(stdin_text));
        HookProcessResult result;
        result.started = true;
        result.exit_code = 0;
        return result;
    });

    std::string error;
    EXPECT_TRUE(host.deactivate(
        manifest, "session-1", "binding-123", 10000, &error)) << error;
    EXPECT_TRUE(host.deactivate(
        manifest, "session-legacy", 10000, &error)) << error;

    ASSERT_EQ(requests.size(), 2u);
    EXPECT_EQ(requests[0]["binding_token"], "binding-123");
    EXPECT_EQ(requests[0]["session_id"], "session-1");
    EXPECT_EQ(requests[1], nlohmann::json({
                               {"type", "channel.deactivate"},
                               {"protocol_version", 1},
                               {"session_id", "session-legacy"},
                           }));
}

TEST(ChannelPluginHost, RedactsBindingTokenFromDeactivationFailures) {
    ChannelPluginManifest manifest;
    manifest.name = "chat";
    manifest.command = "chat-channel.exe";

    const std::string token = R"(binding-"secret\line)";
    const std::string encoded = nlohmann::json(token).dump();
    const std::string escaped =
        encoded.substr(1, encoded.size() - 2);
    auto expect_redacted =
        [&](ChannelPluginHost::Runner runner,
            const std::string& diagnostic_context) {
            ChannelPluginHost host(std::move(runner));
            std::string error;
            EXPECT_FALSE(host.deactivate(
                manifest, "session-1", token, 10000, &error));
            EXPECT_EQ(error.find(token), std::string::npos) << error;
            EXPECT_EQ(error.find(escaped), std::string::npos) << error;
            EXPECT_NE(error.find("[binding token redacted]"),
                      std::string::npos)
                << error;
            EXPECT_NE(error.find(diagnostic_context), std::string::npos)
                << error;
        };

    expect_redacted(
        [&](const HookCommandSpec&, const std::string&, int,
            const std::string&) {
            HookProcessResult result;
            result.error = "runner launch rejected token " + token;
            return result;
        },
        "runner launch rejected");

    expect_redacted(
        [&](const HookCommandSpec&, const std::string&, int,
            const std::string&) {
            HookProcessResult result;
            result.started = true;
            result.exit_code = 7;
            result.stderr_text = "stderr rejected token " + token;
            return result;
        },
        "stderr rejected");

    expect_redacted(
        [](const HookCommandSpec&, const std::string& stdin_text, int,
           const std::string&) {
            HookProcessResult result;
            result.started = true;
            result.exit_code = 8;
            result.output = "request echo: " + stdin_text;
            return result;
        },
        "channel.deactivate");

    expect_redacted(
        [&](const HookCommandSpec&, const std::string&, int,
            const std::string&) {
            HookProcessResult result;
            result.started = true;
            result.exit_code = 0;
            result.stdout_text =
                nlohmann::json{
                    {"type", "channel.status"},
                    {"state", "failed"},
                    {"message", "status rejected token " + token},
                }.dump();
            return result;
        },
        "status rejected");

    expect_redacted(
        [&](const HookCommandSpec&, const std::string&, int,
            const std::string&) -> HookProcessResult {
            throw std::runtime_error(
                "runner exception echoed token " + token);
        },
        "runner exception echoed");

    expect_redacted(
        [&](const HookCommandSpec&, const std::string&, int,
            const std::string&) {
            HookProcessResult result;
            result.started = true;
            result.exit_code = 0;
            result.stdout_text =
                nlohmann::json{
                    {"type", "channel.status"},
                    {"state", token},
                }.dump();
            return result;
        },
        "unsupported channel status state");
}

TEST(ChannelPluginHost, PreservesLegacyDeactivationDiagnostic) {
    ChannelPluginManifest manifest;
    manifest.name = "chat";
    manifest.command = "chat-channel.exe";

    ChannelPluginHost host(
        [](const HookCommandSpec&, const std::string&, int,
           const std::string&) {
            HookProcessResult result;
            result.error = "legacy detach rejected: retry manually";
            return result;
        });

    std::string error;
    EXPECT_FALSE(host.deactivate(
        manifest, "session-legacy", 10000, &error));
    EXPECT_EQ(error,
              "failed to start channel plugin: "
              "legacy detach rejected: retry manually");
    EXPECT_EQ(error.find("redacted"), std::string::npos);
}
