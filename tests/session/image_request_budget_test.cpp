// 覆盖 session/image_request_budget(「压缩图片」的请求体预算,grok-build
// image_budget.rs 的触发线 / 回收线,外加跨请求保留的低水位标记):
//   1. 上限换算:触发线 = 上限 - 3 MiB,回收线 = 上限 / 2;过小的上限被抬到下限
//   2. 低于触发线时一张图都不动(保持 prompt cache 前缀逐字节稳定)
//   3. 达到触发线:从最旧的图开始换成文件引用,直到回收线以下;最新的图保留,
//      消息里的文字不受影响
//   4. 滞回:淘汰过一次后历史继续增长但没到触发线,下一次请求淘汰同一批图
//   5. 再次长到触发线时低水位标记前移,继续从更早的图里腾地方
//   6. 低水位标记对应的图片已不在历史里(压缩 / 改写过)时按无状态重新计算
//   7. 工具结果里的图片同样被换成文件引用
//   8. 尺寸回调返回 0 的部件(不发内联图的)不参与预算
// 尺寸由测试回调按附件 id 查表给出,不读磁盘。

#include <gtest/gtest.h>

#include "session/attachment_prompt_context.hpp"
#include "session/image_request_budget.hpp"

#include <map>
#include <string>
#include <vector>

namespace {

using acecode::ChatMessage;
using acecode::ImageRequestBudget;
using acecode::ImageRequestBudgetLimits;

constexpr std::size_t kMiB = 1024u * 1024u;

nlohmann::json image_part(const std::string& id) {
    return {{"type", "image"},
            {"attachment", {{"id", id}, {"name", id + ".png"}, {"mime_type", "image/png"},
                            {"path", "/tmp/" + id + ".png"}}}};
}

ChatMessage user_with_image(const std::string& id, const std::string& text = "look") {
    ChatMessage message;
    message.role = "user";
    message.content = text;
    message.content_parts = nlohmann::json::array({image_part(id)});
    return message;
}

ChatMessage assistant_text(const std::string& text) {
    ChatMessage message;
    message.role = "assistant";
    message.content = text;
    return message;
}

struct FakeSizes {
    std::map<std::string, std::size_t> bytes;
    acecode::ImageWireBytesFn fn() const {
        return [this](const nlohmann::json& part) -> std::size_t {
            const auto id = part["attachment"].value("id", std::string{});
            const auto found = bytes.find(id);
            return found == bytes.end() ? 0 : found->second;
        };
    }
};

bool is_image(const ChatMessage& message) {
    return message.content_parts.is_array() && !message.content_parts.empty() &&
           message.content_parts[0].value("type", std::string{}) == "image";
}

bool is_budget_withheld(const ChatMessage& message) {
    if (!message.content_parts.is_array() || message.content_parts.empty()) return false;
    const auto& part = message.content_parts[0];
    return part.value("type", std::string{}) == "file" &&
           part["attachment"]["metadata"].value(acecode::kImageBudgetWithheldMetadataKey, false);
}

// 10 张 2 MiB 的图(约 20 MiB),触发线 12 MiB、回收线 7 MiB。
std::vector<ChatMessage> ten_images(FakeSizes& sizes) {
    std::vector<ChatMessage> messages;
    for (int i = 0; i < 10; ++i) {
        const std::string id = "img" + std::to_string(i);
        sizes.bytes[id] = 2 * kMiB;
        messages.push_back(user_with_image(id));
        messages.push_back(assistant_text("ok"));
    }
    return messages;
}

const ImageRequestBudgetLimits kSmallLimits{12 * kMiB, 7 * kMiB};

} // namespace

