#include <gtest/gtest.h>

#include "provider/anthropic_provider.hpp"
#include "provider/grok_responses.hpp"
#include "provider/openai_provider.hpp"
#include "provider/provider_factory.hpp"
#include "session/attachment_store.hpp"
#include "session/output_attachments.hpp"
#include "session/tool_result_storage.hpp"
#include "tool/computer_use_tool.hpp"
#include "tool/tool_executor.hpp"
#include "utils/base64.hpp"
#include "utils/utf8_path.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <random>
#include <string>

namespace {

using acecode::ChatMessage;
using Json = nlohmann::json;

class TestOpenAiProvider : public acecode::OpenAiCompatProvider {
public:
    TestOpenAiProvider() : OpenAiCompatProvider("https://unused.invalid/v1", "", "test") {}
    using OpenAiCompatProvider::build_request_body;
};

ChatMessage calls(std::initializer_list<const char*> ids) {
    ChatMessage message;
    message.role = "assistant";
    message.tool_calls = Json::array();
    for (const auto* id : ids) {
        message.tool_calls.push_back(Json{
            {"id", id}, {"type", "function"},
            {"function", {{"name", "computer_get_window_state"}, {"arguments", "{}"}}},
        });
    }
    return message;
}

std::string all_text(const Json& content) {
    if (content.is_string()) return content.get<std::string>();
    std::string text;
    if (content.is_array()) {
        for (const auto& part : content) {
            if (part.is_object() && part.value("type", std::string{}) == "text") {
                text += part.value("text", std::string{}) + "\n";
            }
        }
    }
    return text;
}

class ToolImageFeedback : public testing::Test {
protected:
    void SetUp() override {
        directory = std::filesystem::temp_directory_path() /
            ("acecode_tool_images_" + std::to_string(std::random_device{}()));
        std::filesystem::create_directories(directory);
        const auto bytes = acecode::base64_decode(png_base64);
        ASSERT_TRUE(bytes.has_value());
        std::string error;
        auto saved = acecode::save_attachment(
            acecode::path_to_utf8(directory), "tool-images", "window.png",
            "image/png", *bytes, &error);
        ASSERT_TRUE(saved.has_value()) << error;
        record = *saved;
    }

    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }

    ChatMessage screenshot(const std::string& id = "shot", int count = 1) const {
        acecode::ToolResult result;
        result.success = true;
        result.output = "Window 42; observation=state-1; screenshot=1; width=1; height=1";
        for (int i = 0; i < count; ++i) {
            result.attachments.push_back(acecode::attachment_to_json(record));
        }
        return acecode::ToolExecutor::format_tool_result(id, result);
    }

    std::filesystem::path directory;
    acecode::AttachmentRecord record;
    const std::string png_base64 =
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR4nGP4z8AAAAMBAQDJ/pLvAAAAAElFTkSuQmCC";
};

TEST_F(ToolImageFeedback, OpenAiImagesFollowAllSiblingResultsWithoutChangingHistory) {
    TestOpenAiProvider provider;
    const auto shot = screenshot("shot", 2);
    ChatMessage other;
    other.role = "tool";
    other.tool_call_id = "other";
    other.content = "Other tool completed";
    ChatMessage user;
    user.role = "user";
    user.content = "Continue";
    const std::vector<ChatMessage> history{calls({"shot", "other"}), shot, other, user};

    for (const bool stream : {false, true}) {
        const auto body = provider.build_request_body(history, {}, stream);
        const auto& messages = body["messages"];
        ASSERT_EQ(messages.size(), 5u);
        EXPECT_EQ(messages[0]["role"], "assistant");
        EXPECT_EQ(messages[1]["role"], "tool");
        EXPECT_EQ(messages[1]["content"], shot.content);
        EXPECT_EQ(messages[2]["role"], "tool");
        EXPECT_EQ(messages[2]["tool_call_id"], "other");
        EXPECT_EQ(messages[3]["role"], "user");
        ASSERT_EQ(messages[3]["content"].size(), 3u);
        EXPECT_NE(messages[3]["content"][0]["text"].get<std::string>().find(
            "tool_call_id=shot"), std::string::npos);
        for (size_t i = 1; i <= 2; ++i) {
            EXPECT_EQ(messages[3]["content"][i]["type"], "image_url");
            EXPECT_EQ(messages[3]["content"][i]["image_url"]["url"],
                "data:image/png;base64," + png_base64);
        }
        EXPECT_EQ(messages[4]["content"], "Continue");
        EXPECT_EQ(messages[1]["content"].get<std::string>().find(png_base64), std::string::npos);
    }
    EXPECT_EQ(history.size(), 4u);
    EXPECT_EQ(history[1].role, "tool");
    EXPECT_EQ(history[1].content_parts, shot.content_parts);
}

