#include "context_items.hpp"

#include "../utils/utf8_path.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace acecode::desktop {

namespace {

namespace fs = std::filesystem;

std::string mime_type_for_path(const fs::path& path) {
    std::string extension = path_to_utf8(path.extension());
    std::transform(extension.begin(), extension.end(), extension.begin(), [](char ch) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    });
    if (extension == ".png") return "image/png";
    if (extension == ".jpg" || extension == ".jpeg") return "image/jpeg";
    if (extension == ".gif") return "image/gif";
    if (extension == ".webp") return "image/webp";
    if (extension == ".bmp") return "image/bmp";
    if (extension == ".svg") return "image/svg+xml";
    if (extension == ".pdf") return "application/pdf";
    if (extension == ".json") return "application/json";
    if (extension == ".txt" || extension == ".md" || extension == ".log") {
        return "text/plain";
    }
    return {};
}

std::string display_name(const fs::path& path) {
    const fs::path filename = path.filename();
    return path_to_utf8(filename.empty() ? path : filename);
}

} // namespace

ContextItemsResult materialize_context_items(
    const std::vector<std::string>& paths) {
    ContextItemsResult result;
    result.items.reserve(paths.size());

    for (const auto& raw_path : paths) {
        if (raw_path.empty()) {
            result.error = "filesystem path is empty";
            return result;
        }

        const fs::path input = path_from_utf8(raw_path);
        if (!input.is_absolute()) {
            result.error = "filesystem path must be absolute: " + raw_path;
            return result;
        }

        std::error_code ec;
        const fs::path canonical = fs::weakly_canonical(input, ec);
        if (ec || canonical.empty()) {
            result.error = "filesystem path is unavailable: " + raw_path;
            return result;
        }

        ContextItem item;
        item.path = path_to_utf8_generic(canonical);
        item.name = display_name(canonical);
        if (fs::is_directory(canonical, ec) && !ec) {
            item.kind = ContextItemKind::Folder;
            result.items.push_back(std::move(item));
            continue;
        }

        ec.clear();
        if (!fs::is_regular_file(canonical, ec) || ec) {
            result.error = "filesystem item is not a regular file or folder: " + raw_path;
            return result;
        }

        item.kind = ContextItemKind::File;
        item.mime_type = mime_type_for_path(canonical);
        item.size_bytes = fs::file_size(canonical, ec);
        if (ec) {
            result.error = "failed to inspect file: " + item.name;
            return result;
        }

        // Desktop files already have a canonical, server-reachable path. Keep
        // every local file path-native (including raster images) so adding it
        // never depends on reading, Base64 encoding, or attachment limits.
        item.reference_only = true;
        result.items.push_back(std::move(item));
    }

    return result;
}

} // namespace acecode::desktop
