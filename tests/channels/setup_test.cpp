#include "channels/setup.hpp"
#include "channels/bridge.hpp"
#include "channels/runtime.hpp"
#include "platform/process/os_process.hpp"
#include "test_support/channels/test_support.hpp"
#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace acecode::channels {
TEST(ChannelSetup, NormalizesPhoneNumbersAndRejectsProtocolAndShellInput) {
    EXPECT_EQ(setup_contacts("+1 (202) 555-0123, +886 912345678;12025550123\n"),
              (std::vector<std::string>{"12025550123@s.whatsapp.net", "886912345678@s.whatsapp.net"}));
    EXPECT_TRUE(setup_contacts("  ").empty());
    EXPECT_THROW(setup_contacts("123"), std::exception);
    EXPECT_THROW(setup_contacts("1234567890123456"), std::exception);
    EXPECT_THROW(setup_contacts("12345@s.whatsapp.net"), std::exception);
    EXPECT_THROW(setup_contacts("$(whoami)"), std::exception);
    EXPECT_THROW(setup_contacts(std::string(4097, '1')), std::exception);
}
TEST(ChannelSetup, RequiresSupportedNodeMajorVersion) {
    EXPECT_TRUE(supported_node_version("v22.0.0\r\n"));
    EXPECT_TRUE(supported_node_version("v24.3.0\n"));
    EXPECT_FALSE(supported_node_version("v20.19.0"));
    EXPECT_FALSE(supported_node_version("22.0.0"));
    EXPECT_FALSE(supported_node_version("Node.js failed"));
}
namespace {
struct SetupHarness {
    std::atomic<bool> cancel{false};
    SetupDependencies deps;
    std::vector<std::string> commands;
    std::vector<std::string> saved_peers;
    std::vector<SetupUpdate> updates;
    std::string state = "pairing";
    int polls = 0;
    bool fail_install = false, fail_finish = false;
    SetupHarness() {
        deps.begin = [this] { commands.push_back("begin"); };
        deps.connect = [this] { commands.push_back("connect"); };
        deps.status = [this]() -> Json {
            commands.push_back("status");
            ++polls;
            return {{"state", state}, {"account", "12025550123@s.whatsapp.net"}, {"qr_text", "qr-" + std::to_string(polls)}};
        };
        deps.save = [this](const std::string& account, const std::vector<std::string>& peers) {
            commands.push_back("save");
            EXPECT_EQ(account, "12025550123@s.whatsapp.net");
            if (fail_finish) throw std::runtime_error("Account changed");
            saved_peers = peers;
        };
        deps.close = [this] { commands.push_back("close"); };
        deps.prepare = [this](const std::atomic<bool>&, const SetupProgress& report) {
            report({SetupPhase::Installing});
            if (fail_install) throw std::runtime_error("npm failed");
        };
        deps.wait = [this] { if (polls >= 2) state = "connected"; };
    }
    void run() {
        run_setup({"886912345678@s.whatsapp.net"}, deps, cancel,
                  [this](const SetupUpdate& update) {
                      if (update.phase == SetupPhase::Complete) EXPECT_EQ(commands.back(), "close");
                      updates.push_back(update);
                  });
    }
    bool called(const std::string& op) const {
        return std::find(commands.begin(), commands.end(), op) != commands.end();
    }
};
}
TEST(ChannelSetup, InstallsRefreshesQrSavesAccessAndClosesBeforeReportingSuccess) {
    SetupHarness test;
    test.run();
    ASSERT_FALSE(test.updates.empty());
    EXPECT_EQ(test.updates.back().phase, SetupPhase::Complete);
    EXPECT_TRUE(test.called("connect"));
    EXPECT_FALSE(test.called("on"));
    EXPECT_EQ(std::count(test.commands.begin(), test.commands.end(), "close"), 1);
    EXPECT_EQ(test.saved_peers, (std::vector<std::string>{"886912345678@s.whatsapp.net", "12025550123@s.whatsapp.net"}));
    std::vector<std::string> qr;
    for (const auto& update : test.updates) if (!update.qr_text.empty()) qr.push_back(update.qr_text);
    EXPECT_EQ(qr, (std::vector<std::string>{"qr-1", "qr-2"}));
    EXPECT_TRUE(test.updates.back().qr_text.empty());
}
TEST(ChannelSetup, ExistingLoginSkipsQrAndStillSavesAccess) {
    SetupHarness test;
    test.state = "connected";
    test.run();
    EXPECT_EQ(test.updates.back().phase, SetupPhase::Complete);
    EXPECT_EQ(test.polls, 1);
}
TEST(ChannelSetup, InstallationFailureClosesLocalSetupWithoutSaving) {
    SetupHarness test;
    test.fail_install = true;
    test.run();
    EXPECT_EQ(test.updates.back().phase, SetupPhase::Failed);
    EXPECT_EQ(test.updates.back().detail, "npm failed");
    EXPECT_TRUE(test.called("close"));
    EXPECT_FALSE(test.called("connect"));
    EXPECT_FALSE(test.called("save"));
}
TEST(ChannelSetup, CancelBeforeStartDoesNotAcquireAccountOrStartBridge) {
    SetupHarness test;
    test.cancel = true;
    test.run();
    EXPECT_EQ(test.updates.back().phase, SetupPhase::Cancelled);
    EXPECT_TRUE(test.commands.empty());
}
TEST(ChannelSetup, CancelDuringPairingCleansUpWithoutEnabling) {
    SetupHarness test;
    test.deps.wait = [&] { test.cancel = true; };
    test.run();
    EXPECT_EQ(test.updates.back().phase, SetupPhase::Cancelled);
    EXPECT_TRUE(test.called("close"));
    EXPECT_FALSE(test.called("save"));
}
TEST(ChannelSetup, CancelDuringPreparationDoesNotConnect) {
    SetupHarness test;
    test.deps.prepare = [&](const std::atomic<bool>&, const SetupProgress&) { test.cancel = true; };
    test.run();
    EXPECT_EQ(test.updates.back().phase, SetupPhase::Cancelled);
    EXPECT_TRUE(test.called("close"));
    EXPECT_FALSE(test.called("connect"));
}
TEST(ChannelSetup, TimeoutCancelsAndOffersRetry) {
    SetupHarness test;
    test.deps.pairing_timeout = std::chrono::milliseconds(0);
    test.run();
    EXPECT_EQ(test.updates.back().phase, SetupPhase::Failed);
    EXPECT_NE(test.updates.back().detail.find("timed out"), std::string::npos);
    EXPECT_TRUE(test.called("close"));
    EXPECT_FALSE(test.called("save"));
}
TEST(ChannelSetup, FailedFinishIsNotReportedAsSuccessOrRetried) {
    SetupHarness test;
    test.state = "connected"; test.fail_finish = true;
    test.run();
    EXPECT_EQ(test.updates.back().phase, SetupPhase::Failed);
    EXPECT_EQ(std::count(test.commands.begin(), test.commands.end(), "save"), 1);
    EXPECT_TRUE(test.called("close"));
}
TEST(ChannelSetup, DisconnectedErrorHasActionableFallback) {
    SetupHarness test;
    test.state = "error";
    test.run();
    EXPECT_EQ(test.updates.back().phase, SetupPhase::Failed);
    EXPECT_FALSE(test.updates.back().detail.empty());
}
TEST(ChannelSetup, RealDependencyInstallationIsOptionalAndUsesTemporaryState) {
    if (!std::getenv("ACECODE_TEST_CHANNEL_INSTALL")) GTEST_SKIP() << "Opt-in network-dependent npm installation";
    const auto dir = test::temporary("install");
    std::atomic<bool> cancel{false};
    bool installing = false;
    EXPECT_NO_THROW(install_whatsapp_dependencies(dir, cancel, [&](const SetupUpdate& value) {
        installing |= value.phase == SetupPhase::Installing;
    }));
    EXPECT_TRUE(installing);
    EXPECT_TRUE(whatsapp_dependencies_ready(dir / "bridge"));
    EXPECT_FALSE(std::filesystem::exists(dir / "auth"));
    EXPECT_FALSE(std::filesystem::exists(dir / "state.json"));
    installing = false;
    EXPECT_NO_THROW(install_whatsapp_dependencies(dir, cancel, [&](const SetupUpdate& value) {
        installing |= value.phase == SetupPhase::Installing;
    }));
    EXPECT_FALSE(installing);
    std::error_code ec; std::filesystem::remove_all(dir, ec);
}