TEST_F(ToolImageFeedback, PersistedSurfaceImagesRetainTheirSourcesWhenMiddleImageIsMissing) {
    Json descriptors = Json::array();
    Json screenshots = Json::array();
    const std::string observation = "observation-with-related-surfaces";
    for (int i = 0; i < 3; ++i) {
        const std::string id = "surface-" + std::to_string(i);
        const Json geometry{{"width", 1}, {"height", 1}, {"native_width", 2}, {"native_height", 2},
            {"originX", -200 + 30 * i}, {"originY", 40 + 20 * i}, {"scaleX", 2}, {"scaleY", 2}};
        descriptors.push_back({{"name", "same-name.png"}, {"mime_type", "image/png"},
            {"data_url", "data:image/png;base64," + png_base64},
            {"metadata", {{"computer_use", {{"observation_id", observation}, {"screenshot_id", id},
                {"window", 42 + i}, {"geometry", geometry}, {"ignored", "must not persist"}}},
                {"unrelated", "must not persist"}}}});
        auto screenshot = geometry;
        screenshot["id"] = id;
        screenshot["zIndex"] = i;
        screenshots.push_back(std::move(screenshot));
    }
    const auto materialized = acecode::materialize_output_attachments(
        descriptors, acecode::path_to_utf8(directory), "surfaces");
    ASSERT_TRUE(materialized.warnings.empty());
    ASSERT_EQ(materialized.attachments.size(), 3u);
    for (int i = 0; i < 3; ++i) {
        const auto stored = acecode::load_attachment(acecode::path_to_utf8(directory), "surfaces",
            materialized.attachments[i]["id"].get<std::string>());
        ASSERT_TRUE(stored.has_value());
        EXPECT_EQ(stored->metadata["computer_use"]["observation_id"], observation);
        EXPECT_EQ(stored->metadata["computer_use"]["screenshot_id"], screenshots[i]["id"]);
        EXPECT_EQ(stored->metadata["computer_use"]["geometry"], descriptors[i]["metadata"]["computer_use"]["geometry"]);
        EXPECT_FALSE(stored->metadata["computer_use"].contains("ignored"));
        EXPECT_FALSE(stored->metadata.contains("unrelated"));
    }
    const Json observation_output{{"observation_id", observation}, {"window", {{"id", 42}, {"pid", 9}}},
        {"screenshots", screenshots}, {"accessibility", {{"tree", std::string(60000, 't')}}}};
    acecode::ToolResult result{acecode::format_computer_use_output(observation_output), true};
    const auto full_output = result.output;
    result.attachments = materialized.attachments;
    ASSERT_TRUE(acecode::prepare_tool_result_for_delivery(result, "computer_get_window_state", "surfaces-call",
        acecode::path_to_utf8(directory / "tool-results")));
    std::ifstream persisted(acecode::path_from_utf8(acecode::persisted_output_filepath(result.output)), std::ios::binary);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(persisted), std::istreambuf_iterator<char>()), full_output);
    ASSERT_TRUE(std::filesystem::remove(acecode::path_from_utf8(result.attachments[1]["path"].get<std::string>())));
    const auto message = acecode::ToolExecutor::format_tool_result("surfaces-call", result);
    const auto unchanged_parts = message.content_parts;
    ChatMessage sibling;
    sibling.role = "tool";
    sibling.tool_call_id = "other";
    sibling.content = "Other result";
    const std::vector<ChatMessage> history{calls({"surfaces-call", "other"}), message, sibling};

    const auto assert_sources = [&](const Json& content, const std::string& image_type) {
        std::vector<std::string> ids;
        for (std::size_t i = 0; i < content.size(); ++i) {
            if (content[i].value("type", std::string{}) != image_type) continue;
            ASSERT_GT(i, 0u);
            ASSERT_EQ(content[i - 1]["type"], "text");
            const auto label = content[i - 1]["text"].get<std::string>();
            const std::string prefix = "[Computer Use screenshot source]\n";
            ASSERT_EQ(label.find(prefix), 0u);
            const auto source = Json::parse(label.substr(prefix.size()));
            EXPECT_EQ(source["observation_id"], observation);
            EXPECT_EQ(source["geometry"]["width"], 1);
            EXPECT_EQ(source["geometry"]["height"], 1);
            const auto id = source["screenshot_id"].get<std::string>();
            EXPECT_EQ(source["geometry"]["originX"], id == "surface-0" ? -200 : -140);
            ids.push_back(id);
        }
        EXPECT_EQ(ids, (std::vector<std::string>{"surface-0", "surface-2"}));
    };
    TestOpenAiProvider openai;
    const auto openai_messages = openai.build_request_body(history, {}, false)["messages"];
    ASSERT_EQ(openai_messages.size(), 4u);
    EXPECT_EQ(openai_messages[1]["role"], "tool");
    EXPECT_EQ(openai_messages[2]["tool_call_id"], "other");
    EXPECT_EQ(openai_messages[3]["role"], "user");
    EXPECT_NE(openai_messages[1]["content"].get<std::string>().find("surface-1"), std::string::npos);
    EXPECT_NE(openai_messages[1]["content"].get<std::string>().find("Attached image unavailable"), std::string::npos);
    assert_sources(openai_messages[3]["content"], "image_url");

    acecode::AnthropicProvider anthropic("https://unused.invalid", "", "test");
    const auto anthropic_messages = anthropic.build_request_body(history, {}, false)["messages"];
    ASSERT_EQ(anthropic_messages.size(), 2u);
    ASSERT_EQ(anthropic_messages[1]["content"].size(), 2u);
    const auto& tool_content = anthropic_messages[1]["content"][0]["content"];
    EXPECT_NE(all_text(tool_content).find("Attached image unavailable"), std::string::npos);
    assert_sources(tool_content, "image");
    EXPECT_EQ(message.content_parts, unchanged_parts);
    EXPECT_EQ(message.content.find(png_base64), std::string::npos);

    openai.set_vision_routing(false, false);
    anthropic.set_vision_routing(false, false);
    const auto nonvision_openai = openai.build_request_body(history, {}, false);
    const auto nonvision_anthropic = anthropic.build_request_body(history, {}, false);
    EXPECT_EQ(nonvision_openai.dump().find(png_base64), std::string::npos);
    EXPECT_EQ(nonvision_anthropic.dump().find(png_base64), std::string::npos);
    EXPECT_NE(nonvision_openai.dump().find("surface-2"), std::string::npos);
    EXPECT_NE(nonvision_anthropic.dump().find("surface-2"), std::string::npos);
}

