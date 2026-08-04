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
    std::string bytes;
};

struct ContextItemsResult {
    std::vector<ContextItem> items;
    std::string error;

    explicit operator bool() const noexcept {
        return error.empty();
    }
};

// Canonicalize and classify native filesystem paths. Regular files are read so
// callers can pass the same payload shape used by the Desktop context picker;
// folders are represented only by their absolute path and are never traversed.
ContextItemsResult materialize_context_items(
    const std::vector<std::string>& paths);

} // namespace acecode::desktop
