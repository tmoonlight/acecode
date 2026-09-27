#pragma once

#include "image/image_processor.hpp"
#include "utils/sha256.hpp"
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace theme_test {
inline std::string png() {
    acecode::image::ImageNormalizeOptions options;
    options.force_png = true;
    const auto result = acecode::image::normalize_image_bytes("P6\n4 2\n255\n" + std::string(24, 'x'), "", options);
    if (!result.ok) throw std::runtime_error(result.error);
    return result.bytes;
}

inline nlohmann::json definition(const std::string& image, const std::string& id = "ai-example",
                                  const std::string& version = "1.0.0") {
    nlohmann::json colors = nlohmann::json::object();
    for (const auto* key : {"bg", "surface", "surface-alt", "surface-hi", "shell-hi", "shell-bg",
        "border", "border-soft", "fg", "fg-2", "fg-mute", "accent", "accent-bg", "accent-soft",
        "ok", "ok-bg", "ok-border", "warn", "warn-bg", "danger", "danger-bg", "code-bg", "code-fg",
        "code-line", "selection", "on-selection", "send-bg", "send-fg"}) colors[key] = "#ABCDEF";
    return {{"schema_version", 1}, {"id", id}, {"version", version}, {"name", "初号机 / 自定义"}, {"mode", "light"},
        {"colors", colors}, {"background", {{"bytes", image.size()}, {"sha256", acecode::sha256_hex(image)}}},
        {"thumbnail", {{"bytes", image.size()}, {"sha256", acecode::sha256_hex(image)}}}};
}
}