// 场景 1:grok-build 的默认上限 50 MiB → 触发 47 MiB、回收 25 MiB;Anthropic 的
// 30,000,000 → 触发 30,000,000-3 MiB、回收 15,000,000;1 字节的上限被抬到 12 MiB
// (再小的话回收线会碰到触发线,一次淘汰换不来任何稳定的回合)。
TEST(ImageRequestBudget, LimitsFollowTheRequestCap) {
    const auto openai = acecode::image_request_budget_limits(acecode::kOpenAiCompatMaxRequestBytes);
    EXPECT_EQ(openai.trigger_bytes, 47 * kMiB);
    EXPECT_EQ(openai.reclaim_target_bytes, 25 * kMiB);
    const auto anthropic = acecode::image_request_budget_limits(acecode::kAnthropicMaxRequestBytes);
    EXPECT_EQ(anthropic.trigger_bytes, 30'000'000u - 3 * kMiB);
    EXPECT_EQ(anthropic.reclaim_target_bytes, 15'000'000u);
    const auto tiny = acecode::image_request_budget_limits(1);
    EXPECT_EQ(tiny.trigger_bytes, 9 * kMiB);
    EXPECT_EQ(tiny.reclaim_target_bytes, 6 * kMiB);
}

// 场景 2:3 张 2 MiB 的图(约 6 MiB),触发线 12 MiB。
// 期望:evicted=0,所有部件仍是 image,body_bytes_after == body_bytes。
TEST(ImageRequestBudget, BelowTriggerLeavesEveryImage) {
    FakeSizes sizes;
    std::vector<ChatMessage> messages;
    for (int i = 0; i < 3; ++i) {
        const std::string id = "img" + std::to_string(i);
        sizes.bytes[id] = 2 * kMiB;
        messages.push_back(user_with_image(id));
    }
    ImageRequestBudget budget;
    const auto outcome = budget.apply(messages, kSmallLimits, sizes.fn());
    EXPECT_EQ(outcome.evicted, 0u);
    EXPECT_EQ(outcome.inline_images, 3u);
    EXPECT_EQ(outcome.body_bytes_after, outcome.body_bytes);
    for (const auto& message : messages) EXPECT_TRUE(is_image(message));
}

// 场景 3:10 张 2 MiB 的图(约 20 MiB ≥ 触发线 12 MiB)。
// 期望:从 img0 开始淘汰,直到 ≤ 7 MiB —— 淘汰 7 张(剩 3 张约 6 MiB 加文件引用与
// 文字的几 KB),留下最新的 3 张;被淘汰的部件
// 变成带 image_budget_withheld 标记的文件引用,所在消息的文字 "look" 不变。
TEST(ImageRequestBudget, OverTriggerEvictsOldestToReclaimTarget) {
    FakeSizes sizes;
    auto messages = ten_images(sizes);
    ImageRequestBudget budget;
    const auto outcome = budget.apply(messages, kSmallLimits, sizes.fn());
    EXPECT_EQ(outcome.evicted, 7u);
    EXPECT_LE(outcome.body_bytes_after, kSmallLimits.reclaim_target_bytes);
    for (int i = 0; i < 10; ++i) {
        const auto& message = messages[static_cast<std::size_t>(i) * 2];
        EXPECT_EQ(message.content, "look");
        if (i < 7) {
            EXPECT_TRUE(is_budget_withheld(message)) << "img" << i << " 应被淘汰";
        } else {
            EXPECT_TRUE(is_image(message)) << "img" << i << " 是较新的图,应保留";
        }
    }
}

// 场景 4(滞回):场景 3 淘汰后,再追加 2 张新图(总量 24 MiB,扣掉已淘汰的 7 张后
// 约 10 MiB,低于触发线)。
// 期望:第二次请求只淘汰与第一次相同的 7 张(低水位标记生效),新图全部保留 ——
// 修复前(grok-build 的无状态写法)每加一张图就会多淘汰一张旧图,前缀每回合都变。
TEST(ImageRequestBudget, LowWaterMarkKeepsTheSameImagesRemoved) {
    FakeSizes sizes;
    auto history = ten_images(sizes);
    ImageRequestBudget budget;
    auto first = history;
    ASSERT_EQ(budget.apply(first, kSmallLimits, sizes.fn()).evicted, 7u);

    for (int i = 10; i < 12; ++i) {
        const std::string id = "img" + std::to_string(i);
        sizes.bytes[id] = 2 * kMiB;
        history.push_back(user_with_image(id));
    }
    auto second = history;
    const auto outcome = budget.apply(second, kSmallLimits, sizes.fn());
    EXPECT_EQ(outcome.evicted, 7u);
    for (std::size_t i = 0; i < 7; ++i) EXPECT_EQ(second[i * 2].content_parts, first[i * 2].content_parts);
    EXPECT_TRUE(is_image(second.back()));
}