TEST_F(ToolImageFeedback, ComputerScreenshotMetadataRequiresMatchingImageGeometry) {
    const Json computer{{"observation_id", "observation"}, {"screenshot_id", "surface"},
        {"geometry", {{"width", 2}, {"height", 1}}}};
    Json descriptor{{"name", "screen.png"}, {"mime_type", "image/png"},
        {"data_url", "data:image/png;base64," + png_base64}, {"metadata", {{"computer_use", computer}}}};
    const auto rejected = acecode::materialize_output_attachments(
        Json::array({descriptor}), acecode::path_to_utf8(directory), "mismatch");
    EXPECT_TRUE(rejected.attachments.empty());
    ASSERT_EQ(rejected.warnings.size(), 1u);
    EXPECT_NE(rejected.warnings.front().find("dimensions"), std::string::npos);
    descriptor["metadata"] = {{"ordinary_attachment_field", "unchanged"}};
    const auto ordinary = acecode::materialize_output_attachments(
        Json::array({descriptor}), acecode::path_to_utf8(directory), "ordinary");
    ASSERT_EQ(ordinary.attachments.size(), 1u);
    EXPECT_TRUE(ordinary.warnings.empty());
    EXPECT_FALSE(ordinary.attachments[0].contains("metadata"));
}

TEST_F(ToolImageFeedback, PointerMetadataSurvivesMaterializationStorageAndBothProviders) {
    const std::vector<Json> cursors{
        {{"visible", true}, {"source", "agent"}, {"x", 0.25}, {"y", 0.75},
         {"hotspot_x", 0.125}, {"hotspot_y", 0.25}, {"width", 0.5}, {"height", 0.875}},
        {{"visible", true}, {"source", "system"}, {"x", 0}, {"y", 0.5},
         {"hotspot_x", 7.5}, {"hotspot_y", 9}, {"width", 50}, {"height", 25}},
        {{"visible", false}},
    };
    Json descriptors = Json::array();
    for (std::size_t index = 0; index < cursors.size(); ++index) {
        auto cursor = cursors[index];
        cursor["untrusted_extra"] = "must not persist";
        const Json computer{{"observation_id", "pointer-observation"}, {"screenshot_id", "pointer-" + std::to_string(index)},
            {"window", 42}, {"geometry", {{"width", 1}, {"height", 1}}}, {"cursor", cursor}};
        descriptors.push_back({{"name", "pointer.png"}, {"mime_type", "image/png"},
            {"data_url", "data:image/png;base64," + png_base64}, {"metadata", {{"computer_use", computer}}}});
    }
    const auto materialized = acecode::materialize_output_attachments(
        descriptors, acecode::path_to_utf8(directory), "pointer-images");
    ASSERT_TRUE(materialized.warnings.empty());
    ASSERT_EQ(materialized.attachments.size(), cursors.size());
    for (std::size_t index = 0; index < cursors.size(); ++index) {
        const auto stored = acecode::load_attachment(acecode::path_to_utf8(directory), "pointer-images",
            materialized.attachments[index]["id"].get<std::string>());
        ASSERT_TRUE(stored.has_value());
        EXPECT_EQ(stored->metadata["computer_use"]["cursor"], cursors[index]);
    }
    acecode::ToolResult result{"Observed pointer", true};
    result.attachments = materialized.attachments;
    const auto message = acecode::ToolExecutor::format_tool_result("pointer-call", result);
    const std::vector<ChatMessage> history{calls({"pointer-call"}), message};
    TestOpenAiProvider openai;
    acecode::AnthropicProvider anthropic("https://unused.invalid", "", "test");
    const auto openai_messages = openai.build_request_body(history, {}, false)["messages"];
    const auto anthropic_messages = anthropic.build_request_body(history, {}, false)["messages"];
    ASSERT_EQ(openai_messages.size(), 3u);
    ASSERT_EQ(anthropic_messages.size(), 2u);
    const auto openai_text = all_text(openai_messages[2]["content"]);
    const auto anthropic_text = all_text(anthropic_messages[1]["content"][0]["content"]);
    for (const auto& cursor : cursors) {
        EXPECT_NE(openai_text.find(cursor.dump()), std::string::npos);
        EXPECT_NE(anthropic_text.find(cursor.dump()), std::string::npos);
    }
    EXPECT_EQ(openai_text.find("untrusted_extra"), std::string::npos);
    EXPECT_EQ(anthropic_text.find("untrusted_extra"), std::string::npos);
}

