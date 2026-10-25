// 覆盖 src/apps/web/handlers/image_compression_handler.cpp 的纯函数(设置 > 常规 >
// 「压缩图片」):
//   1. image_compression_snapshot:GET 响应体只有 enabled
//   2. parse_image_compression_request:PUT 的 patch 语义、enabled 类型校验、失败时 out 不变

#include <gtest/gtest.h>

#include "web/handlers/image_compression_handler.hpp"

#include <string>
#include <vector>

using acecode::ImageCompressionConfig;
using acecode::web::image_compression_snapshot;
using acecode::web::parse_image_compression_request;

// 场景:GET 快照(默认开启)。期望:只有 enabled 一个字段且为 true。
TEST(ImageCompressionHandler, SnapshotExposesOnlyEnabled) {
    const auto snap = image_compression_snapshot(ImageCompressionConfig{});
    EXPECT_TRUE(snap.at("enabled").get<bool>());
    EXPECT_EQ(snap.size(), 1u);
}

// 场景:设置页开关发 PUT {enabled:false} / {enabled:true},以及空对象 patch。
// 期望:按 body 生效;空 patch 沿用当前值。
TEST(ImageCompressionHandler, TogglesEnabledWithPatchSemantics) {
    ImageCompressionConfig out;
    std::string error;
    ASSERT_TRUE(parse_image_compression_request({{"enabled", false}}, ImageCompressionConfig{},
                                                out, error)) << error;
    EXPECT_FALSE(out.enabled);
    const ImageCompressionConfig off{false};
    ASSERT_TRUE(parse_image_compression_request({{"enabled", true}}, off, out, error)) << error;
    EXPECT_TRUE(out.enabled);
    ASSERT_TRUE(parse_image_compression_request(nlohmann::json::object(), off, out, error))
        << error;
    EXPECT_FALSE(out.enabled);
}

// 场景:非法请求 —— body 不是对象 / enabled 非布尔。期望:返回 false、error 非空,
// out 保持调用前的值。
TEST(ImageCompressionHandler, RejectsInvalidRequestsWithoutTouchingOutput) {
    const auto cases = std::vector<nlohmann::json>{
        nlohmann::json::array(),
        {{"enabled", "no"}},
        {{"enabled", 0}},
    };
    for (const auto& body : cases) {
        const ImageCompressionConfig before{false};
        ImageCompressionConfig out = before;
        std::string error;
        EXPECT_FALSE(parse_image_compression_request(body, ImageCompressionConfig{}, out, error))
            << body.dump();
        EXPECT_FALSE(error.empty()) << body.dump();
        EXPECT_EQ(out, before) << body.dump();
    }
}
