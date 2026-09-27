#include "channels/bridge.hpp"
#include "test_support.hpp"
#include "test_support/repo_root.hpp"
#include <fstream>
#include <future>
#include <thread>

namespace acecode::channels {
TEST(ChannelBridge, ProcessFramesStatusAndEvents) {
    Bridge bridge; bridge.start(test::fake_bridge({}));
    EXPECT_EQ(bridge.request("status", Json::object())["state"], "disconnected");
    EXPECT_EQ(bridge.request("connect", Json::object())["state"], "connected");
    EXPECT_FALSE(bridge.take_events().empty());
    bridge.stop(); EXPECT_FALSE(bridge.running());
}
TEST(ChannelBridge, TimeoutAndExitResolvePendingRequests) {
    Bridge bridge; bridge.start(test::fake_bridge({}));
    EXPECT_THROW(bridge.request("hang", Json::object(), std::chrono::milliseconds(150)), std::exception);
    EXPECT_FALSE(bridge.running()); bridge.stop();
    bridge.start(test::fake_bridge({}));
    EXPECT_THROW(bridge.request("exit", Json::object()), std::exception);
    bridge.stop();
}
TEST(ChannelBridge, MalformedAndOversizedFramesFailClosed) {
    Bridge bridge; bridge.start(test::fake_bridge({}));
    EXPECT_THROW(bridge.request("malformed", Json::object()), std::exception);
    bridge.stop(); bridge.start(test::fake_bridge({}));
    EXPECT_THROW(bridge.request("oversized", Json::object()), std::exception);
    bridge.stop();
}
TEST(ChannelBridge, ShutdownWakesConcurrentPendingRequest) {
    Bridge bridge; bridge.start(test::fake_bridge({}));
    bridge.request("status", Json::object());
    auto pending = std::async(std::launch::async, [&] {
        try { bridge.request("hang", Json::object()); return false; }
        catch (const std::exception&) { return true; }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50)); bridge.stop();
    EXPECT_EQ(pending.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_TRUE(pending.get());
}
TEST(ChannelBridge, RealBridgeStartsWithoutConnectingAnAccount) {
    const auto script = acecode::test_support::find_repo_root(__FILE__) /
        "assets/channels/whatsapp/bridge.mjs";
    // 搬迁指错路径时以前会静默 SKIP；先验证受版本管理的资源，再判断依赖。
    ASSERT_TRUE(std::filesystem::is_regular_file(script)) << "Bridge script missing: " << script;
    ASSERT_TRUE(std::filesystem::is_regular_file(script.parent_path() / "package.json"))
        << "Bridge package manifest missing: " << script.parent_path();
    const auto dependencies = script.parent_path() / "node_modules";
    if (!std::filesystem::exists(dependencies)) {
        GTEST_SKIP() << "npm ci required in " << script.parent_path();
    }
    ASSERT_TRUE(std::filesystem::is_directory(dependencies))
        << "Bridge dependency path is not a directory: " << dependencies;
    const auto dir = test::temporary("real-bridge");
    Bridge bridge;
    bridge.start({{"node", path_to_utf8(script), "--state-dir", path_to_utf8(dir), "--setup-only"}, {}, {}});
    EXPECT_EQ(bridge.request("status", Json::object())["state"], "disconnected");
    for (const auto* method : {"send", "send_file", "download"}) {
        try {
            bridge.request(method, Json::object());
            ADD_FAILURE() << "Configuration mode must reject " << method;
        } catch (const std::exception& e) {
            EXPECT_NE(std::string(e.what()).find("Configuration mode"), std::string::npos);
        }
    }
    bridge.stop();
    std::error_code ec; std::filesystem::remove_all(dir, ec);
}
TEST(ChannelBridge, MissingDependenciesPreparePrivateAssetsWithoutMutatingPackage) {
    const auto dir = test::temporary("setup");
    try {
        whatsapp_bridge_options(dir);
        FAIL() << "A new private runtime must require guided dependency installation";
    } catch (const std::exception& e) {
        EXPECT_NE(std::string(e.what()).find("acecode channels"), std::string::npos);
    }
    EXPECT_TRUE(std::filesystem::is_regular_file(dir / "bridge" / "bridge.mjs"));
    EXPECT_TRUE(std::filesystem::is_regular_file(dir / "bridge" / "package-lock.json"));
    EXPECT_FALSE(std::filesystem::exists(dir / "auth"));
    std::error_code ec; std::filesystem::remove_all(dir, ec);
}
TEST(ChannelBridge, DependencyVersionsMustMatchPackagedPins) {
    const auto dir = test::temporary("versions");
    EXPECT_THROW(whatsapp_bridge_options(dir), std::exception);
    const auto installed = dir / "bridge";
    std::ifstream manifest(installed / "package.json");
    const auto dependencies = Json::parse(manifest).at("dependencies");
    manifest.close();
    for (const auto& dependency : dependencies.items()) {
        const auto package_dir = installed / "node_modules" / path_from_utf8(dependency.key());
        std::filesystem::create_directories(package_dir);
        std::ofstream(package_dir / "package.json") << Json{{"version", dependency.value()}};
    }
    EXPECT_NO_THROW(whatsapp_bridge_options(dir));
    std::ofstream(installed / "node_modules/@whiskeysockets/baileys/package.json") << R"({"version":"0.0.0"})";
    EXPECT_THROW(whatsapp_bridge_options(dir), std::exception);
    std::error_code ec; std::filesystem::remove_all(dir, ec);
}
} // namespace acecode::channels
