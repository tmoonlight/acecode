#include <gtest/gtest.h>

#include "llm/token_estimate.hpp"

#include <string>
#include <vector>

namespace {

acecode::ChatMessage tool_message_with_images(int images) {
    acecode::ChatMessage message;
    message.role = "tool";
    message.tool_call_id = "shot";
    message.content = R"({"ok":true,"width":1494,"height":938})";
    message.content_parts = nlohmann::json::array();
    for (int i = 0; i < images; ++i) {
        message.content_parts.push_back(nlohmann::json{
            {"type", "image"},
            {"attachment", {
                {"id", "att_" + std::to_string(i)},
                {"name", "browser-screenshot.png"},
                {"mime_type", "image/png"},
                {"size_bytes", 121073},
            }},
        });
    }
    return message;
}

} // namespace

// 触发场景:一条工具结果带两张图片部件(浏览器截图),另一条完全相同但图片部件
// 已降级成 file 部件。
// 期望行为:每张图片部件按 kEstimatedImageTokens 计入估算;降级成 file 的部件
// 不再按图片计;has_image_parts 只认 type == "image"。
// 回归背景(反馈 LINDANDAN069):旧估算只算附件元数据那几百字节,一张截图约
// 100 token,带图请求被服务端判超限时本地估算只有 3.7 万,估算器完全看不见图。
// 这里不按 base64 长度算(121KB 截图会被算成约 4 万 token),理由见头文件注释。
TEST(TokenEstimate, CountsEachInlineImagePartAsFixedTokenCost) {
    const auto without = tool_message_with_images(0);
    const auto with_two = tool_message_with_images(2);

    EXPECT_EQ(acecode::count_image_parts(without), 0u);
    EXPECT_EQ(acecode::count_image_parts(with_two), 2u);
    const std::size_t metadata_bytes = with_two.content_parts.dump().size();
    EXPECT_EQ(acecode::estimate_message_payload_bytes(with_two),
              acecode::estimate_message_payload_bytes(without) + metadata_bytes +
                  2 * acecode::kEstimatedImageTokens * 4);

    const int tokens = acecode::estimate_message_tokens({with_two});
    EXPECT_GE(tokens, static_cast<int>(2 * acecode::kEstimatedImageTokens));
    EXPECT_LT(tokens, static_cast<int>(2 * acecode::kEstimatedImageTokens + 1000))
        << "图片不能按 base64 长度计,否则截图多的会话几张图就触发压缩";

    auto withheld = with_two;
    for (auto& part : withheld.content_parts) part["type"] = "file";
    EXPECT_EQ(acecode::count_image_parts(withheld), 0u);
    EXPECT_LT(acecode::estimate_message_tokens({withheld}), tokens);

    EXPECT_TRUE(acecode::has_image_parts({without, with_two}));
    EXPECT_FALSE(acecode::has_image_parts({without, withheld}));
}