TEST_F(ToolImageFeedback, InvalidPointerMetadataIsOmittedWithoutLosingTheScreenshot) {
    const Json valid{{"visible", true}, {"source", "agent"}, {"x", 0.25}, {"y", 0.75},
        {"hotspot_x", 0.125}, {"hotspot_y", 0.25}, {"width", 0.5}, {"height", 0.875}};
    std::vector<Json> invalid{Json::array(), Json{{"visible", "true"}}, Json{{"visible", true}}};
    for (const auto& value : {Json("other"), Json(10), Json(nullptr)}) {
        auto cursor = valid;
        cursor["source"] = value;
        invalid.push_back(std::move(cursor));
    }
    for (const char* key : {"x", "y", "hotspot_x", "hotspot_y", "width", "height"}) {
        auto cursor = valid;
        cursor[key] = std::numeric_limits<double>::infinity();
        invalid.push_back(std::move(cursor));
        cursor = valid;
        cursor.erase(key);
        invalid.push_back(std::move(cursor));
    }
    for (const auto& item : std::vector<std::pair<std::string, double>>{
            {"x", -0.25}, {"y", 1}, {"width", 0}, {"height", -1}, {"hotspot_x", 0.5}, {"hotspot_y", -0.25}}) {
        auto cursor = valid;
        cursor[item.first] = item.second;
        invalid.push_back(std::move(cursor));
    }
    Json descriptors = Json::array();
    for (std::size_t index = 0; index < invalid.size(); ++index) {
        const Json computer{{"observation_id", "invalid-pointer-observation"}, {"screenshot_id", "invalid-" + std::to_string(index)},
            {"geometry", {{"width", 1}, {"height", 1}}}, {"cursor", invalid[index]}};
        descriptors.push_back({{"name", "pointer.png"}, {"mime_type", "image/png"},
            {"data_url", "data:image/png;base64," + png_base64}, {"metadata", {{"computer_use", computer}}}});
    }
    const auto materialized = acecode::materialize_output_attachments(
        descriptors, acecode::path_to_utf8(directory), "invalid-pointer-images");
    ASSERT_TRUE(materialized.warnings.empty());
    ASSERT_EQ(materialized.attachments.size(), invalid.size());
    for (const auto& attachment : materialized.attachments) {
        const auto stored = acecode::load_attachment(acecode::path_to_utf8(directory), "invalid-pointer-images",
            attachment["id"].get<std::string>());
        ASSERT_TRUE(stored.has_value());
        EXPECT_FALSE(stored->metadata["computer_use"].contains("cursor"));
        EXPECT_EQ(stored->metadata["computer_use"]["observation_id"], "invalid-pointer-observation");
    }
}

