#include "show_image_tool.hpp"

#include "image/image_processor.hpp"
#include "llm/tool_icons.hpp"
#include "tool/tool_args_parser.hpp"
#include "tool/tool_errors.hpp"
#include "utils/base64.hpp"
#include "utils/utf8_path.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>

namespace acecode {

namespace {

std::string ascii_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string image_mime_for_path(const std::filesystem::path& path) {
    const std::string extension = ascii_lower(path_to_utf8(path.extension()));
    if (extension == ".png") return "image/png";
    if (extension == ".jpg" || extension == ".jpeg") return "image/jpeg";
    if (extension == ".gif") return "image/gif";
    if (extension == ".webp") return "image/webp";
    if (extension == ".bmp") return "image/bmp";
    return {};
}

// `crop` is {x, y, width, height} in source pixels; missing means the whole image.
std::optional<image::ImageCropRegion> crop_region_arg(const ToolArgsParser& parser,
                                                      std::string* error) {
    const auto crop = parser.get<nlohmann::json>("crop");
    if (!crop.has_value() || crop->is_null()) return std::nullopt;
    image::ImageCropRegion region;
    const auto field = [&](const char* key, int* out) {
        if (!crop->is_object() || !crop->contains(key) || !(*crop)[key].is_number()) return false;
        const double value = (*crop)[key].get<double>();
        if (value < -1e9 || value > 1e9) return false;
        *out = static_cast<int>(value);
        return true;
    };
    if (!field("x", &region.x) || !field("y", &region.y) || !field("width", &region.width) ||
        !field("height", &region.height)) {
        *error = "crop needs numeric x, y, width and height in source pixels";
        return std::nullopt;
    }
    return region;
}

ToolResult execute_show_image(const std::string& arguments_json,
                              const ToolContext& ctx) {
    ToolArgsParser parser(arguments_json);
    if (parser.has_error()) return ToolResult{parser.error(), false};

    const std::string image_path =
        parser.get_or<std::string>("image_path", std::string{});
    if (image_path.empty()) {
        return ToolResult{ToolErrors::missing_parameter("image_path"), false};
    }

    namespace fs = std::filesystem;
    fs::path resolved = path_from_utf8(image_path);
    if (resolved.is_relative() && !ctx.cwd.empty()) {
        resolved = path_from_utf8(ctx.cwd) / resolved;
    }

    std::error_code ec;
    if (resolved.is_relative()) {
        resolved = fs::absolute(resolved, ec);
        if (ec) {
            return ToolResult{
                "[Error] Failed to resolve image path: " + image_path +
                    " (" + ec.message() + ")",
                false};
        }
    }
    resolved = resolved.lexically_normal();

    if (!fs::exists(resolved, ec) || ec) {
        return ToolResult{
            ToolErrors::file_not_found(path_to_utf8(resolved), ctx.cwd),
            false};
    }
    if (!fs::is_regular_file(resolved, ec) || ec) {
        return ToolResult{
            ToolErrors::path_not_regular_file(path_to_utf8(resolved)),
            false};
    }

    const std::string mime_type = image_mime_for_path(resolved);
    if (mime_type.empty()) {
        return ToolResult{
            ToolErrors::invalid_parameter(
                "image_path",
                "supported formats are .png, .jpg, .jpeg, .gif, .webp, and .bmp"),
            false};
    }

    const std::string absolute_path = path_to_utf8(resolved);
    const std::string name = path_to_utf8(resolved.filename());
    const std::string requested_title =
        parser.get_or<std::string>("title", std::string{});

    std::string crop_error;
    const auto crop = crop_region_arg(parser, &crop_error);
    if (!crop_error.empty()) {
        return ToolResult{ToolErrors::invalid_parameter("crop", crop_error), false};
    }

    ToolSummary summary;
    summary.verb = "Showed";
    summary.object = requested_title.empty() ? name : requested_title;
    summary.icon = tool_icon("show_image");

    if (crop.has_value()) {
        std::ifstream input(resolved, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(input)),
                                std::istreambuf_iterator<char>());
        if (!input.good() && !input.eof()) {
            return ToolResult{"[Error] Failed to read image: " + absolute_path, false};
        }
        auto cropped = image::crop_image_bytes(bytes, *crop);
        if (!cropped.ok) {
            return ToolResult{ToolErrors::invalid_parameter("crop", cropped.error), false};
        }
        const auto& r = cropped.region;
        const std::string size_text = std::to_string(r.width) + "x" + std::to_string(r.height);
        const std::string crop_name = path_to_utf8(resolved.stem()) + "-crop" +
                                      (cropped.mime_type == "image/jpeg" ? ".jpg" : ".png");
        summary.metrics.emplace_back("crop", size_text);
        ToolResult result{"Image: " + absolute_path + " (cropped to " + size_text + " at " +
                              std::to_string(r.x) + "," + std::to_string(r.y) + " of " +
                              std::to_string(cropped.input.width) + "x" +
                              std::to_string(cropped.input.height) + ")",
                          true};
        result.summary = std::move(summary);
        result.attachments = nlohmann::json::array({
            nlohmann::json{
                {"name", crop_name},
                {"mime_type", cropped.mime_type},
                {"data_url", "data:" + cropped.mime_type + ";base64," +
                                 base64_encode(cropped.bytes)},
            },
        });
        return result;
    }

    const auto file_size = fs::file_size(resolved, ec);
    if (!ec) {
        summary.metrics.emplace_back(
            "size", format_bytes_compact(static_cast<std::size_t>(file_size)));
    }

    ToolResult result{"Image: " + absolute_path, true};
    result.summary = std::move(summary);
    result.attachments = nlohmann::json::array({
        nlohmann::json{
            {"name", name},
            {"mime_type", mime_type},
            {"path", absolute_path},
        },
    });
    return result;
}

} // namespace

ToolImpl create_show_image_tool() {
    ToolDef definition;
    definition.name = "show_image";
    definition.description =
        "Surface a local image file to the user as a durable chat attachment. "
        "Use this after producing, downloading, or capturing an image. "
        "To show only part of an image (for example the area you are describing), "
        "pass crop with the region in the image's own pixels. "
        "Supported formats: PNG, JPEG, GIF, WebP, and BMP.";
    definition.parameters = nlohmann::json{
        {"type", "object"},
        {"properties", {
            {"image_path", {
                {"type", "string"},
                {"description",
                 "Absolute path or path relative to the active working directory."},
            }},
            {"title", {
                {"type", "string"},
                {"description",
                 "Optional display title; defaults to the image filename."},
            }},
            {"crop", {
                {"type", "object"},
                {"description",
                 "Optional region to cut out, in source pixels from the top-left corner. "
                 "Parts outside the image are trimmed."},
                {"properties", {
                    {"x", {{"type", "integer"}}},
                    {"y", {{"type", "integer"}}},
                    {"width", {{"type", "integer"}}},
                    {"height", {{"type", "integer"}}},
                }},
                {"required", nlohmann::json::array({"x", "y", "width", "height"})},
            }},
        }},
        {"required", nlohmann::json::array({"image_path"})},
    };

    return ToolImpl{
        std::move(definition),
        execute_show_image,
        /*is_read_only=*/true,
    };
}

} // namespace acecode
