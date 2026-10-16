#include <gtest/gtest.h>

#include "tool/ios_simulator/dtuhid_messages.hpp"

#include <string>

using namespace acecode::ios_simulator::dtuhid;

// 场景:一根手指按下 → 移动 → 抬起,再按下;两根手指各自独立计相位。
// 期望:0 began / 1 moved / 2 ended,抬起后的下一次按下重新开始。
TEST(IosSimulatorDtuhid, ContactPhasesFollowDownMoveUp) {
    ContactTracker finger;
    EXPECT_EQ(finger.next(false), Phase::Began);
    EXPECT_TRUE(finger.active());
    EXPECT_EQ(finger.next(false), Phase::Moved);
    EXPECT_EQ(finger.next(false), Phase::Moved);
    EXPECT_EQ(finger.next(true), Phase::Ended);
    EXPECT_FALSE(finger.active());
    EXPECT_EQ(finger.next(false), Phase::Began);
    EXPECT_EQ(static_cast<std::uint64_t>(Phase::Began), 0u);
    EXPECT_EQ(static_cast<std::uint64_t>(Phase::Moved), 1u);
    EXPECT_EQ(static_cast<std::uint64_t>(Phase::Ended), 2u);
}

TEST(IosSimulatorDtuhid, PointsAreFractionsOfThePortraitPointSize) {
    const auto point = unit_point(195, 422, 390, 844);
    EXPECT_DOUBLE_EQ(point.x, 0.5);
    EXPECT_DOUBLE_EQ(point.y, 0.5);
    const auto corner = unit_point(390, 0, 390, 844);
    EXPECT_DOUBLE_EQ(corner.x, 1.0);
    EXPECT_DOUBLE_EQ(corner.y, 0.0);
    const auto unknown = unit_point(10, 10, 0, 0);
    EXPECT_DOUBLE_EQ(unknown.x, 0.0);
    EXPECT_DOUBLE_EQ(unknown.y, 0.0);
    EXPECT_EQ(button_state(true), 1u);
    EXPECT_EQ(button_state(false), 2u);
}

#ifdef __APPLE__
namespace {

std::string text(xpc_object_t dictionary, const char* key) {
    const char* value = xpc_dictionary_get_string(dictionary, key);
    return value ? value : "";
}

bool has_key(xpc_object_t dictionary, const char* key) { return xpc_dictionary_get_value(dictionary, key) != nullptr; }

std::size_t key_count(xpc_object_t dictionary) { return xpc_dictionary_get_count(dictionary); }

} // namespace

// 场景:单指触摸消息。期望:信封四个键齐全,payload 是 pointOne(0..1)+eventType+edge+target,
// 没有 pointTwo;信封持有 payload,调用方释放自己那份后仍可读。
TEST(IosSimulatorDtuhid, DigitizerEnvelopeMatchesTheServiceSchema) {
    xpc_object_t payload = digitizer_payload(unit_point(39, 84.4, 390, 844), nullptr, Phase::Began, 3);
    xpc_object_t message = envelope(kDigitizerEvent, payload, false);
    xpc_release(payload);

    EXPECT_EQ(key_count(message), 4u);
    EXPECT_EQ(text(message, "messageType"), "IndigoDigitizerEvent");
    EXPECT_EQ(text(message, "featureIdentifier"), "com.apple.coredevice.feature.remote.hid.digitizer");
    EXPECT_FALSE(xpc_dictionary_get_bool(message, "isBarrier"));
    xpc_object_t body = xpc_dictionary_get_value(message, "payload");
    ASSERT_NE(body, nullptr);
    ASSERT_EQ(xpc_get_type(body), XPC_TYPE_DICTIONARY);
    EXPECT_EQ(key_count(body), 4u);
    xpc_object_t one = xpc_dictionary_get_value(body, "pointOne");
    ASSERT_NE(one, nullptr);
    EXPECT_DOUBLE_EQ(xpc_dictionary_get_double(one, "x"), 0.1);
    EXPECT_DOUBLE_EQ(xpc_dictionary_get_double(one, "y"), 0.1);
    EXPECT_FALSE(has_key(body, "pointTwo"));
    EXPECT_EQ(xpc_dictionary_get_uint64(body, "eventType"), 0u);
    EXPECT_EQ(xpc_dictionary_get_uint64(body, "edge"), 3u);
    EXPECT_EQ(xpc_dictionary_get_uint64(body, "target"), 0u);
    EXPECT_EQ(xpc_get_type(xpc_dictionary_get_value(body, "eventType")), XPC_TYPE_UINT64);
    xpc_release(message);
}

// 场景:双指(捏合 / 旋转)。期望:带 pointTwo,edge 恒为 0(双指不触发边缘手势)。
TEST(IosSimulatorDtuhid, TwoFingerEventCarriesPointTwoWithoutEdge) {
    const UnitPoint second{0.75, 0.25};
    xpc_object_t payload = digitizer_payload({0.25, 0.75}, &second, Phase::Moved, 4);
    EXPECT_EQ(key_count(payload), 5u);
    xpc_object_t two = xpc_dictionary_get_value(payload, "pointTwo");
    ASSERT_NE(two, nullptr);
    EXPECT_DOUBLE_EQ(xpc_dictionary_get_double(two, "x"), 0.75);
    EXPECT_DOUBLE_EQ(xpc_dictionary_get_double(two, "y"), 0.25);
    EXPECT_EQ(xpc_dictionary_get_uint64(payload, "eventType"), 1u);
    EXPECT_EQ(xpc_dictionary_get_uint64(payload, "edge"), 0u);
    xpc_release(payload);
}

TEST(IosSimulatorDtuhid, ButtonAndKeyboardPayloads) {
    xpc_object_t home = button_payload(0x0C, 0x40, true);
    EXPECT_EQ(key_count(home), 3u);
    EXPECT_EQ(xpc_dictionary_get_uint64(home, "usagePage"), 0x0Cu);
    EXPECT_EQ(xpc_dictionary_get_uint64(home, "usageCode"), 0x40u);
    EXPECT_EQ(xpc_dictionary_get_uint64(home, "state"), 1u);
    xpc_release(home);

    xpc_object_t key = keyboard_payload(0x04, false);
    EXPECT_EQ(key_count(key), 2u);
    EXPECT_EQ(xpc_dictionary_get_uint64(key, "usageCode"), 0x04u);
    EXPECT_EQ(xpc_dictionary_get_uint64(key, "state"), 2u);
    EXPECT_FALSE(has_key(key, "usagePage"));
    xpc_release(key);
}

// 场景:连接后的存活探测。期望:barrier 信封里是 usage 0 的键盘抬起(对设备无副作用)。
TEST(IosSimulatorDtuhid, LivenessProbeIsABarrierKeyUpOfUsageZero) {
    xpc_object_t probe = liveness_probe();
    EXPECT_EQ(text(probe, "messageType"), "IndigoKeyboardButtonEvent");
    EXPECT_TRUE(xpc_dictionary_get_bool(probe, "isBarrier"));
    xpc_object_t body = xpc_dictionary_get_value(probe, "payload");
    ASSERT_NE(body, nullptr);
    EXPECT_EQ(xpc_dictionary_get_uint64(body, "usageCode"), 0u);
    EXPECT_EQ(xpc_dictionary_get_uint64(body, "state"), 2u);
    xpc_release(probe);
}
#endif
