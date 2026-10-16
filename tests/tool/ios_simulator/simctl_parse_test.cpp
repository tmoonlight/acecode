#include <gtest/gtest.h>

#include "tool/ios_simulator/simctl.hpp"

using namespace acecode::ios_simulator;

TEST(SimctlParse, MergesRuntimesAndSortsBootedFirst) {
    const auto list = nlohmann::json::parse(R"({
      "runtimes": [{"identifier": "com.apple.CoreSimulator.SimRuntime.iOS-26-5", "name": "iOS 26.5",
                    "version": "26.5", "platform": "iOS"}],
      "devices": {
        "com.apple.CoreSimulator.SimRuntime.iOS-26-5": [
          {"udid": "B", "name": "iPhone 17e", "state": "Shutdown", "isAvailable": true,
           "deviceTypeIdentifier": "com.apple.CoreSimulator.SimDeviceType.iPhone-17e"},
          {"udid": "A", "name": "iPhone 17 Pro", "state": "Booted", "isAvailable": true}
        ],
        "com.apple.CoreSimulator.SimRuntime.watchOS-12-0": [
          {"udid": "W", "name": "Apple Watch", "state": "Shutdown", "isAvailable": false,
           "availabilityError": "runtime profile not found"}
        ]
      }
    })");
    auto devices = parse_simctl_list(list);
    ASSERT_EQ(devices.size(), 3u);
    EXPECT_EQ(devices[0].udid, "A");
    EXPECT_TRUE(is_booted_state(devices[0].state));
    EXPECT_EQ(devices[0].runtime_name, "iOS 26.5");
    EXPECT_EQ(devices[0].platform, "iOS");
    const auto watch = std::find_if(devices.begin(), devices.end(), [](const SimctlDevice& d) { return d.udid == "W"; });
    ASSERT_NE(watch, devices.end());
    EXPECT_EQ(watch->runtime_name, "watchOS 12.0");
    EXPECT_EQ(watch->platform, "watchOS");
    EXPECT_FALSE(watch->available);
    EXPECT_EQ(to_json(devices[0])["booted"], true);
}

TEST(SimctlParse, ToleratesMissingSections) {
    EXPECT_TRUE(parse_simctl_list(nlohmann::json::object()).empty());
    EXPECT_TRUE(parse_simctl_list(nlohmann::json::parse(R"({"devices": {"x": "bad"}})")).empty());
}

TEST(SimctlParse, ResultMessagesPreferStderr) {
    SimctlResult result;
    result.exit_code = 164;
    result.err = "Invalid device: X\n";
    EXPECT_EQ(result.message(), "Invalid device: X");
    result.err.clear();
    EXPECT_NE(result.message().find("164"), std::string::npos);
    result.timed_out = true;
    EXPECT_EQ(result.message(), "simctl timed out.");
}