TEST(ChannelSetup, LocalPairingUsesIsolatedProfileAndStopsChildBeforeCompletionWithoutHosting) {
    const auto dir = test::temporary("local-setup");
    int spawns = 0;
    auto deps = default_setup_dependencies(dir, [&](const std::filesystem::path& path) {
        ++spawns;
        EXPECT_NE(path, dir);
        EXPECT_EQ(path.parent_path(), dir / "profiles");
        auto options = test::fake_bridge(path);
        options.argv.insert(options.argv.end(), {"--pid-file", path_to_utf8(dir / "bridge-pid.json")});
        return options;
    });
    deps.prepare = [](const std::atomic<bool>&, const SetupProgress&) {};
    std::atomic<bool> cancel{false};
    bool complete = false;
    run_setup({"200@s.whatsapp.net"}, deps, cancel, [&](const SetupUpdate& update) {
        if (update.phase != SetupPhase::Complete) return;
        complete = true;
        EXPECT_FALSE(std::filesystem::exists(dir / "owner.lock"));
        EXPECT_THROW(deps.status(), std::exception);
        std::ifstream file(dir / "bridge-pid.json");
        const auto child = Json::parse(file);
        EXPECT_FALSE(daemon::is_pid_alive(child.at("pid").get<std::int64_t>()));
        const auto args = child.at("argv").get<std::vector<std::string>>();
        EXPECT_NE(std::find(args.begin(), args.end(), "--setup-only"), args.end());
    });
    EXPECT_TRUE(complete);
    EXPECT_EQ(spawns, 1);
    State saved(dir); saved.load();
    EXPECT_TRUE(saved.enabled());
    EXPECT_TRUE(saved.allowed({"100@s.whatsapp.net", "100@s.whatsapp.net", "100@s.whatsapp.net", false}, true));
    EXPECT_TRUE(saved.allowed({"100@s.whatsapp.net", "200@s.whatsapp.net", "200@s.whatsapp.net", false}, true));
    EXPECT_FALSE(std::filesystem::exists(dir / "owner.json"));
    EXPECT_FALSE(std::filesystem::exists(dir / "run"));
    EXPECT_TRUE(saved.snapshot().at("bindings").empty());
    EXPECT_NE(saved.transport_directory(), dir);
    EXPECT_TRUE(std::filesystem::exists(saved.transport_directory()));
    deps = {};
    std::error_code ec; std::filesystem::remove_all(dir, ec);
}