TEST_F(ToolImageFeedback, OpenAiRepairsInterruptedSiblingBeforeImageMessage) {
    TestOpenAiProvider provider;
    ChatMessage system;
    system.role = "system";
    system.content = "Request-local instructions";
    const auto body = provider.build_request_body(
        {calls({"shot", "interrupted"}), screenshot(), system}, {}, false);
    const auto& messages = body["messages"];
    ASSERT_EQ(messages.size(), 5u);
    EXPECT_EQ(messages[0]["role"], "system");
    EXPECT_EQ(messages[1]["role"], "assistant");
    EXPECT_EQ(messages[2]["tool_call_id"], "shot");
    EXPECT_EQ(messages[3]["tool_call_id"], "interrupted");
    EXPECT_EQ(messages[4]["role"], "user");
    EXPECT_EQ(messages[4]["content"][1]["type"], "image_url");
}

TEST_F(ToolImageFeedback, OpenAiBatchImagesStayWithTheirRowsWhenSystemMessagesMove) {
    TestOpenAiProvider provider;
    ChatMessage system;
    system.role = "system";
    system.content = "Request-local context";
    const auto body = provider.build_request_body({
        calls({"first"}), screenshot("first"), system,
        calls({"second"}), screenshot("second", 2), system,
    }, {}, false);
    const auto& messages = body["messages"];
    ASSERT_EQ(messages.size(), 7u);
    EXPECT_EQ(messages[0]["role"], "system");
    EXPECT_EQ(messages[2]["tool_call_id"], "first");
    ASSERT_EQ(messages[3]["content"].size(), 2u);
    EXPECT_NE(all_text(messages[3]["content"]).find("tool_call_id=first"), std::string::npos);
    EXPECT_EQ(messages[3]["content"][1]["type"], "image_url");
    EXPECT_EQ(messages[5]["tool_call_id"], "second");
    ASSERT_EQ(messages[6]["content"].size(), 3u);
    EXPECT_NE(all_text(messages[6]["content"]).find("tool_call_id=second"), std::string::npos);
    EXPECT_EQ(messages[6]["content"][1]["type"], "image_url");
    EXPECT_EQ(messages[6]["content"][2]["type"], "image_url");
}

