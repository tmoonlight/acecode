#include "model_image_payload.hpp"

#include "image/image_processor.hpp"
#include "session/attachment_model_image.hpp"

#include <algorithm>
#include <cctype>

namespace acecode {

bool record_is_vision_image(const AttachmentRecord& record) {
    std::string mime = attachment_mime_for_name(record.name, record.mime_type);
    std::transform(mime.begin(), mime.end(), mime.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (mime == "image/svg+xml") return false;
    return mime.rfind("image/", 0) == 0;
}

std::optional<CompressedModelImage> compressed_model_image(const AttachmentRecord& record) {
    if (!image::model_image_compression_enabled()) return std::nullopt;
    const auto model_image = attachment_image_for_model(record);
    if (!model_image || !model_image->compressed) return std::nullopt;
    return CompressedModelImage{model_image->bytes, model_image->mime_type,
                                attachment_model_image_note(*model_image)};
}

ProviderHistoryRecoveryResult recover_provider_history_with_image_budget(
    const std::vector<ChatMessage>& messages,
    ImageRequestBudget& budget,
    std::size_t max_request_bytes,
    bool model_has_vision) {
    auto recovered = recover_provider_history(messages);
    if (!model_has_vision || !image::model_image_compression_enabled()) return recovered;
    budget.apply(recovered.messages, image_request_budget_limits(max_request_bytes),
        [](const nlohmann::json& part) -> std::size_t {
            const auto attachment = part.find("attachment");
            if (attachment == part.end()) return 0;
            const auto record = attachment_from_json(*attachment);
            if (!record.has_value() || !record_is_vision_image(*record)) return 0;
            return attachment_image_wire_bytes(*record);
        });
    return recovered;
}

} // namespace acecode
