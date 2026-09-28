#pragma once

#include <nlohmann/json.hpp>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace acecode {

// Inline pasted_text blocks are joined to neighbouring non-empty pieces of the
// submission text with this separator. The Web composer implements the same
// rule (composerContentSubmissionText); both are pinned by shared examples.
inline constexpr const char* kPastedTextSeparator = "\n\n";

// The only accepted `store` value on an attachment part. It exists only in a
// home (workspace) draft: the attachment still lives in the workspace draft
// attachment area and is imported into a session before sending, so parts in a
// sent message never carry it (resolve_composer_content_attachments drops it).
inline constexpr const char* kWorkspaceDraftStore = "workspace_draft";

// One shared budget for every string in a composer_content document (drafts
// and messages alike, so nothing is "sendable but not savable"). The Web
// composer keeps inline pasted blocks at or below 256 KiB in total and turns
// larger pastes into attachment files; this is only the server-side backstop.
inline constexpr std::size_t kComposerContentMaxBytes = 2 * 1024 * 1024;

struct ComposerContentResult {
    bool ok = false;
    nlohmann::json content;
    // Editor text: text parts plus path/skill tokens, in order. It never
    // contains pasted blocks, so draft restore, fork restored_prompt and input
    // history see exactly what the rich editor shows.
    std::string text;
    // Message body: the editor pieces plus inline pasted_text blocks in part
    // order, each block separated from neighbouring non-empty pieces by
    // kPastedTextSeparator. File-backed pastes are attachment parts (with a
    // `paste` descriptor); their text is not in composer_content at all and
    // reaches the model as a file reference.
    std::string submission_text;
    // The first non-empty submission piece is a pasted_text block (nothing was
    // typed before it). Pasted material must never be expanded as a slash
    // command, whatever text it happens to start with.
    bool leads_with_pasted_text = false;
    std::string error;
};

namespace composer_content_detail {

inline bool is_bounded_non_negative_integer(const nlohmann::json& value) {
    constexpr std::uint64_t kMax = std::uint64_t{1} << 53;  // exact in JS numbers
    if (value.is_number_unsigned()) return value.get<std::uint64_t>() <= kMax;
    if (value.is_number_integer()) {
        const auto signed_value = value.get<std::int64_t>();
        return signed_value >= 0 && static_cast<std::uint64_t>(signed_value) <= kMax;
    }
    return false;
}

inline bool is_store_scope(const std::string& scope) {
    if (scope.empty() || scope.size() > 128) return false;
    for (const char c : scope) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_';
        if (!ok) return false;
    }
    return true;
}

// `paste` on an attachment part describes a file-backed pasted block:
// {title, chars, lines[, part, parts]}. Unknown keys are dropped; `title`
// counts toward the shared byte budget.
inline bool normalize_paste_descriptor(const nlohmann::json& paste,
                                       nlohmann::json& clean,
                                       std::size_t& bytes) {
    using nlohmann::json;
    if (!paste.is_object()) return false;
    json out = json::object();
    if (paste.contains("title")) {
        if (!paste["title"].is_string()) return false;
        const auto& title = paste["title"].get_ref<const std::string&>();
        if (title.size() > 1024 || title.find('\0') != std::string::npos) return false;
        bytes += title.size();
        if (bytes > kComposerContentMaxBytes) return false;
        out["title"] = title;
    }
    for (const char* key : {"chars", "lines"}) {
        if (!paste.contains(key)) continue;
        if (!is_bounded_non_negative_integer(paste[key])) return false;
        out[key] = paste[key];
    }
    const bool has_part = paste.contains("part");
    const bool has_parts = paste.contains("parts");
    if (has_part || has_parts) {
        // A split paste always names both its ordinal and the total.
        if (!has_part || !has_parts ||
            !is_bounded_non_negative_integer(paste["part"]) ||
            !is_bounded_non_negative_integer(paste["parts"])) return false;
        const auto part = paste["part"].get<std::uint64_t>();
        const auto parts = paste["parts"].get<std::uint64_t>();
        if (part < 1 || part > parts || parts > 1024) return false;
        out["part"] = part;
        out["parts"] = parts;
    }
    clean["paste"] = std::move(out);
    return true;
}

} // namespace composer_content_detail

