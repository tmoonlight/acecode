#include "composer_attachments.hpp"
#include "attachment_store.hpp"

namespace acecode {

std::string attachment_name_from_json(const nlohmann::json& attachment) {
    return attachment.value("name", std::string{"attachment"});
}

std::string display_prompt_with_attachments(
    const std::string& prompt,
    const std::vector<nlohmann::json>& attachments) {
    std::string display = prompt;
    for (const auto& attachment : attachments) {
        if (!display.empty()) display.push_back('\n');
        const std::string kind = attachment.value("kind", std::string{"file"});
        display += "[";
        display += (kind == "image") ? "Image: " : "File: ";
        display += attachment_name_from_json(attachment);
        display += "]";
    }
    return display;
}

UserInput build_user_input_with_attachments(
    const std::string& prompt,
    const std::string& display_text,
    const std::vector<nlohmann::json>& attachments) {
    UserInput input;
    input.text = prompt;
    input.display_text = display_text;
    input.content_parts = nlohmann::json::array();
    if (!prompt.empty()) {
        input.content_parts.push_back({{"type", "text"}, {"text", prompt}});
    }
    nlohmann::json attachment_meta = nlohmann::json::array();
    for (const auto& attachment : attachments) {
        // 按 MIME + 文件名重新分类(route-attachments-by-capability 1.7),不直接信
        // 持久化的 kind:SVG 等非视觉媒体会被归为 file,避免误走图片 part。
        const std::string part_kind = attachment_kind_for_mime(
            attachment.value("mime_type", std::string{}),
            attachment.value("name", std::string{}));
        input.content_parts.push_back({
            {"type", part_kind == "image" ? "image" : "file"},
            {"attachment", attachment},
        });
        attachment_meta.push_back(attachment);
    }
    if (attachment_meta.empty()) {
        input.content_parts = nlohmann::json::array();
    } else {
        input.metadata["attachments"] = std::move(attachment_meta);
    }
    return input;
}


} // namespace acecode
