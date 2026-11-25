#include "tui/mode_picker.hpp"

#include <gtest/gtest.h>

using acecode::build_mode_picker_options;
using acecode::PermissionMode;

TEST(ModePicker, BuildsAllModesInCommandOrder) {
    const auto options = build_mode_picker_options(PermissionMode::Default);

    // 自动模式（试验）排在 auto 之后(openspec add-auto-review-mode):五项。
    ASSERT_EQ(options.size(), 5u);
    EXPECT_EQ(options[0].mode, PermissionMode::Default);
    EXPECT_EQ(options[0].name, "default");
    EXPECT_EQ(options[1].mode, PermissionMode::Auto);
    EXPECT_EQ(options[1].name, "auto");
    EXPECT_EQ(options[2].mode, PermissionMode::AutoReview);
    EXPECT_EQ(options[2].name, "auto-review");
    EXPECT_EQ(options[3].mode, PermissionMode::Plan);
    EXPECT_EQ(options[3].name, "plan");
    EXPECT_EQ(options[4].mode, PermissionMode::Yolo);
    EXPECT_EQ(options[4].name, "yolo");
}

TEST(ModePicker, UsesPermissionManagerDescriptions) {
    const auto options = build_mode_picker_options(PermissionMode::Default);

    for (const auto& option : options) {
        EXPECT_EQ(option.description,
                  acecode::PermissionManager::mode_description(option.mode));
        EXPECT_FALSE(option.description.empty());
    }
}

TEST(ModePicker, MarksOnlyCurrentMode) {
    const auto options = build_mode_picker_options(PermissionMode::Plan);

    ASSERT_EQ(options.size(), 5u);
    EXPECT_FALSE(options[0].is_current);
    EXPECT_FALSE(options[1].is_current);
    EXPECT_FALSE(options[2].is_current);
    EXPECT_TRUE(options[3].is_current);
    EXPECT_FALSE(options[4].is_current);
}
