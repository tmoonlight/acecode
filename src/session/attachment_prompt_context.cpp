#include "attachment_prompt_context.hpp"

#include "utils/utf8_path.hpp"

#include <nlohmann/json.hpp>

namespace acecode {

std::optional<std::string> attachment_source_path(
    const AttachmentRecord& record) {
    if (!record.metadata.is_object()) return std::nullopt;
    const auto it = record.metadata.find("source_path");
    if (it == record.metadata.end() || !it->is_string()) {
        return std::nullopt;
    }
    const std::string path = it->get<std::string>();
    return path.empty() ? std::nullopt : std::optional<std::string>{path};
}

std::string file_attachment_reference_text(
    const AttachmentRecord& record) {
    const std::string snapshot_path = record.path.empty()
        ? std::string{}
        : path_to_utf8_generic(path_from_utf8(record.path));
    nlohmann::json reference = {
        {"attachment_id", record.id},
        {"name", record.name},
        {"mime_type", record.mime_type},
        {"size_bytes", record.size_bytes},
    };

    const auto source_path = attachment_source_path(record);
    if (source_path.has_value()) {
        reference["source_path"] = *source_path;
    }
    if (!snapshot_path.empty()) {
        reference["snapshot_path"] = snapshot_path;
    }

    const std::string read_path = source_path.value_or(snapshot_path);
    if (!read_path.empty()) {
        reference["read_path"] = read_path;
    }

    std::string text = "[Attached file reference]\n" + reference.dump(2);
    text += "\nThe file content is not included in this message.";
    if (read_path.empty()) {
        text += " No readable path is available.";
        return text;
    }

    text +=
        " Read `read_path` with `file_read` or another suitable read-only "
        "inspection tool only when the task needs the contents.";
    if (source_path.has_value() && !snapshot_path.empty()) {
        text +=
            " If `source_path` is unavailable, read `snapshot_path` instead."
            " Modify `source_path` only when the user asks to change the original"
            " file; never modify `snapshot_path`.";
    } else if (source_path.has_value()) {
        text +=
            " This attachment is a source reference; no session snapshot was"
            " created. Modify `source_path` only when the user asks to change"
            " the original file.";
    } else if (!snapshot_path.empty()) {
        text += " `snapshot_path` is the session copy; never modify it.";
    }
    return text;
}

} // namespace acecode
