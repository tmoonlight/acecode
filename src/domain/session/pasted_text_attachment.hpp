#pragma once

// Pasted text stored as an attachment file.
//
// The Web composer turns a large paste (UTF-8 >= 128 KiB, or one that would
// push the inline pasted blocks past 256 KiB) into a text/plain attachment and
// sends only a file reference to the model. Such attachments are marked with
// metadata.origin = "pasted_text" plus metadata.pasted_text = {chars, lines
// [, part, parts]}, so the server can tell the user's own material apart from
// an ordinary "attached file" (command expansion gate, reference wording).
//
// A paste made on the home page has no session yet. It is stored in the
// workspace draft attachment area under the owner kWorkspaceDraftAttachmentOwner
// and copied into the new session right before sending. Those files are only
// referenced by the home draft, so they are pruned when the draft stops
// referencing them (see prune_workspace_draft_attachments).

#include "attachment_store.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

namespace acecode {

inline constexpr const char* kPastedTextOrigin = "pasted_text";

// Owner (the attachments/<owner>/ directory name) of home draft attachments.
// The leading dot makes it impossible to collide with a session id: session
// ids, including headless --session-id values, only use [A-Za-z0-9-_].
inline constexpr const char* kWorkspaceDraftAttachmentOwner = ".workspace-draft";

// An unreferenced home draft attachment is removed only once it is older than
// this. The upload finishes before the draft that references it is saved (the
// home draft is saved with a 250 ms debounce, and a save already in flight may
// still carry the previous draft), so a fresh upload must survive a save that
// does not mention it yet.
inline constexpr std::chrono::seconds kWorkspaceDraftAttachmentMinAge{10 * 60};

// Reads `origin` / `paste` from an attachment upload body and writes them into
// `metadata`. `origin` absent: nothing is written (an ordinary upload).
// `origin` == "pasted_text": metadata.origin is set, the effective mime type
// (attachment_mime_for_name(name, mime_type)) must be text/plain, and the
// optional `paste` object {chars, lines, part, parts} becomes
// metadata.pasted_text (unknown keys dropped; part/parts follow the composer
// rule: both or neither, 1 <= part <= parts <= 1024). Any other value, or
// `paste` without the pasted_text origin, is an error.
bool apply_pasted_text_upload_metadata(const nlohmann::json& body,
                                       nlohmann::json& metadata,
                                       std::string& error);

// True when the attachment record was uploaded as pasted text.
bool is_pasted_text_attachment(const AttachmentRecord& record);

// Removes attachments under <attachment_project_dir>/attachments/.workspace-draft/
// that `draft` (the saved home draft JSON: {text, composer_content}) does not
// reference and whose metadata file was last written before now - min_age.
// A reference is a composer_content attachment part with
// store == "workspace_draft". Files are matched by name (<id>.json and the blob
// <id>.<ext> next to it), never through the paths recorded inside the JSON.
// Never throws; entries that cannot be inspected or removed are skipped.
// Returns the number of attachments removed.
std::size_t prune_workspace_draft_attachments(
    const std::filesystem::path& attachment_project_dir,
    const nlohmann::json& draft,
    std::chrono::seconds min_age,
    std::filesystem::file_time_type now = std::filesystem::file_time_type::clock::now());

} // namespace acecode