// 场景 5:场景 4 之后继续追加,直到扣掉已淘汰的图也达到触发线(再加 4 张,
// 约 18 MiB)。期望:淘汰数大于 7,标记前移;最新一张仍保留。
TEST(ImageRequestBudget, RegrowthPastTriggerAdvancesTheMark) {
    FakeSizes sizes;
    auto history = ten_images(sizes);
    ImageRequestBudget budget;
    auto first = history;
    ASSERT_EQ(budget.apply(first, kSmallLimits, sizes.fn()).evicted, 7u);

    for (int i = 10; i < 14; ++i) {
        const std::string id = "img" + std::to_string(i);
        sizes.bytes[id] = 2 * kMiB;
        history.push_back(user_with_image(id));
    }
    auto grown = history;
    const auto outcome = budget.apply(grown, kSmallLimits, sizes.fn());
    EXPECT_GT(outcome.evicted, 7u);
    EXPECT_LE(outcome.body_bytes_after, kSmallLimits.reclaim_target_bytes);
    EXPECT_TRUE(is_image(grown.back()));
}

// 场景 6:淘汰过后,历史被压缩成只剩 img8 / img9 两张(低水位标记 img6 不在了),
// 总量 4 MiB。期望:找不到标记就按无状态计算,低于触发线 → 一张都不淘汰。
TEST(ImageRequestBudget, MissingMarkFallsBackToStatelessBudget) {
    FakeSizes sizes;
    auto history = ten_images(sizes);
    ImageRequestBudget budget;
    ASSERT_EQ(budget.apply(history, kSmallLimits, sizes.fn()).evicted, 7u);

    std::vector<ChatMessage> compacted = {user_with_image("img8"), user_with_image("img9")};
    const auto outcome = budget.apply(compacted, kSmallLimits, sizes.fn());
    EXPECT_EQ(outcome.evicted, 0u);
    EXPECT_TRUE(is_image(compacted[0]));
    EXPECT_TRUE(is_image(compacted[1]));
}

// 场景 7:最旧的图在工具结果里(role=tool,例如截图工具的输出),其后 9 张用户图。
// 期望:工具结果里的图同样被换成文件引用,tool_call_id 与正文保持不变。
TEST(ImageRequestBudget, ToolResultImagesAreWithheldToo) {
    FakeSizes sizes;
    auto messages = ten_images(sizes);
    ChatMessage tool;
    tool.role = "tool";
    tool.tool_call_id = "call-1";
    tool.content = "Captured";
    tool.content_parts = nlohmann::json::array({image_part("shot")});
    sizes.bytes["shot"] = 2 * kMiB;
    messages.insert(messages.begin(), tool);

    ImageRequestBudget budget;
    budget.apply(messages, kSmallLimits, sizes.fn());
    EXPECT_TRUE(is_budget_withheld(messages[0]));
    EXPECT_EQ(messages[0].tool_call_id, "call-1");
    EXPECT_EQ(messages[0].content, "Captured");
}

// 场景 8:10 张图里有 9 张的尺寸回调返回 0(例如 MIME 不是可发送的栅格图),
// 只有 1 张 2 MiB。期望:inline_images=1,低于触发线,不淘汰;返回 0 的部件原样保留。
TEST(ImageRequestBudget, PartsWithoutWireBytesAreIgnored) {
    FakeSizes sizes;
    auto messages = ten_images(sizes);
    for (int i = 1; i < 10; ++i) sizes.bytes.erase("img" + std::to_string(i));
    ImageRequestBudget budget;
    const auto outcome = budget.apply(messages, kSmallLimits, sizes.fn());
    EXPECT_EQ(outcome.inline_images, 1u);
    EXPECT_EQ(outcome.evicted, 0u);
    EXPECT_TRUE(is_image(messages[2]));
}