TEST(ChannelSetup, SavedLegacyLoginConfiguresWhileOwnerIsBusyWithoutStartingOrContactingAnything) {
    const auto dir = test::temporary("setup-busy");
    auto legacy = State(dir).snapshot();
    legacy.erase("profile");
    legacy["enabled"] = true;
    legacy["access"]["100@s.whatsapp.net"] = Json::array({"100@s.whatsapp.net"});
    const Json credentials{{"me", {{"id", "100:12@s.whatsapp.net"}}}, {"registered", true}};
    std::filesystem::create_directories(dir / "auth");
    std::ofstream(dir / "state.json") << legacy.dump();
    std::ofstream(dir / "auth" / "creds.json") << credentials.dump();
    std::ofstream(dir / "owner.json") << "Do not inspect this owner descriptor";
    OwnerLock owner;
    ASSERT_TRUE(owner.acquire(dir / "owner.lock"));
    int spawns = 0;
    auto deps = default_setup_dependencies(dir, [&](const std::filesystem::path& path) {
        ++spawns; return test::fake_bridge(path);
    });
    std::atomic<bool> cancel{false};
    SetupUpdate result;
    run_setup({"200@s.whatsapp.net"}, deps, cancel, [&](const SetupUpdate& update) {
        EXPECT_NE(update.phase, SetupPhase::Installing);
        EXPECT_NE(update.phase, SetupPhase::Pairing);
        result = update;
    });
    EXPECT_EQ(result.phase, SetupPhase::Complete) << result.detail;
    EXPECT_TRUE(result.detail.empty());
    EXPECT_EQ(spawns, 0);
    OwnerLock probe;
    EXPECT_FALSE(probe.acquire(dir / "owner.lock"));
    State saved(dir); saved.load();
    EXPECT_TRUE(saved.allowed({"100@s.whatsapp.net", "200@s.whatsapp.net", "200@s.whatsapp.net", false}, true));
    EXPECT_EQ(saved.transport_directory(), dir);
    std::ifstream history(dir / "state.json"), auth(dir / "auth" / "creds.json"), descriptor(dir / "owner.json");
    EXPECT_EQ(Json::parse(history), legacy);
    EXPECT_EQ(Json::parse(auth), credentials);
    std::string text; std::getline(descriptor, text);
    EXPECT_EQ(text, "Do not inspect this owner descriptor");
    history.close(); auth.close(); descriptor.close();
    EXPECT_FALSE(std::filesystem::exists(dir / "profiles"));
    EXPECT_FALSE(std::filesystem::exists(dir / "bridge"));
    owner.release(); deps = {};
    std::error_code ec; std::filesystem::remove_all(dir, ec);
}

