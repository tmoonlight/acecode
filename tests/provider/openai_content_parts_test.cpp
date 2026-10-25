// tests/provider/openai_content_parts_test.cpp
// 覆盖 route-attachments-by-capability 的 provider 序列化 gate(tasks 1.4)。
// openai_content_for_message 是 image/file 附件路由的唯一收口点,这里直接调用它,
// 断言:
//   - 视觉模型能拿到真实 image_url part;
//   - 非视觉模型拿到句柄文本而非图片,且 fallback 措辞随是否存在视觉模型变化;
//   - 非图片附件(text-like / PDF / SVG / 误标成 image 的非图片)只产生按需读取引用,
//     永远不内联正文或产生 image_url;
//   - 「压缩图片」开启时发压缩版并附说明文字,关闭时原图原样发送;请求体预算只在
//     开关开启且模型能看图时生效。
#include <gtest/gtest.h>

#include "image/image_processor.hpp"
#include "provider/model_image_payload.hpp"
#include "provider/openai_provider.hpp"
#include "session/attachment_model_image.hpp"
#include "session/attachment_store.hpp"
#include "utils/base64.hpp"
#include "utils/utf8_path.hpp"

#include <filesystem>
#include <random>
#include <string>

namespace fs = std::filesystem;

namespace {

fs::path temp_project(const std::string& hint) {
    auto dir = fs::temp_directory_path() /
        ("acecode_content_parts_" + hint + "_" +
         std::to_string(std::random_device{}()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

acecode::AttachmentRecord save(const std::string& project_dir,
                               const std::string& name,
                               const std::string& mime,
                               const std::string& bytes) {
    std::string error;
    auto record = acecode::save_attachment(
        project_dir, "sid-content", name, mime, bytes, &error);
    EXPECT_TRUE(record.has_value()) << error;
    return record.value_or(acecode::AttachmentRecord{});
}

acecode::ChatMessage image_part_message(const acecode::AttachmentRecord& record) {
    acecode::ChatMessage msg;
    msg.role = "user";
    msg.content = "look";
    // 故意用 type=image,模拟 UI/历史可能产生的 image part;gate/分类应自行判定。
    msg.content_parts = nlohmann::json::array({
        nlohmann::json{{"type", "text"}, {"text", "look"}},
        nlohmann::json{{"type", "image"}, {"attachment", acecode::attachment_to_json(record)}},
    });
    return msg;
}

bool has_image_url(const nlohmann::json& content) {
    if (!content.is_array()) return false;
    for (const auto& part : content) {
        if (part.is_object() && part.value("type", std::string{}) == "image_url") {
            return true;
        }
    }
    return false;
}

std::string collect_text(const nlohmann::json& content) {
    std::string text;
    if (!content.is_array()) {
        return content.is_string() ? content.get<std::string>() : std::string{};
    }
    for (const auto& part : content) {
        if (part.is_object() && part.value("type", std::string{}) == "text") {
            text += part.value("text", std::string{});
            text += "\n";
        }
    }
    return text;
}

} // namespace

// 场景:视觉模型 + 图片附件 → 真实 image_url part。
TEST(OpenAiContentParts, VisionModelGetsImageUrl) {
    auto dir = temp_project("vision_ok");
    const std::string project_dir = acecode::path_to_utf8(dir);
    auto record = save(project_dir, "shot.png", "image/png", "fake-png-bytes");

    auto content = acecode::openai_content_for_message(
        image_part_message(record), /*model_has_vision=*/true,
        /*any_vision_model_available=*/false);

    EXPECT_TRUE(has_image_url(content)) << content.dump(2);
    fs::remove_all(dir);
}

TEST(OpenAiContentParts, SourcePathTextRemainsVisibleBesideVisionImage) {
    auto dir = temp_project("vision_source_path");
    const std::string project_dir = acecode::path_to_utf8(dir);
    auto record = save(project_dir, "shot.png", "image/png", "fake-png-bytes");
    auto message = image_part_message(record);
    message.content_parts.insert(message.content_parts.begin() + 1, nlohmann::json{
        {"type", "text"},
        {"text", "[Local attachment source]\n{\"absolute_path\":\"C:/repo/shot.png\"}"},
    });

    auto content = acecode::openai_content_for_message(
        message, /*model_has_vision=*/true,
        /*any_vision_model_available=*/false);

    EXPECT_TRUE(has_image_url(content)) << content.dump(2);
    EXPECT_NE(collect_text(content).find("C:/repo/shot.png"), std::string::npos);
    fs::remove_all(dir);
}

// 场景:非视觉模型 + 存在其它视觉模型 → 无图片 payload,fallback 引导用 vision_analyze。
TEST(OpenAiContentParts, NonVisionModelGetsHandlePointingToVisionAnalyze) {
    auto dir = temp_project("no_vision_but_available");
    const std::string project_dir = acecode::path_to_utf8(dir);
    auto record = save(project_dir, "shot.png", "image/png", "fake-png-bytes");

    auto content = acecode::openai_content_for_message(
        image_part_message(record), /*model_has_vision=*/false,
        /*any_vision_model_available=*/true);

    EXPECT_FALSE(has_image_url(content)) << content.dump(2);
    const std::string text = collect_text(content);
    EXPECT_NE(text.find("vision_analyze"), std::string::npos) << text;
    fs::remove_all(dir);
}

// 场景:非视觉模型 + 系统无任何视觉模型 → 无图片 payload,fallback 说明需配置视觉模型。
TEST(OpenAiContentParts, NonVisionModelNoVisionAvailableExplainsConfig) {
    auto dir = temp_project("no_vision_at_all");
    const std::string project_dir = acecode::path_to_utf8(dir);
    auto record = save(project_dir, "shot.png", "image/png", "fake-png-bytes");

    auto content = acecode::openai_content_for_message(
        image_part_message(record), /*model_has_vision=*/false,
        /*any_vision_model_available=*/false);

    EXPECT_FALSE(has_image_url(content)) << content.dump(2);
    const std::string text = collect_text(content);
    EXPECT_NE(text.find("vision"), std::string::npos) << text;
    // 不应误导用户去调用 vision_analyze —— 因为系统根本没有可用视觉模型。
    EXPECT_EQ(text.find("vision_analyze tool"), std::string::npos) << text;
    fs::remove_all(dir);
}

// 场景:text-like 文件(file part)永远不会变成图片。
TEST(OpenAiContentParts, TextFileNeverImage) {
    auto dir = temp_project("textfile");
    const std::string project_dir = acecode::path_to_utf8(dir);
    constexpr const char* kContentSentinel = "ATTACHMENT_BODY_MUST_NOT_BE_INLINE";
    auto record = save(project_dir, "notes.txt", "text/plain", kContentSentinel);

    acecode::ChatMessage msg;
    msg.role = "user";
    msg.content_parts = nlohmann::json::array({
        nlohmann::json{{"type", "file"}, {"attachment", acecode::attachment_to_json(record)}},
    });

    auto content = acecode::openai_content_for_message(msg, true, true);
    EXPECT_FALSE(has_image_url(content)) << content.dump(2);
    const std::string text = collect_text(content);
    EXPECT_NE(text.find("[Attached file reference]"), std::string::npos);
    EXPECT_NE(text.find("notes.txt"), std::string::npos);
    EXPECT_NE(text.find(record.id), std::string::npos);
    EXPECT_NE(text.find(acecode::path_to_utf8_generic(
                  acecode::path_from_utf8(record.path))),
              std::string::npos);
    EXPECT_NE(text.find("read_path"), std::string::npos);
    EXPECT_EQ(text.find(kContentSentinel), std::string::npos);
    fs::remove_all(dir);
}

// 场景:PDF 即便被标成 image part、即便模型支持 vision,也不会变成图片 payload。
TEST(OpenAiContentParts, PdfStaysFileEvenWithVision) {
    auto dir = temp_project("pdf");
    const std::string project_dir = acecode::path_to_utf8(dir);
    auto record = save(project_dir, "doc.pdf", "application/pdf", "%PDF-1.4 fake");

    auto content = acecode::openai_content_for_message(
        image_part_message(record), /*model_has_vision=*/true, true);

    EXPECT_FALSE(has_image_url(content)) << content.dump(2);
    EXPECT_NE(collect_text(content).find("[Attached file reference]"),
              std::string::npos);
    fs::remove_all(dir);
}

// 场景:SVG 虽然 MIME 以 image/ 开头,但被排除出 vision 路由,不产生 image_url。
TEST(OpenAiContentParts, SvgNotSerializedAsImage) {
    auto dir = temp_project("svg");
    const std::string project_dir = acecode::path_to_utf8(dir);
    auto record = save(project_dir, "vec.svg", "image/svg+xml", "<svg></svg>");

    auto content = acecode::openai_content_for_message(
        image_part_message(record), /*model_has_vision=*/true, true);

    EXPECT_FALSE(has_image_url(content)) << content.dump(2);
    fs::remove_all(dir);
}

// 场景:误标 —— attachment 元数据是非图片(application/pdf)却被包成 image part,
// 序列化层必须据 MIME 把它降级为文件句柄,绝不发图片 payload。
TEST(OpenAiContentParts, MislabeledNonImageDowngradedToFile) {
    acecode::ChatMessage msg;
    msg.role = "user";
    // 手工构造误标:kind=image 但 mime 是 application/pdf。
    nlohmann::json attachment = {
        {"id", "att_x"}, {"session_id", "s"}, {"name", "report.pdf"},
        {"kind", "image"}, {"mime_type", "application/pdf"},
        {"path", "x"}, {"blob_url", "/b"}, {"size_bytes", 10},
    };
    msg.content_parts = nlohmann::json::array({
        nlohmann::json{{"type", "image"}, {"attachment", attachment}},
    });

    auto content = acecode::openai_content_for_message(msg, /*model_has_vision=*/true, true);
    EXPECT_FALSE(has_image_url(content)) << content.dump(2);
}

namespace {

// 「压缩图片」是进程级开关,用例结束必须恢复,否则会影响同进程里的其它用例。
struct ScopedImageCompression {
    bool previous = acecode::image::model_image_compression_enabled();
    explicit ScopedImageCompression(bool enabled) {
        acecode::image::set_model_image_compression_enabled(enabled);
    }
    ~ScopedImageCompression() { acecode::image::set_model_image_compression_enabled(previous); }
};

std::string gradient_ppm(int width, int height) {
    std::string bytes = "P6\n" + std::to_string(width) + " " + std::to_string(height) + "\n255\n";
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            bytes.push_back(static_cast<char>((x * 255) / std::max(1, width - 1)));
            bytes.push_back(static_cast<char>((y * 255) / std::max(1, height - 1)));
            bytes.push_back(static_cast<char>(32));
        }
    }
    return bytes;
}

std::string first_image_url(const nlohmann::json& content) {
    for (const auto& part : content) {
        if (part.value("type", std::string{}) == "image_url") {
            return part["image_url"].value("url", std::string{});
        }
    }
    return {};
}

} // namespace

// 场景:「压缩图片」开启,视觉模型收到一张 2400x300 的图片附件(存入时不触发
// 原有的 10 MB / 2560 px 规范化)。
// 期望:image_url 发的是压缩版(长边 2000),图片前面有一段说明原尺寸与压缩后尺寸
// 的文字;用户写的 "look" 仍在。
TEST(OpenAiContentParts, CompressionSendsCompressedImageWithNote) {
    ScopedImageCompression compression(true);
    acecode::clear_attachment_model_image_cache_for_tests();
    auto dir = temp_project("compressed");
    const std::string project_dir = acecode::path_to_utf8(dir);
    const std::string original = gradient_ppm(2400, 300);
    auto record = save(project_dir, "wide.png", "image/png", original);

    auto content = acecode::openai_content_for_message(
        image_part_message(record), /*model_has_vision=*/true, false);

    const std::string text = collect_text(content);
    EXPECT_NE(text.find("look"), std::string::npos);
    EXPECT_NE(text.find("2400x300"), std::string::npos) << text;
    EXPECT_NE(text.find("-> 2000x250"), std::string::npos) << text;
    const std::string url = first_image_url(content);
    ASSERT_FALSE(url.empty()) << content.dump(2);
    EXPECT_NE(url, "data:image/png;base64," + acecode::base64_encode(original));
    acecode::clear_attachment_model_image_cache_for_tests();
    fs::remove_all(dir);
}

// 场景:「压缩图片」关闭,同一张 2400x300 的图片。
// 期望:回到原有行为 —— 原图字节原样发送,没有压缩说明文字。
TEST(OpenAiContentParts, CompressionOffSendsOriginalImage) {
    ScopedImageCompression compression(false);
    acecode::clear_attachment_model_image_cache_for_tests();
    auto dir = temp_project("uncompressed");
    const std::string project_dir = acecode::path_to_utf8(dir);
    const std::string original = gradient_ppm(2400, 300);
    auto record = save(project_dir, "wide.png", "image/png", original);

    auto content = acecode::openai_content_for_message(
        image_part_message(record), /*model_has_vision=*/true, false);

    EXPECT_EQ(collect_text(content).find("Image compressed"), std::string::npos);
    EXPECT_EQ(first_image_url(content),
              "data:image/png;base64," + acecode::base64_encode(original));
    fs::remove_all(dir);
}

// 场景:请求体预算的生效条件。一张图片附件,快照文件不存在,所以发送体积按记录里的
// 60 MiB 估算(超过 OpenAI 兼容协议 47 MiB 的触发线)。
// 期望:「压缩图片」关闭或模型不能看图时消息原样;两者都满足时这张图被换成文件引用。
TEST(OpenAiContentParts, ImageBudgetRequiresCompressionAndVision) {
    std::vector<acecode::ChatMessage> messages;
    acecode::AttachmentRecord record;
    record.id = "att_budget";
    record.name = "a.png";
    record.mime_type = "image/png";
    record.path = "missing.png";
    record.size_bytes = 60u * 1024u * 1024u;
    messages.push_back(image_part_message(record));
    const auto before = messages.front().content_parts;
    const auto project = [&](bool compression, bool vision) {
        ScopedImageCompression scoped(compression);
        acecode::ImageRequestBudget budget;
        return acecode::recover_provider_history_with_image_budget(
            messages, budget, acecode::kOpenAiCompatMaxRequestBytes, vision).messages;
    };
    EXPECT_EQ(project(false, true).front().content_parts, before);
    EXPECT_EQ(project(true, false).front().content_parts, before);
    const auto budgeted = project(true, true);
    EXPECT_EQ(budgeted.front().content_parts[1].value("type", std::string{}), "file");
}

// 分类源头:attachment_kind_for_mime 把 SVG 归为 file,其它图片仍为 image。
TEST(OpenAiContentParts, AttachmentKindClassification) {
    EXPECT_EQ(acecode::attachment_kind_for_mime("image/png", "a.png"), "image");
    EXPECT_EQ(acecode::attachment_kind_for_mime("image/svg+xml", "a.svg"), "file");
    EXPECT_EQ(acecode::attachment_kind_for_mime("application/pdf", "a.pdf"), "file");
    EXPECT_EQ(acecode::attachment_kind_for_mime("text/plain", "a.txt"), "file");
}