TEST_F(ToolImageFeedback, ReusedCallIdsKeepImagesWithTheirOwnResults) {
    TestOpenAiProvider provider;
    // 跨模型步骤复用 ID 时保留两轮截图，同时防止按旧 ID 串用图片。
    for (const bool stream : {false, true}) {
        const auto first_body = provider.build_request_body(
            {calls({"shot"}), screenshot("shot")}, {}, stream);
        const auto body = provider.build_request_body({
            calls({"shot"}), screenshot("shot"),
            calls({"shot", "later"}), screenshot("shot", 3), screenshot("later", 2),
        }, {}, stream);
        const auto& messages = body["messages"];
        ASSERT_EQ(messages.size(), 7u);
        for (std::size_t i = 0; i < 3; ++i) {
            EXPECT_EQ(messages[i], first_body["messages"][i]);
        }
        ASSERT_EQ(messages[3]["tool_calls"].size(), 2u);
        const auto reused_id = messages[3]["tool_calls"][0]["id"].get<std::string>();
        EXPECT_NE(reused_id, "shot");
        EXPECT_NE(reused_id, "later");
        EXPECT_EQ(messages[3]["tool_calls"][1]["id"], "later");
        EXPECT_EQ(messages[4]["tool_call_id"], reused_id);
        EXPECT_EQ(messages[5]["tool_call_id"], "later");
        ASSERT_EQ(messages[6]["content"].size(), 7u);
        const auto& images = messages[6]["content"];
        EXPECT_NE(images[0]["text"].get<std::string>().find(
            "tool_call_id=" + reused_id), std::string::npos);
        EXPECT_NE(images[4]["text"].get<std::string>().find(
            "tool_call_id=later"), std::string::npos);
        EXPECT_EQ(all_text(images).find("tool_call_id=shot"), std::string::npos);
        for (const auto i : {1, 2, 3, 5, 6}) {
            EXPECT_EQ(images[i]["type"], "image_url");
            EXPECT_EQ(images[i]["image_url"]["url"], "data:image/png;base64," + png_base64);
        }
    }
}

TEST_F(ToolImageFeedback, ReusedCallWithoutResultDoesNotReplayEarlierImages) {
    TestOpenAiProvider provider;
    const auto body = provider.build_request_body({
        calls({"shot"}), screenshot("shot"), calls({"shot"}),
    }, {}, false);
    const auto& messages = body["messages"];
    ASSERT_EQ(messages.size(), 5u);
    ASSERT_EQ(messages[2]["content"].size(), 2u);
    EXPECT_EQ(messages[2]["content"][1]["type"], "image_url");
    const auto& reused_id = messages[3]["tool_calls"][0]["id"];
    EXPECT_NE(reused_id, "shot");
    EXPECT_EQ(messages.back()["role"], "tool");
    EXPECT_EQ(messages.back()["tool_call_id"], reused_id);
    const auto text = all_text(messages.back()["content"]);
    EXPECT_NE(text.find("outcome is unknown"), std::string::npos);
    EXPECT_EQ(text.find(png_base64), std::string::npos);
}