TEST(ChannelSetup, LocalPairingValidatesAccountAndRetainsExistingAccess) {
    const auto dir = test::temporary("setup-verified");
    State saved(dir); saved.enable_with_access("100@s.whatsapp.net", {"300@s.whatsapp.net"});
    const auto before = saved.snapshot();
    auto deps = default_setup_dependencies(dir, test::fake_bridge);
    deps.begin(); deps.connect();
    EXPECT_THROW(deps.save("200@s.whatsapp.net", {"200@s.whatsapp.net"}), std::exception);
    saved.load(); EXPECT_EQ(saved.snapshot(), before);
    EXPECT_NO_THROW(deps.save("100@s.whatsapp.net", {"100@s.whatsapp.net", "200@s.whatsapp.net"}));
    deps.close();
    saved.load();
    EXPECT_TRUE(saved.allowed({"100@s.whatsapp.net", "300@s.whatsapp.net", "300@s.whatsapp.net", false}, true));
    EXPECT_TRUE(saved.allowed({"100@s.whatsapp.net", "200@s.whatsapp.net", "200@s.whatsapp.net", false}, true));
    deps = {};
    std::error_code ec; std::filesystem::remove_all(dir, ec);
}

TEST(ChannelSetup, SavedProfileLoginSkipsPairingAndKeepsTheSelectedProfile) {
    const auto dir = test::temporary("saved-profile");
    State saved(dir);
    saved.enable_with_access("100@s.whatsapp.net", {"100@s.whatsapp.net"}, std::string(32, 'a'));
    const auto profile = saved.transport_directory();
    std::filesystem::create_directories(profile / "auth");
    std::ofstream(profile / "auth" / "creds.json") << R"({"me":{"id":"100_0:3@c.us"}})";
    auto deps = default_setup_dependencies(dir, [](const std::filesystem::path& path) {
        ADD_FAILURE() << "A saved profile must not start a pairing process";
        return test::fake_bridge(path);
    });
    std::atomic<bool> cancel{false};
    SetupUpdate result;
    run_setup({}, deps, cancel, [&](const SetupUpdate& update) { result = update; });
    EXPECT_EQ(result.phase, SetupPhase::Complete) << result.detail;
    EXPECT_EQ(result.account, "100@s.whatsapp.net");
    saved.load(); EXPECT_EQ(saved.transport_directory(), profile);
    EXPECT_FALSE(std::filesystem::exists(dir / "owner.lock"));
    EXPECT_FALSE(std::filesystem::exists(profile / "bridge"));
    deps = {};
    std::error_code ec; std::filesystem::remove_all(dir, ec);
}

