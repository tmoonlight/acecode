// Covers pure helpers for /api/sessions/:id/permissions.

#include <gtest/gtest.h>

#include "web/handlers/permission_mode_handler.hpp"

using acecode::PermissionMode;
using acecode::web::parse_permission_mode_name;
using acecode::web::permission_mode_to_json;

TEST(PermissionModeHandler, ParsesCanonicalAndLegacyNames) {
    EXPECT_EQ(parse_permission_mode_name("default"), PermissionMode::Default);
    EXPECT_EQ(parse_permission_mode_name("auto"), PermissionMode::Auto);
    EXPECT_EQ(parse_permission_mode_name("accept-edits"), PermissionMode::Auto);
    EXPECT_EQ(parse_permission_mode_name("acceptEdits"), PermissionMode::Auto);
    EXPECT_EQ(parse_permission_mode_name("plan"), PermissionMode::Plan);
    EXPECT_EQ(parse_permission_mode_name("yolo"), PermissionMode::Yolo);
    EXPECT_FALSE(parse_permission_mode_name("ask").has_value());
    EXPECT_FALSE(parse_permission_mode_name("").has_value());
}

TEST(PermissionModeHandler, SerializesCanonicalModeName) {
    auto j = permission_mode_to_json(PermissionMode::Auto);
    EXPECT_EQ(j["mode"], "auto");
    EXPECT_TRUE(j.contains("description"));

    auto plan = permission_mode_to_json(PermissionMode::Plan);
    EXPECT_EQ(plan["mode"], "plan");
    EXPECT_TRUE(plan["description"].get<std::string>().find("Plan") != std::string::npos);
}

// 场景:会话权限接口与默认权限接口收到自动模式（试验）的线上名(openspec add-auto-review-mode)。
// 期望:解析成 AutoReview;返回体的 mode 写回 "auto-review" 并带非空描述。
// 回归意义:Web 端拿不到这个名字时会把模式规范化成「默认权限」显示。
TEST(PermissionModeHandler, AcceptsAndSerializesAutoReview) {
    EXPECT_EQ(parse_permission_mode_name("auto-review"), PermissionMode::AutoReview);
    EXPECT_FALSE(parse_permission_mode_name("auto_review").has_value());

    auto j = permission_mode_to_json(PermissionMode::AutoReview);
    EXPECT_EQ(j["mode"], "auto-review");
    ASSERT_TRUE(j.contains("description"));
    EXPECT_FALSE(j["description"].get<std::string>().empty());
}