TEST_F(ToolImageFeedback, OpenAiNonVisionKeepsObservationAndDoesNotEncodeImages) {
    TestOpenAiProvider provider;
    provider.set_vision_routing(false, true);
    const auto shot = screenshot();
    const auto body = provider.build_request_body({calls({"shot"}), shot}, {}, false);
    ASSERT_EQ(body["messages"].size(), 2u);
    const auto text = all_text(body["messages"][1]["content"]);
    EXPECT_NE(text.find(shot.content), std::string::npos);
    EXPECT_NE(text.find("cannot inspect images"), std::string::npos);
    EXPECT_NE(text.find("vision_analyze"), std::string::npos);
    EXPECT_EQ(body.dump().find(png_base64), std::string::npos);
}

TEST_F(ToolImageFeedback, UnreadableImagesKeepToolTextAndExplicitFallback) {
    std::filesystem::remove(acecode::path_from_utf8(record.path));
    const auto shot = screenshot();
    TestOpenAiProvider openai;
    acecode::AnthropicProvider anthropic("https://unused.invalid/v1", "", "test");
    const auto openai_body = openai.build_request_body({calls({"shot"}), shot}, {}, false);
    ASSERT_EQ(openai_body["messages"].size(), 2u);
    const auto openai_text = all_text(openai_body["messages"][1]["content"]);
    EXPECT_NE(openai_text.find(shot.content), std::string::npos);
    EXPECT_NE(openai_text.find("Attached image unavailable"), std::string::npos);
    const auto anthropic_body = anthropic.build_request_body({calls({"shot"}), shot}, {});
    const auto anthropic_text = all_text(
        anthropic_body["messages"][1]["content"][0]["content"]);
    EXPECT_NE(anthropic_text.find(shot.content), std::string::npos);
    EXPECT_NE(anthropic_text.find("Attached image unavailable"), std::string::npos);
    EXPECT_EQ(openai_body.dump().find(png_base64), std::string::npos);
    EXPECT_EQ(anthropic_body.dump().find(png_base64), std::string::npos);
}

TEST_F(ToolImageFeedback, InvalidToolResultsCannotIntroduceImageMessages) {
    TestOpenAiProvider provider;
    ChatMessage text_result;
    text_result.role = "tool";
    text_result.tool_call_id = "shot";
    text_result.content = "Original result";
    const auto body = provider.build_request_body(
        {screenshot("orphan"), calls({"shot"}), text_result, screenshot(), screenshot("unexpected")},
        {}, false);
    ASSERT_EQ(body["messages"].size(), 2u);
    EXPECT_EQ(body["messages"][1]["content"], "Original result");
    EXPECT_EQ(body.dump().find(png_base64), std::string::npos);
}

TEST_F(ToolImageFeedback, GrokResponsesKeepsImagesAfterFunctionOutputs) {
    TestOpenAiProvider provider;
    const std::vector<ChatMessage> history{calls({"shot", "other"}), screenshot(), screenshot("other")};
    const auto chat = provider.build_request_body(history, {}, false);
    std::string error;
    const auto body = acecode::build_grok_responses_request(chat, &history, &error);
    ASSERT_TRUE(error.empty()) << error;
    const auto& input = body["input"];
    ASSERT_EQ(input.size(), 5u);
    EXPECT_EQ(input[0]["type"], "function_call");
    EXPECT_EQ(input[1]["type"], "function_call");
    EXPECT_EQ(input[2]["type"], "function_call_output");
    EXPECT_EQ(input[3]["type"], "function_call_output");
    EXPECT_EQ(input[4]["role"], "user");
    EXPECT_EQ(input[4]["content"][1]["type"], "input_image");
    EXPECT_EQ(input[4]["content"][3]["type"], "input_image");
}

