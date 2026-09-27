#include "context_items.hpp"

#include "utils/utf8_path.hpp"
#include "utils/base64.hpp"
#include "utils/clipboard.hpp"
#include "utils/uuid.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

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

ContextItemsResult store_context_data_files(
    const std::string& directory,
    const std::vector<ContextDataFile>& files) {
    ContextItemsResult result;
    if (files.size() > kMaxClipboardFilesystemPaths) {
        result.error = "too many filesystem items";
        return result;
    }
    const fs::path root = path_from_utf8(directory);
    if (!root.is_absolute()) {
        result.error = "context data directory must be absolute";
        return result;
    }
    std::vector<fs::path> created;
    std::vector<std::string> paths;
    try {
        fs::create_directories(root);
        for (const auto& file : files) {
            if (file.data_base64.size() > ((kMaxClipboardImageBytes + 2) / 3) * 4) {
                throw std::runtime_error("file data exceeds 25 MiB");
            }
            const auto bytes = base64_decode(file.data_base64);
            if (!bytes || bytes->size() > kMaxClipboardImageBytes) {
                throw std::runtime_error("invalid or oversized file data");
            }
            std::string name = file.name.substr(file.name.find_last_of("/\\") + 1);
            for (char& ch : name) {
                if (static_cast<unsigned char>(ch) < 32 || std::string("<>:\"|?*").find(ch) != std::string::npos) ch = '_';
            }
            while (!name.empty() && (name.back() == '.' || name.back() == ' ')) name.pop_back();
            if (name.empty()) name = "clipboard-file";
            // Avoid Windows device filenames while keeping ordinary names intact.
            std::string stem = name.substr(0, name.find('.'));
            std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
            if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL"
                || (stem.size() == 4 && (stem.substr(0, 3) == "COM" || stem.substr(0, 3) == "LPT") && stem[3] >= '1' && stem[3] <= '9')) name = '_' + name;
            const auto folder = root / generate_uuid();
            if (!fs::create_directory(folder)) throw std::runtime_error("cannot create context data directory");
            created.push_back(folder);
            const auto path = folder / path_from_utf8(name);
            std::ofstream output(path, std::ios::binary);
            output.write(bytes->data(), static_cast<std::streamsize>(bytes->size()));
            output.close();
            if (!output) throw std::runtime_error("cannot save context file");
            paths.push_back(path_to_utf8(path));
        }
        result = materialize_context_items(paths);
    } catch (const std::exception& error) {
        result.error = error.what();
    }
    if (!result) {
        result.items.clear();
        for (const auto& folder : created) {
            std::error_code ec;
            fs::remove_all(folder, ec);
        }
    }
    return result;
}

} // namespace acecode::desktop
