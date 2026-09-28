#include "pasted_text_attachment.hpp"

#include "composer_content.hpp"

#include <algorithm>
#include <cctype>
#include <system_error>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

namespace acecode {

namespace {

// "text/plain", optionally with parameters ("text/plain; charset=utf-8").
bool is_text_plain_mime(const std::string& mime_type) {
    std::string base = mime_type.substr(0, mime_type.find(';'));
    while (!base.empty() && std::isspace(static_cast<unsigned char>(base.back()))) base.pop_back();
    std::transform(base.begin(), base.end(), base.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return base == "text/plain";
}

bool present(const nlohmann::json& body, const char* key) {
    return body.contains(key) && !body[key].is_null();
}

} // namespace

bool apply_pasted_text_upload_metadata(const nlohmann::json& body,
                                       nlohmann::json& metadata,
                                       std::string& error) {
    using nlohmann::json;
    if (!body.is_object()) return true;
    if (!present(body, "origin")) {
        if (present(body, "paste")) {
            error = "paste requires origin \"pasted_text\"";
            return false;
        }
        return true;
    }
    if (!body["origin"].is_string() || body["origin"] != kPastedTextOrigin) {
        error = "unsupported attachment origin";
        return false;
    }
    const std::string name = body.value("name", std::string{});
    const std::string supplied_mime =
        body.contains("mime_type") && body["mime_type"].is_string()
            ? body["mime_type"].get<std::string>() : std::string{};
    if (!is_text_plain_mime(attachment_mime_for_name(name, supplied_mime))) {
        error = "pasted text attachments must be text/plain";
        return false;
    }

    json stats = json::object();
    if (present(body, "paste")) {
        // Same field rules as the composer `paste` descriptor, minus the title
        // (the title is display data and lives only in composer_content).
        const auto& paste = body["paste"];
        if (!paste.is_object()) {
            error = "paste must be an object";
            return false;
        }
        json descriptor = paste;
        descriptor.erase("title");
        json clean = json::object();
        std::size_t bytes = 0;
        if (!composer_content_detail::normalize_paste_descriptor(descriptor, clean, bytes)) {
            error = "invalid paste descriptor";
            return false;
        }
        stats = std::move(clean["paste"]);
    }

    if (!metadata.is_object()) metadata = json::object();
    metadata["origin"] = kPastedTextOrigin;
    metadata["pasted_text"] = std::move(stats);
    return true;
}

bool is_pasted_text_attachment(const AttachmentRecord& record) {
    return record.metadata.is_object() &&
           record.metadata.contains("origin") &&
           record.metadata["origin"].is_string() &&
           record.metadata["origin"] == kPastedTextOrigin;
}

std::size_t prune_workspace_draft_attachments(
    const fs::path& attachment_project_dir,
    const nlohmann::json& draft,
    std::chrono::seconds min_age,
    fs::file_time_type now) {
    std::unordered_set<std::string> referenced;
    try {
        if (draft.is_object() && draft.contains("composer_content") &&
            draft["composer_content"].is_object() &&
            draft["composer_content"].contains("parts") &&
            draft["composer_content"]["parts"].is_array()) {
            for (const auto& part : draft["composer_content"]["parts"]) {
                if (!part.is_object() || part.value("type", std::string{}) != "attachment") continue;
                if (part.value("store", std::string{}) != kWorkspaceDraftStore) continue;
                const auto id = part.value("id", std::string{});
                if (!id.empty()) referenced.insert(id);
            }
        }
    } catch (...) {
        // A malformed draft references nothing we can trust; keep everything.
        return 0;
    }

    const fs::path dir = attachment_project_dir / "attachments" / kWorkspaceDraftAttachmentOwner;
    std::error_code ec;
    if (!fs::is_directory(dir, ec) || ec) return 0;

    // Collect first, delete afterwards: removing entries while a
    // directory_iterator walks the same directory is unspecified.
    std::vector<std::string> expired;
    std::vector<fs::path> files;
    fs::directory_iterator it(dir, ec);
    if (ec) return 0;
    for (const fs::directory_iterator end; it != end; it.increment(ec)) {
        if (ec) break;
        const auto& entry = *it;
        std::error_code entry_ec;
        if (!entry.is_regular_file(entry_ec) || entry_ec) continue;
        files.push_back(entry.path());
        if (entry.path().extension() != ".json") continue;
        std::string id;
        try {
            id = entry.path().stem().string();
        } catch (...) {
            continue;  // not an ASCII attachment id
        }
        if (!is_valid_attachment_id(id) || referenced.count(id)) continue;
        const auto written = fs::last_write_time(entry.path(), entry_ec);
        if (entry_ec || written > now - min_age) continue;
        expired.push_back(id);
    }

    std::size_t removed = 0;
    for (const auto& id : expired) {
        // Blob(s) first, metadata last: if a blob cannot be removed, the
        // metadata stays and the next prune retries the whole attachment.
        bool blobs_removed = true;
        fs::path metadata_file;
        for (const auto& file : files) {
            std::string stem;
            try {
                stem = file.stem().string();
            } catch (...) {
                continue;
            }
            if (stem != id) continue;
            if (file.extension() == ".json") {
                metadata_file = file;
                continue;
            }
            std::error_code remove_ec;
            fs::remove(file, remove_ec);
            if (remove_ec) blobs_removed = false;
        }
        if (!blobs_removed || metadata_file.empty()) continue;
        std::error_code remove_ec;
        if (fs::remove(metadata_file, remove_ec) && !remove_ec) ++removed;
    }
    return removed;
}

} // namespace acecode
