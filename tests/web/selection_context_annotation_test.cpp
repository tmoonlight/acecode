// 覆盖 Web 会话发送时的选区批注清洗与隐藏提示注入。可见消息仍只保留
// display_text，selection_context content part 则持久化精确锚点和批注。

#include "web/server_impl.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <string>

namespace acecode::web {
namespace {

using nlohmann::json;

json annotated_selection() {
    return json{
        {"type", "selection"},
        {"id", "selection-1"},
        {"text", "const answer = 42;"},
        {"selected_text", "const answer = 42;"},
        {"label", "answer.cpp:8"},
        {"source", json{
            {"path", "N:\\project\\answer.cpp"},
            {"kind", "text"},
            {"view", "source"},
            {"start_line", 8},
            {"end_line", 8},
            {"line_count", 1},
            {"start_offset", 0},
            {"end_offset", 18},
            {"content_revision", "content-v1:i:5ce15b0fd1726f2a"},
            {"ignored", "drop me"},
        }},
        {"annotations", json::array({
            json{
                {"id", "annotation-a"},
                {"text", "这里需要解释为什么是 42"},
                {"created_at", "2026-07-26T08:00:00.000Z"},
                {"ignored", "drop me"},
            },
        })},
        {"ignored", "drop me"},
    };
}

// 场景：selection content part 只保留允许的锚点和批注字段，offset=0 也必须保留。
TEST(SelectionContextAnnotation, SanitizesPersistedAnchorAndAnnotations) {
    auto meta = sanitized_selection_context_meta(annotated_selection());
    ASSERT_TRUE(meta.has_value());
    EXPECT_EQ((*meta)["selected_text"], "const answer = 42;");
    ASSERT_TRUE((*meta).contains("source"));
    EXPECT_EQ((*meta)["source"]["view"], "source");
    EXPECT_EQ((*meta)["source"]["start_offset"], 0);
    EXPECT_EQ((*meta)["source"]["end_offset"], 18);
    EXPECT_EQ((*meta)["source"]["content_revision"],
              "content-v1:i:5ce15b0fd1726f2a");
    EXPECT_FALSE((*meta)["source"].contains("ignored"));
    ASSERT_TRUE((*meta).contains("annotations"));
    ASSERT_EQ((*meta)["annotations"].size(), 1u);
    EXPECT_EQ((*meta)["annotations"][0]["id"], "annotation-a");
    EXPECT_EQ((*meta)["annotations"][0]["text"], "这里需要解释为什么是 42");
    EXPECT_EQ((*meta)["annotations"][0]["created_at"],
              "2026-07-26T08:00:00.000Z");
    EXPECT_FALSE((*meta)["annotations"][0].contains("ignored"));
    EXPECT_FALSE((*meta).contains("ignored"));
}

// 场景：文档版本只作为有界元数据持久化，不能用超长客户端字段膨胀会话。
TEST(SelectionContextAnnotation, BoundsPersistedContentRevision) {
    auto selection = annotated_selection();
    selection["source"]["content_revision"] = std::string(300, 'r');

    auto meta = sanitized_selection_context_meta(selection);
    ASSERT_TRUE(meta.has_value());
    ASSERT_TRUE((*meta).contains("source"));
    ASSERT_TRUE((*meta)["source"].contains("content_revision"));
    EXPECT_EQ(
        (*meta)["source"]["content_revision"].get<std::string>().size(),
        128u);
}

// 场景：恶意或损坏的批注数组受单条长度和总条数上限约束，并按 id 去重。
TEST(SelectionContextAnnotation, BoundsAnnotationRecords) {
    auto selection = annotated_selection();
    selection["annotations"] = json::array();
    selection["annotations"].push_back(json{
        {"id", "duplicate"},
        {"text", std::string(kMaxSelectionAnnotationChars + 100, 'x')},
    });
    selection["annotations"].push_back(json{
        {"id", "duplicate"},
        {"text", "must be dropped"},
    });
    for (std::size_t index = 0; index < kMaxSelectionAnnotations + 10; ++index) {
        selection["annotations"].push_back(json{
            {"id", "annotation-" + std::to_string(index)},
            {"text", "note"},
        });
    }

    auto meta = sanitized_selection_context_meta(selection);
    ASSERT_TRUE(meta.has_value());
    ASSERT_TRUE((*meta).contains("annotations"));
    EXPECT_EQ((*meta)["annotations"].size(), kMaxSelectionAnnotations);
    const auto clipped = (*meta)["annotations"][0]["text"].get<std::string>();
    EXPECT_EQ(clipped.size(),
              kMaxSelectionAnnotationChars +
                  std::string("\n[Annotation truncated]").size());
    EXPECT_NE(clipped.find("[Annotation truncated]"), std::string::npos);
}

// 场景：UTF-8 多字节字符恰好跨过字节上限时，服务端不能留下损坏的 JSON。
TEST(SelectionContextAnnotation, KeepsTruncatedAnnotationsValidUtf8) {
    auto selection = annotated_selection();
    selection["annotations"] = json::array({
        json{
            {"id", "utf8-boundary"},
            {"text",
             std::string(kMaxSelectionAnnotationChars - 1, 'x') + "批"},
        },
    });

    auto meta = sanitized_selection_context_meta(selection);
    ASSERT_TRUE(meta.has_value());
    ASSERT_EQ((*meta)["annotations"].size(), 1u);
    const auto clipped =
        (*meta)["annotations"][0]["text"].get<std::string>();
    EXPECT_EQ(clipped.find("批"), std::string::npos);
    EXPECT_NE(clipped.find("[Annotation truncated]"), std::string::npos);
    EXPECT_NO_THROW(static_cast<void>(meta->dump()));
}

// 场景：批注的客户端标识和时间字段也必须有界，避免绕过正文长度限制膨胀会话。
TEST(SelectionContextAnnotation, BoundsAnnotationMetadata) {
    auto selection = annotated_selection();
    selection["annotations"] = json::array({
        json{
            {"id", std::string(400, 'i')},
            {"text", "bounded metadata"},
            {"created_at", std::string(100, 't')},
        },
    });

    auto meta = sanitized_selection_context_meta(selection);
    ASSERT_TRUE(meta.has_value());
    ASSERT_EQ((*meta)["annotations"].size(), 1u);
    EXPECT_EQ(
        (*meta)["annotations"][0]["id"].get<std::string>().size(),
        256u);
    EXPECT_EQ(
        (*meta)["annotations"][0]["created_at"].get<std::string>().size(),
        64u);
}

// 场景：模型收到选中文本及全部批注，但用户实际输入仍由 display_text 单独展示。
TEST(SelectionContextAnnotation, ExpandsHiddenProviderPromptWithAnnotations) {
    auto selection = annotated_selection();
    selection["annotations"].push_back(json{
        {"id", "annotation-b"},
        {"text", "还要补一个边界测试"},
    });

    const auto context =
        build_selection_prompt_context(json::array({selection}));
    ASSERT_EQ(context.meta.size(), 1u);
    EXPECT_NE(context.prompt.find("Source: N:\\project\\answer.cpp:8"),
              std::string::npos);
    EXPECT_NE(context.prompt.find("Text:\nconst answer = 42;"),
              std::string::npos);
    EXPECT_NE(context.prompt.find("Annotations:\n1. 这里需要解释为什么是 42"),
              std::string::npos);
    EXPECT_NE(context.prompt.find("2. 还要补一个边界测试"),
              std::string::npos);

    const auto augmented =
        build_selection_augmented_prompt(context, "请按批注修改");
    EXPECT_NE(augmented.find(context.prompt), std::string::npos);
    EXPECT_NE(augmented.find("User request:\n请按批注修改"), std::string::npos);
}

// 场景：旧版普通引用没有 selected_text、offset 或 annotations 时仍可正常发送。
TEST(SelectionContextAnnotation, KeepsLegacyPlainSelectionCompatible) {
    const json legacy = {
        {"type", "selection"},
        {"text", "legacy text"},
        {"label", "legacy.cpp:3"},
        {"source", json{
            {"path", "legacy.cpp"},
            {"start_line", 3},
            {"end_line", 3},
        }},
    };

    const auto context =
        build_selection_prompt_context(json::array({legacy}));
    ASSERT_EQ(context.meta.size(), 1u);
    EXPECT_EQ(context.meta[0]["selected_text"], "legacy text");
    EXPECT_FALSE(context.meta[0].contains("annotations"));
    EXPECT_NE(context.prompt.find("Text:\nlegacy text"), std::string::npos);
    EXPECT_EQ(context.prompt.find("Annotations:"), std::string::npos);
}

} // namespace
} // namespace acecode::web
