#include "image_compression_handler.hpp"

namespace acecode::web {

nlohmann::json image_compression_snapshot(const ImageCompressionConfig& cfg) {
    return nlohmann::json{{"enabled", cfg.enabled}};
}

bool parse_image_compression_request(
    const nlohmann::json& body,
    const ImageCompressionConfig& current,
    ImageCompressionConfig& out,
    std::string& error) {
    if (!body.is_object()) {
        error = "expected a JSON object";
        return false;
    }
    ImageCompressionConfig next = current;
    if (body.contains("enabled")) {
        if (!body["enabled"].is_boolean()) {
            error = "enabled must be a boolean";
            return false;
        }
        next.enabled = body["enabled"].get<bool>();
    }
    out = next;
    return true;
}

} // namespace acecode::web
