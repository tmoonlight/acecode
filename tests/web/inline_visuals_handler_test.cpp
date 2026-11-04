// 覆盖 src/apps/web/handlers/inline_visuals_handler.cpp 的纯函数(设置 > 常规 >
// 「可视化图表」):
//   1. inline_visuals_snapshot:GET 响应体只有 enabled
//   2. parse_inline_visuals_request:PUT 的 patch 语义、enabled 类型校验、失败时 out 不变

#include <gtest/gtest.h>

#include "web/handlers/inline_visuals_handler.hpp"

#include <string>
#include <vector>

using acecode::InlineVisualsConfig;
using acecode::web::inline_visuals_snapshot;
using acecode::web::parse_inline_visuals_request;

// 场景:设置页打开时 GET 快照(默认开启)。期望:只有 enabled 一个字段且为 true。
TEST(InlineVisualsHandler, SnapshotExposesOnlyEnabled) {
    const auto snap = inline_visuals_snapshot(InlineVisualsConfig{});
    EXPECT_TRUE(snap.at("enabled").get<bool>());
    EXPECT_EQ(snap.size(), 1u);
    EXPECT_FALSE(inline_visuals_snapshot(InlineVisualsConfig{false}).at("enabled").get<bool>());
}

// 场景:设置页开关发 PUT {enabled:false} / {enabled:true},以及空对象 patch。
// 期望:按 body 生效;空 patch 沿用当前值(不会把关闭的开关意外打开)。
TEST(InlineVisualsHandler, TogglesEnabledWithPatchSemantics) {
    InlineVisualsConfig out;
    std::string error;
    ASSERT_TRUE(parse_inline_visuals_request({{"enabled", false}}, InlineVisualsConfig{},
                                             out, error)) << error;
    EXPECT_FALSE(out.enabled);
    const InlineVisualsConfig off{false};
    ASSERT_TRUE(parse_inline_visuals_request({{"enabled", true}}, off, out, error)) << error;
    EXPECT_TRUE(out.enabled);
    ASSERT_TRUE(parse_inline_visuals_request(nlohmann::json::object(), off, out, error))
        << error;
    EXPECT_FALSE(out.enabled);
}

// 场景:非法请求 —— body 不是对象、enabled 是字符串 "no"(规格里的 400 场景)或数字。
// 期望:返回 false、error 非空,out 保持调用前的值(路由据此回 400,配置不变)。
TEST(InlineVisualsHandler, RejectsInvalidRequestsWithoutTouchingOutput) {
    const auto cases = std::vector<nlohmann::json>{
        nlohmann::json::array(),
        {{"enabled", "no"}},
        {{"enabled", 0}},
        {{"enabled", nullptr}},
    };
    for (const auto& body : cases) {
        const InlineVisualsConfig before{false};
        InlineVisualsConfig out = before;
        std::string error;
        EXPECT_FALSE(parse_inline_visuals_request(body, InlineVisualsConfig{}, out, error))
            << body.dump();
        EXPECT_FALSE(error.empty()) << body.dump();
        EXPECT_EQ(out, before) << body.dump();
    }
}
