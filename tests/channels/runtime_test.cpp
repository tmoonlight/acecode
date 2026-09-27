#include "channels/runtime.hpp"
#include "channels/setup.hpp"
#include "test_support/channels/test_support.hpp"
#include "daemon/cli.hpp"
#include "utils/paths.hpp"
#include <cpr/cpr.h>
#include <fstream>
#include <thread>

namespace acecode::channels {
TEST(ChannelRuntime, DaemonStatusHonorsExplicitRuntimeDirectory) {
    const auto dir = test::temporary("daemon-status");
    const auto previous = get_run_dir_override();
    EXPECT_EQ(daemon::cli::run({"status", "--run-dir=" + path_to_utf8(dir)}, ""), 1);
    EXPECT_EQ(get_run_dir_override(), path_to_utf8(dir));
    set_run_dir_override(previous);
}
TEST(ChannelRuntime, ExclusiveLockReleasesForStandbyTakeover) {
    const auto dir = test::temporary("lock");
    OwnerLock first, second;
    EXPECT_TRUE(first.acquire(dir / "owner.lock"));
    EXPECT_FALSE(second.acquire(dir / "owner.lock"));
    first.release(); EXPECT_TRUE(second.acquire(dir / "owner.lock"));
    second.release(); std::error_code ec; std::filesystem::remove_all(dir, ec);
}
TEST(ChannelRuntime, AuthenticatedControlAndOwnerTakeover) {
    const auto dir = test::temporary("runtime");
    State saved(dir); saved.set_enabled(true);
    test::Client client;
    GatewayDeps deps{client};
    deps.permissions = [](const std::string&) { return std::vector<Json>{}; };
    deps.transcript = [](const std::string&) { return Json::array(); };
    deps.session_cwd = [dir](const std::string&) { return path_to_utf8(dir); };
    Runtime first(deps, dir, test::fake_bridge), second(deps, dir, test::fake_bridge);
    auto wait_owner = [&] {
        for (int i = 0; i < 100; ++i) {
            try {
                const auto value = request_control({{"op", "status"}}, dir);
                if (value.value("host_running", false)) return value;
            } catch (...) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return Json::object();
    };
    first.start();
    const auto status = wait_owner();
    ASSERT_TRUE(status.contains("enabled"));
    EXPECT_TRUE(status["enabled"].get<bool>());
    std::ifstream file(dir / "owner.json"); const auto owner = Json::parse(file);
    file.close();
    auto response = cpr::Post(cpr::Url{"http://127.0.0.1:" + std::to_string(owner.at("port").get<int>()) + "/channels"},
        cpr::Body{R"({"op":"on"})"}, cpr::Proxies{{"http", ""}, {"https", ""}}, cpr::Timeout{2000});
    EXPECT_EQ(response.status_code, 403);
    EXPECT_NO_THROW(request_control({{"op", "on"}}, dir));
    second.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    std::ifstream still_file(dir / "owner.json");
    EXPECT_EQ(Json::parse(still_file).at("token"), owner.at("token"));
    still_file.close();
    first.stop();
    const auto takeover = wait_owner();
    ASSERT_TRUE(takeover.contains("enabled"));
    EXPECT_TRUE(takeover["enabled"].get<bool>());
    EXPECT_NO_THROW(request_control({{"op", "off"}}, dir));
    second.stop();
    EXPECT_FALSE(std::filesystem::exists(dir / "owner.json"));
    std::error_code ec; std::filesystem::remove_all(dir, ec);
}
TEST(ChannelRuntime, FirstStartedHostAloneAutoConnectsAndStandbyTakesOverAfterExit) {
    const auto dir = test::temporary("first-runtime");
    State saved(dir); saved.enable_with_access("100@s.whatsapp.net", {"100@s.whatsapp.net"});
    test::Client client;
    GatewayDeps deps{client};
    std::atomic<int> first_spawns{0}, second_spawns{0};
    Runtime first(deps, dir, [&](const std::filesystem::path& path) { ++first_spawns; return test::fake_bridge(path); });
    Runtime second(deps, dir, [&](const std::filesystem::path& path) { ++second_spawns; return test::fake_bridge(path); });
    auto wait_connected = [&] {
        for (int i = 0; i < 100; ++i) {
            try { if (request_control({{"op", "status"}}, dir).value("state", "") == "connected") return true; } catch (...) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return false;
    };
    first.start(); second.start(); // No readiness delay: ownership is claimed synchronously.
    ASSERT_TRUE(wait_connected());
    EXPECT_EQ(first_spawns, 1);
    EXPECT_EQ(second_spawns, 0);
    EXPECT_THROW(request_control({{"op", "setup_begin"}}, dir), std::exception);
    auto setup = default_setup_dependencies(dir, test::fake_bridge);
    EXPECT_NO_THROW(setup.begin());
    setup.close();
    EXPECT_EQ(request_control({{"op", "status"}}, dir).at("state"), "connected");
    first.stop();
    ASSERT_TRUE(wait_connected());
    EXPECT_EQ(first_spawns, 1);
    EXPECT_EQ(second_spawns, 1);
    second.stop();
    EXPECT_FALSE(request_control({{"op", "status"}}, dir).at("host_running").get<bool>());
    EXPECT_FALSE(std::filesystem::exists(dir / "owner.json"));
    setup = {};
    std::error_code ec; std::filesystem::remove_all(dir, ec);
}

TEST(ChannelRuntime, DisabledInstanceIgnoresLaterConfigurationAndDoesNotBlockNewHosts) {
    const auto dir = test::temporary("setup-runtime");
    auto setup = default_setup_dependencies(dir, test::fake_bridge);
    setup.begin(); setup.connect();
    test::Client client;
    std::atomic<int> old_spawns{0}, new_spawns{0};
    Runtime old(GatewayDeps{client}, dir, [&](const std::filesystem::path& path) { ++old_spawns; return test::fake_bridge(path); });
    Runtime next(GatewayDeps{client}, dir, [&](const std::filesystem::path& path) { ++new_spawns; return test::fake_bridge(path); });
    old.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    EXPECT_EQ(old_spawns, 0);
    EXPECT_FALSE(std::filesystem::exists(dir / "owner.json"));
    setup.save("100@s.whatsapp.net", {"100@s.whatsapp.net"}); setup.close();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    EXPECT_EQ(old_spawns, 0);
    EXPECT_FALSE(std::filesystem::exists(dir / "owner.json"));
    next.start();
    bool connected = false;
    for (int i = 0; i < 100; ++i) {
        try { connected = request_control({{"op", "status"}}, dir).value("state", "") == "connected"; } catch (...) {}
        if (connected) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    EXPECT_TRUE(connected);
    EXPECT_EQ(old_spawns, 0);
    EXPECT_EQ(new_spawns, 1);
    EXPECT_TRUE(client.creates.empty());
    next.stop(); old.stop(); setup = {};
    std::error_code ec; std::filesystem::remove_all(dir, ec);
}

TEST(ChannelRuntime, RunningAndStandbyInstancesKeepStartupSettingsUntilNewInstanceStarts) {
    const auto dir = test::temporary("startup-snapshot");
    const std::string account = "100@s.whatsapp.net", contact = "200@s.whatsapp.net";
    State saved(dir); saved.enable_with_access(account, {account});
    std::filesystem::create_directories(dir / "auth");
    std::ofstream(dir / "auth" / "creds.json") << Json{{"me", {{"id", account}}}}.dump();
    test::Client client;
    GatewayDeps deps{client};
    std::atomic<int> first_spawns{0}, standby_spawns{0}, next_spawns{0};
    Runtime first(deps, dir, [&](const std::filesystem::path& path) { ++first_spawns; return test::fake_bridge(path); });
    Runtime standby(deps, dir, [&](const std::filesystem::path& path) { ++standby_spawns; return test::fake_bridge(path); });
    Runtime next(deps, dir, [&](const std::filesystem::path& path) { ++next_spawns; return test::fake_bridge(path); });
    auto wait_connected = [&] {
        for (int i = 0; i < 100; ++i) {
            try {
                auto value = request_control({{"op", "status"}}, dir);
                if (value.value("state", "") == "connected") return value;
            } catch (...) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return Json::object();
    };
    first.start(); standby.start();
    auto original = wait_connected();
    ASSERT_TRUE(original.contains("access"));
    auto setup = default_setup_dependencies(dir, [](const std::filesystem::path& path) {
        ADD_FAILURE() << "Saved-login configuration must not start a bridge";
        return test::fake_bridge(path);
    });
    std::atomic<bool> cancel{false};
    SetupUpdate result;
    run_setup({contact}, setup, cancel, [&](const SetupUpdate& update) { result = update; });
    EXPECT_EQ(result.phase, SetupPhase::Complete) << result.detail;
    EXPECT_EQ(request_control({{"op", "status"}}, dir).at("access"), original.at("access"));
    EXPECT_EQ(first_spawns, 1);
    EXPECT_EQ(standby_spawns, 0);
    first.stop();
    const auto takeover = wait_connected();
    ASSERT_TRUE(takeover.contains("access"));
    EXPECT_EQ(takeover.at("access"), original.at("access"));
    EXPECT_EQ(standby_spawns, 1);
    standby.stop();
    next.start();
    const auto updated = wait_connected();
    ASSERT_TRUE(updated.contains("access"));
    EXPECT_EQ(updated.at("access").at(account), Json::array({account, contact}));
    EXPECT_EQ(next_spawns, 1);
    next.stop(); setup = {};
    std::error_code ec; std::filesystem::remove_all(dir, ec);
}

TEST(ChannelRuntime, SavedConfigurationStatusDoesNotLaunchAHost) {
    const auto dir = test::temporary("offline-status");
    State saved(dir); saved.enable_with_access("100@s.whatsapp.net", {"100@s.whatsapp.net"});
    const auto status = request_control({{"op", "status"}}, dir);
    EXPECT_TRUE(status.at("enabled").get<bool>());
    EXPECT_FALSE(status.at("host_running").get<bool>());
    EXPECT_NE(status.at("state").get<std::string>().find("waiting for daemon/Desktop"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(dir / "owner.json"));
    EXPECT_FALSE(std::filesystem::exists(dir / "run"));
    EXPECT_THROW(request_control({{"op", "on"}}, dir), std::exception);
    std::error_code ec; std::filesystem::remove_all(dir, ec);
}
} // namespace acecode::channels
