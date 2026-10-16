#include <gtest/gtest.h>

#include "tool/ios_simulator/input_plan.hpp"

#include <stdexcept>

using namespace acecode::ios_simulator;

namespace {
const ScreenSize kPhone{402, 874};

int count_type(const nlohmann::json& events, const char* type) {
    int count = 0;
    for (const auto& event : events) count += event.value("type", std::string()) == type;
    return count;
}
} // namespace

TEST(IosSimulatorInputPlan, TapHoldsAndReleasesAtThePoint) {
    auto events = plan_tap(100, 200, 0, kPhone);
    ASSERT_EQ(events.size(), 3u);
    EXPECT_EQ(events[0]["phase"], "down");
    EXPECT_EQ(events[2]["phase"], "up");
    EXPECT_EQ(events[2]["x"], 100);
    EXPECT_LT(planned_duration_ms(events), 200);
    EXPECT_NEAR(planned_duration_ms(plan_tap(10, 10, 1.2, kPhone)), 1200, 1);
    EXPECT_THROW(plan_tap(500, 10, 0, kPhone), std::invalid_argument);
    EXPECT_THROW(plan_tap(10, -1, 0, kPhone), std::invalid_argument);
}

TEST(IosSimulatorInputPlan, SwipeInterpolatesAndEndsWithStationarySample) {
    auto events = plan_swipe(200, 700, 200, 200, 0.5, kPhone);
    const int touches = count_type(events, "touch");
    EXPECT_EQ(touches, 50 + 1 + 1 + 1);  // 500 pt at 10 pt/sample, plus hold and release
    EXPECT_EQ(events.back()["phase"], "up");
    EXPECT_EQ(events.back()["y"], 200);
    EXPECT_NEAR(planned_duration_ms(events), 500, 1);
    EXPECT_FALSE(events.front().contains("edge"));
}

TEST(IosSimulatorInputPlan, EdgeGesturesAreTaggedFromTheStartPoint) {
    EXPECT_EQ(edge_for_start(201, 873, kPhone), "bottom");
    EXPECT_EQ(edge_for_start(1, 400, kPhone), "left");
    EXPECT_EQ(edge_for_start(401, 400, kPhone), "right");
    EXPECT_EQ(edge_for_start(200, 2, kPhone), "top");
    EXPECT_EQ(edge_for_start(200, 400, kPhone), "none");
    auto events = plan_swipe(201, 873, 201, 400, 0.3, kPhone);
    EXPECT_EQ(events.front()["edge"], "bottom");
}

TEST(IosSimulatorInputPlan, TouchPathsHonourDelaysAndLimits) {
    std::vector<PathPoint> points{{10, 10, 0}, {20, 20, 100}, {30, 30, 50}};
    auto events = plan_touch_path(points, kPhone);
    EXPECT_EQ(count_type(events, "touch"), 3);
    EXPECT_EQ(events.back()["phase"], "up");
    EXPECT_NEAR(planned_duration_ms(events), 150, 0.1);
    EXPECT_THROW(plan_touch_path({{10, 10, 0}}, kPhone), std::invalid_argument);
    std::vector<PathPoint> slow(40, PathPoint{10, 10, 1000});
    EXPECT_THROW(plan_touch_path(slow, kPhone), std::invalid_argument);

    std::vector<TwoFingerPoint> pinch{{100, 400, 300, 400, 0}, {150, 400, 250, 400, 30}};
    auto two = plan_touch2_path(pinch, kPhone);
    EXPECT_EQ(count_type(two, "touch2"), 2);
    EXPECT_EQ(two.back()["phase"], "up");
    EXPECT_EQ(two.back()["x2"], 250);
}

TEST(IosSimulatorInputPlan, TextMapsToUsKeyboardUsages) {
    auto strokes = keys_for_text("Hi!\n");
    ASSERT_TRUE(strokes.has_value());
    ASSERT_EQ(strokes->size(), 4u);
    EXPECT_EQ((*strokes)[0].usage, 0x0Bu);  // h
    EXPECT_TRUE((*strokes)[0].shift);
    EXPECT_EQ((*strokes)[1].usage, 0x0Cu);  // i
    EXPECT_FALSE((*strokes)[1].shift);
    EXPECT_EQ((*strokes)[2].usage, 0x1Eu);  // ! = shift+1
    EXPECT_TRUE((*strokes)[2].shift);
    EXPECT_EQ((*strokes)[3].usage, 0x28u);  // return
    EXPECT_FALSE(keys_for_text("你好").has_value());
    EXPECT_FALSE(keys_for_text("emoji 😀").has_value());

    auto events = plan_keystrokes(*strokes, 8);
    // shift wraps the first and third strokes.
    EXPECT_EQ(events[0]["usage"], kUsageLeftShift);
    EXPECT_EQ(events[0]["direction"], "down");
}

TEST(IosSimulatorInputPlan, ChordsParseNamedKeysAndModifiers) {
    auto chord = chord_for_name("Cmd+A");
    ASSERT_TRUE(chord.has_value());
    EXPECT_EQ((*chord)[0], kUsageLeftCommand);
    EXPECT_EQ((*chord)[1], 0x04u);
    EXPECT_EQ((*chord_for_name("return"))[0], 0x28u);
    EXPECT_EQ((*chord_for_name("shift+tab"))[1], 0x2Bu);
    EXPECT_EQ((*chord_for_name("f12"))[0], 0x45u);
    EXPECT_FALSE(chord_for_name("hyper+q").has_value());
    EXPECT_FALSE(chord_for_name("").has_value());
    auto events = plan_chord({kUsageLeftCommand, kUsageV});
    ASSERT_EQ(events.size(), 4u);
    EXPECT_EQ(events[2]["usage"], kUsageV);
    EXPECT_EQ(events[2]["direction"], "up");
    EXPECT_EQ(events[3]["usage"], kUsageLeftCommand);
}