TEST(ChannelSetup, NewPairingNeverUsesTheExistingUnlinkedCredentialDirectory) {
    const auto dir = test::temporary("isolated-pairing");
    const Json credentials{{"registered", false}, {"me", nullptr}};
    std::filesystem::create_directories(dir / "auth");
    std::ofstream(dir / "auth" / "creds.json") << credentials.dump();
    OwnerLock owner;
    ASSERT_TRUE(owner.acquire(dir / "owner.lock"));
    std::filesystem::path paired;
    auto deps = default_setup_dependencies(dir, [&](const std::filesystem::path& path) {
        paired = path;
        EXPECT_NE(path, dir);
        return test::fake_bridge(path);
    });
    deps.prepare = [](const std::atomic<bool>&, const SetupProgress&) {};
    std::atomic<bool> cancel{false};
    SetupUpdate result;
    run_setup({}, deps, cancel, [&](const SetupUpdate& update) { result = update; });
    EXPECT_EQ(result.phase, SetupPhase::Complete) << result.detail;
    State saved(dir); saved.load();
    EXPECT_EQ(saved.transport_directory(), paired);
    std::ifstream auth(dir / "auth" / "creds.json");
    EXPECT_EQ(Json::parse(auth), credentials);
    auth.close();
    OwnerLock probe;
    EXPECT_FALSE(probe.acquire(dir / "owner.lock"));
    owner.release(); deps = {};
    std::error_code ec; std::filesystem::remove_all(dir, ec);
}

TEST(ChannelSetup, LocalPairingCancellationAndFailureStopChildAndPreserveConfiguration) {
    for (const bool fail : {false, true}) {
        const auto dir = test::temporary("setup-cleanup");
        State saved(dir); saved.set_enabled(false);
        const auto before = saved.snapshot();
        auto deps = default_setup_dependencies(dir, [&](const std::filesystem::path& path) {
            auto options = test::fake_bridge(path);
            options.argv.insert(options.argv.end(), {"--pid-file", path_to_utf8(dir / "bridge-pid.json")});
            return options;
        });
        deps.prepare = [](const std::atomic<bool>&, const SetupProgress&) {};
        auto real_status = deps.status;
        deps.status = [real_status, fail] {
            auto status = real_status(); status["state"] = fail ? "error" : "pairing"; return status;
        };
        std::atomic<bool> cancel{false};
        deps.wait = [&] { cancel = true; };
        SetupUpdate result;
        run_setup({}, deps, cancel, [&](const SetupUpdate& update) { result = update; });
        EXPECT_EQ(result.phase, fail ? SetupPhase::Failed : SetupPhase::Cancelled);
        std::ifstream file(dir / "bridge-pid.json");
        EXPECT_FALSE(daemon::is_pid_alive(Json::parse(file).at("pid").get<std::int64_t>()));
        file.close();
        saved.load(); EXPECT_EQ(saved.snapshot(), before);
        EXPECT_TRUE(std::filesystem::is_empty(dir / "profiles"));
        OwnerLock probe;
        EXPECT_TRUE(probe.acquire(dir / "owner.lock"));
        probe.release(); deps = {}; real_status = {};
        std::error_code ec; std::filesystem::remove_all(dir, ec);
    }
}
} // namespace acecode::channels