TEST_F(ToolImageFeedback, AnthropicImagesRemainInsideBatchedToolResults) {
    acecode::AnthropicProvider provider("https://unused.invalid/v1", "", "test");
    const auto shot = screenshot();
    auto error_result = screenshot("other");
    error_result.metadata["tool_success"] = false;
    error_result.content_parts = Json::array();
    error_result.content = "Target window closed";
    for (const bool stream : {false, true}) {
        const auto body = provider.build_request_body(
            {calls({"shot", "other"}), shot, error_result}, {}, stream);
        ASSERT_EQ(body["messages"].size(), 2u);
        const auto& blocks = body["messages"][1]["content"];
        ASSERT_EQ(blocks.size(), 2u);
        EXPECT_EQ(blocks[0]["tool_use_id"], "shot");
        EXPECT_EQ(blocks[1]["tool_use_id"], "other");
        EXPECT_EQ(blocks[1]["is_error"], true);
        EXPECT_EQ(blocks[1]["content"], "Target window closed");
        const auto& content = blocks[0]["content"];
        ASSERT_EQ(content.size(), 2u);
        EXPECT_EQ(content[0]["text"], shot.content);
        EXPECT_EQ(content[1]["type"], "image");
        EXPECT_EQ(content[1]["source"]["type"], "base64");
        EXPECT_EQ(content[1]["source"]["media_type"], "image/png");
        EXPECT_EQ(content[1]["source"]["data"], png_base64);
    }
}

TEST_F(ToolImageFeedback, AnthropicCapabilityGatePreservesObservation) {
    acecode::AnthropicProvider provider("https://unused.invalid/v1", "", "test");
    provider.set_vision_routing(false, false);
    const auto shot = screenshot();
    const auto body = provider.build_request_body({calls({"shot"}), shot}, {});
    const auto text = all_text(body["messages"][1]["content"][0]["content"]);
    EXPECT_NE(text.find(shot.content), std::string::npos);
    EXPECT_NE(text.find("cannot inspect images"), std::string::npos);
    EXPECT_EQ(body.dump().find(png_base64), std::string::npos);
    EXPECT_FALSE(provider.supports_vision());
}

TEST_F(ToolImageFeedback, AnthropicAlsoAcceptsUserImageAttachments) {
    acecode::AnthropicProvider provider("https://unused.invalid/v1", "", "test");
    auto user = screenshot();
    user.role = "user";
    user.tool_call_id.clear();
    const auto body = provider.build_request_body({user}, {});
    ASSERT_EQ(body["messages"].size(), 1u);
    const auto& content = body["messages"][0]["content"];
    EXPECT_EQ(content[0]["text"], user.content);
    EXPECT_EQ(content[1]["source"]["data"], png_base64);
}

TEST_F(ToolImageFeedback, AnthropicRejectsUnsupportedImageEncodingWithoutLeakingBytes) {
    record.mime_type = "image/tiff";
    acecode::AnthropicProvider provider("https://unused.invalid/v1", "", "test");
    const auto body = provider.build_request_body({calls({"shot"}), screenshot()}, {});
    const auto text = all_text(body["messages"][1]["content"][0]["content"]);
    EXPECT_NE(text.find("unsupported image format"), std::string::npos);
    EXPECT_EQ(body.dump().find(png_base64), std::string::npos);
}

TEST(ToolImageFeedbackFactory, AnthropicVisionRoutingAffectsProviderAndFingerprint) {
    acecode::ModelProfile profile;
    profile.name = "anthropic-images";
    profile.provider = "anthropic";
    profile.model = "claude-test";
    profile.base_url = "https://unused.invalid/v1";
    auto no_vision = acecode::prepare_provider_construction(profile);
    ASSERT_TRUE(no_vision.has_value());
    EXPECT_FALSE(no_vision->construct().provider->supports_vision());
    profile.capabilities = {"vision"};
    auto vision = acecode::prepare_provider_construction(profile);
    ASSERT_TRUE(vision.has_value());
    EXPECT_NE(no_vision->fingerprint(), vision->fingerprint());
    EXPECT_TRUE(vision->construct().provider->supports_vision());
}

} // namespace
