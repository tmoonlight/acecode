// End-to-end checks against a real, booted simulator. Skipped unless
// ACECODE_IOS_SIM_E2E_UDID is set; also needs ACECODE_IOS_SIM_HELPER (path to
// acecode-ios-sim) and optionally ACECODE_IOS_SIM_XCODE (Xcode.app path).
// They interact with the device (Home button, a tap on the home screen).
#include <gtest/gtest.h>

#include "tool/ios_simulator/device_consent.hpp"
#include "tool/ios_simulator/runtime.hpp"
#include "tool/ios_simulator/tool_actions.hpp"
#include "utils/state_file.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <thread>

using namespace acecode;
using namespace acecode::ios_simulator;

namespace {

class IosSimulatorE2E : public testing::Test {
protected:
    void SetUp() override {
        const char* udid = std::getenv("ACECODE_IOS_SIM_E2E_UDID");
        if (!udid || !*udid) GTEST_SKIP() << "ACECODE_IOS_SIM_E2E_UDID not set";
        udid_ = udid;
        state_path_ = std::filesystem::temp_directory_path() / "acecode-ios-sim-e2e-state.json";
        std::filesystem::remove(state_path_);
        set_state_file_path_for_test(state_path_.string());
        RuntimeConfig config;
        if (const char* xcode = std::getenv("ACECODE_IOS_SIM_XCODE")) config.xcode_path = xcode;
        Runtime::instance().configure(config);
        ctx_.session_id = "e2e-session";
        ctx_.current_permission_mode = [] { return std::string("yolo"); };
    }
    void TearDown() override {
        if (udid_.empty()) return;
        Runtime::instance().release_session("e2e-session");
        set_state_file_path_for_test("");
        std::filesystem::remove(state_path_);
    }
    nlohmann::json args(nlohmann::json extra) const {
        extra["device"] = udid_;
        return extra;
    }
    std::string udid_;
    std::filesystem::path state_path_;
    ToolContext ctx_;
};

} // namespace

TEST_F(IosSimulatorE2E, AttachScreenshotInspectAndInput) {
    auto attach = run_simulator_action("attach", args({}), ctx_);
    ASSERT_TRUE(attach.success) << attach.output;
    EXPECT_EQ(attach.metadata["ios_simulator"]["udid"], udid_);
    EXPECT_TRUE(device_consent_granted(udid_));

    auto shot = run_simulator_action("screenshot", args({}), ctx_);
    ASSERT_TRUE(shot.success) << shot.output;
    ASSERT_EQ(shot.attachments.size(), 1u);
    EXPECT_EQ(shot.attachments[0]["data_url"].get<std::string>().rfind("data:image/jpeg;base64,", 0), 0u);

    ASSERT_TRUE(run_simulator_action("button", args({{"name", "home"}}), ctx_).success);
    auto inspect = run_simulator_action("inspect", args({}), ctx_);
    EXPECT_TRUE(inspect.success) << inspect.output;

    const double width = attach.metadata["ios_simulator"]["point_width"].get<double>();
    const double height = attach.metadata["ios_simulator"]["point_height"].get<double>();
    auto swipe = run_simulator_action("swipe", args({{"x", width * 0.8}, {"y", height * 0.5},
                                                     {"x2", width * 0.2}, {"y2", height * 0.5}}), ctx_);
    EXPECT_TRUE(swipe.success) << swipe.output;
    ASSERT_TRUE(run_simulator_action("button", args({{"name", "home"}}), ctx_).success);
    auto key = run_simulator_action("key", args({{"key", "cmd+a"}}), ctx_);
    EXPECT_TRUE(key.success) << key.output;
    auto refused = run_simulator_action("tap", args({{"x", width + 50}, {"y", 10}}), ctx_);
    EXPECT_FALSE(refused.success);
}

TEST_F(IosSimulatorE2E, StreamDeliversFrames) {
    std::atomic<int> frames{0};
    std::string error;
    const auto subscription = Runtime::instance().subscribe(udid_, [&frames](const StreamFrame& frame) {
        if (!frame.ended && frame.jpeg && !frame.jpeg->empty()) frames.fetch_add(1);
    }, &error);
    ASSERT_NE(subscription, 0u) << error;
    for (int i = 0; i < 50 && frames.load() == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    Runtime::instance().unsubscribe(subscription);
    EXPECT_GT(frames.load(), 0);
}
