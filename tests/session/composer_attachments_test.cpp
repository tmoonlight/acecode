#include <gtest/gtest.h>
#include "session/composer_attachments.hpp"

TEST(ComposerAttachments, PlainTextKeepsLegacyEmptyParts) {
    // 只有正文时沿用旧 text 入口,不能额外重复发送一份 text part。
    const auto input = acecode::build_user_input_with_attachments("prompt", "display", {});
    EXPECT_EQ(input.text, "prompt");
    EXPECT_EQ(input.display_text, "display");
    EXPECT_TRUE(input.content_parts.empty());
    EXPECT_FALSE(input.metadata.contains("attachments"));
}
TEST(ComposerAttachments, SvgClassificationUsesMimeAndNameInsteadOfStoredKind) {
    // 旧附件即使保存为 image,SVG 仍应送文件 part;显示标签与原元数据保持原样。
    const nlohmann::json svg{{"name", "plot.svg"}, {"kind", "image"}, {"mime_type", "image/svg+xml"}};
    const nlohmann::json png{{"name", "photo.png"}, {"kind", "image"}, {"mime_type", "image/png"}};
    const auto input = acecode::build_user_input_with_attachments("inspect", "shown", {svg, png});
    ASSERT_EQ(input.content_parts.size(), 3u);
    EXPECT_EQ(input.content_parts[0]["type"], "text");
    EXPECT_EQ(input.content_parts[1]["type"], "file");
    EXPECT_EQ(input.content_parts[2]["type"], "image");
    EXPECT_EQ(input.metadata["attachments"], nlohmann::json::array({svg, png}));
    EXPECT_EQ(acecode::display_prompt_with_attachments("inspect", {svg, png}),
              "inspect\n[Image: plot.svg]\n[Image: photo.png]");
}
