#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace acecode::desktop {

enum class ContextItemKind {
    File,
    Folder,
};

struct ContextItem {
    ContextItemKind kind = ContextItemKind::File;
    std::string path;
    std::string name;
    std::string mime_type;
    std::uintmax_t size_bytes = 0;
    bool reference_only = false;
    std::string bytes;
};

struct ContextItemsResult {
    std::vector<ContextItem> items;
    std::string error;

    explicit operator bool() const noexcept {
        return error.empty();
    }
};

// Canonicalize and classify native filesystem paths. Ordinary files are
// represented by source-path metadata only, including raster images.
// Folders are represented only by their
// absolute path and are never traversed.
ContextItemsResult materialize_context_items(
    const std::vector<std::string>& paths);

struct ContextDataFile {
    std::string name;
    std::string data_base64;
};

// Data without a source path (for example a screenshot) must exist on disk
// before it can be referenced. Keep it across restarts for saved drafts.
ContextItemsResult store_context_data_files(
    const std::string& directory,
    const std::vector<ContextDataFile>& files);

} // namespace acecode::desktop