// This is a display/restore contract, independent of provider content parts.
// Keep resource resolution at the API boundary: client paths are not authority
// for an attachment, and draft placeholders are never accepted as sent files.
//
// Part types: text / path / skill (editor pieces), attachment (uploaded
// resource; `paste` marks a file-backed pasted block, `store` a home-draft
// attachment that still has to be imported), pasted_text (an inline pasted
// block {key, text}; its text is part of submission_text but never of text).
inline ComposerContentResult normalize_composer_content(const nlohmann::json& value) {
    using nlohmann::json;
    ComposerContentResult result;
    result.error = "invalid composer_content";
    if (!value.is_object() || !value.contains("version") ||
        !value["version"].is_number_integer() || value["version"] != 1 ||
        !value.contains("parts") || !value["parts"].is_array()) return result;
    if (value["parts"].size() > 4096) {
        result.error = "composer_content exceeds 4096 parts";
        return result;
    }
    std::size_t bytes = 0;
    const auto over_budget = [&] {
        if (bytes <= kComposerContentMaxBytes) return false;
        result.error = "composer_content exceeds 2 MiB";
        return true;
    };
    // Submission assembly: empty pieces are skipped; a separator goes between
    // two non-empty pieces whenever either of them is a pasted block.
    bool seen_piece = false;
    bool last_piece_was_paste = false;
    const auto append_submission = [&](const std::string& piece, bool is_paste) {
        if (piece.empty()) return;
        if (!seen_piece) {
            seen_piece = true;
            result.leads_with_pasted_text = is_paste;
        } else if (is_paste || last_piece_was_paste) {
            result.submission_text += kPastedTextSeparator;
        }
        result.submission_text += piece;
        last_piece_was_paste = is_paste;
    };
    json parts = json::array();
    for (const auto& part : value["parts"]) {
        if (!part.is_object() || !part.contains("type") || !part["type"].is_string()) return result;
        const auto type = part["type"].get<std::string>();
        json clean{{"type", type}};
        auto field = [&](const char* key, bool required, std::size_t max_bytes,
                         bool empty_allowed = false) {
            if (!part.contains(key)) return !required;
            if (!part[key].is_string()) return false;
            const auto& text = part[key].get_ref<const std::string&>();
            if ((!empty_allowed && text.empty()) || text.size() > max_bytes ||
                text.find('\0') != std::string::npos) return false;
            bytes += text.size();
            if (over_budget()) return false;
            clean[key] = text;
            return true;
        };
        if (type == "text") {
            if (!field("text", true, kComposerContentMaxBytes, true)) return result;
            const auto& text = clean["text"].get_ref<const std::string&>();
            result.text += text;
            append_submission(text, false);
        } else if (type == "path") {
            if (!field("path", true, 65536) || !field("token", true, 65536)) return result;
            if (part.contains("directory")) {
                if (!part["directory"].is_boolean()) return result;
                clean["directory"] = part["directory"];
            }
            const auto& token = clean["token"].get_ref<const std::string&>();
            result.text += token;
            append_submission(token, false);
        } else if (type == "skill") {
            if (!field("name", true, 16384) || !field("token", true, 65536) ||
                !field("path", false, 65536, true)) return result;
            const auto& token = clean["token"].get_ref<const std::string&>();
            result.text += token;
            append_submission(token, false);
        } else if (type == "pasted_text") {
            if (!field("key", false, 256, true)) return result;
            if (!part.contains("text") || !part["text"].is_string()) return result;
            const auto& pasted = part["text"].get_ref<const std::string&>();
            if (pasted.find('\0') != std::string::npos) return result;
            if (pasted.empty()) continue;  // an empty block carries nothing
            bytes += pasted.size();
            if (over_budget()) return result;
            clean["text"] = pasted;
            append_submission(pasted, true);
        } else if (type == "attachment") {
            if (!field("key", true, 256) || !field("id", false, 256, true) ||
                !field("name", true, 16384) || !field("kind", true, 64) ||
                !field("mime_type", false, 1024, true) ||
                !field("path", false, 65536, true)) return result;
            if (part.contains("paste") && !part["paste"].is_null()) {
                if (!composer_content_detail::normalize_paste_descriptor(
                        part["paste"], clean, bytes)) {
                    over_budget();
                    return result;
                }
            }
            if (part.contains("store") && !part["store"].is_null()) {
                if (!part["store"].is_string() || part["store"] != kWorkspaceDraftStore ||
                    !part.contains("store_scope") || !part["store_scope"].is_string() ||
                    !composer_content_detail::is_store_scope(
                        part["store_scope"].get_ref<const std::string&>())) return result;
                clean["store"] = kWorkspaceDraftStore;
                clean["store_scope"] = part["store_scope"];
            }
        } else {
            result.error = "unsupported composer_content part type";
            return result;
        }
        parts.push_back(std::move(clean));
    }
    result.content = json{{"version", 1}, {"parts", std::move(parts)}};
    result.ok = true;
    result.error.clear();
    return result;
}

// Records come from load_attachment for this session, never from raw request
// metadata. Hydrate display fields from those records and keep occurrence keys.
// A resolved part is a session attachment by construction, so any home-draft
// `store` marker is dropped; the `paste` descriptor is kept for display.
inline bool resolve_composer_content_attachments(
    nlohmann::json& content, const nlohmann::json& verified_records,
    std::string& error) {
    using nlohmann::json;
    std::unordered_map<std::string, const json*> records;
    for (const auto& record : verified_records) {
        records.emplace(record.value("id", std::string{}), &record);
    }
    for (auto& part : content["parts"]) {
        if (part.value("type", std::string{}) != "attachment") continue;
        const auto id = part.value("id", std::string{});
        const auto found = records.find(id);
        if (id.empty() || found == records.end()) {
            error = "composer_content attachment must reference a submitted attachment id";
            return false;
        }
        const auto& record = *found->second;
        part["name"] = record.value("name", std::string{});
        part["kind"] = record.value("kind", std::string{"file"});
        part["mime_type"] = record.value("mime_type", std::string{});
        part.erase("store");
        part.erase("store_scope");
        part.erase("path");
        if (record.contains("path") && record["path"].is_string()) part["path"] = record["path"];
        if (record.contains("metadata") && record["metadata"].is_object() &&
            record["metadata"].contains("source_path") &&
            record["metadata"]["source_path"].is_string()) {
            part["path"] = record["metadata"]["source_path"];
        }
    }
    return true;
}

} // namespace acecode
