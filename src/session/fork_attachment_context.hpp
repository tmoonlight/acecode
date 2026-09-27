#pragma once

#include "attachment_store.hpp"
#include "composer_content.hpp"
#include "llm/llm_provider.hpp"
#include <unordered_map>

namespace acecode {

inline std::optional<AttachmentRecord> copy_attachment_to_session(
    const std::string& project_dir, const std::string& target_session,
    const AttachmentRecord& source, std::string& error) {
    if (source.metadata.is_object() &&
        source.metadata.value("storage", std::string{}) == "source_reference") {
        return save_attachment_reference(project_dir, target_session, source.name,
            source.mime_type, source.metadata.value("source_path", std::string{}), &error);
    }
    auto bytes = read_attachment_bytes(source, kMaxAttachmentBytes, &error);
    if (!bytes) return std::nullopt;
    return save_attachment(project_dir, target_session, source.name,
        source.mime_type, *bytes, &error, source.metadata);
}

// Only persisted resource descriptors supply source-session identities. Each
// referenced upload is materialized once for the fork and every representation
// (display parts, content_parts and metadata) receives the same target record.
inline bool copy_fork_composer_attachments(
    std::vector<ChatMessage>& messages, const std::string& project_dir,
    const std::string& source_session, const std::string& target_session,
    std::string& error) {
    using nlohmann::json;
    std::unordered_map<std::string, json> copied;
    for (auto& message : messages) {
        if (!message.metadata.is_object() || !message.metadata.contains("composer_content")) continue;
        auto normalized = normalize_composer_content(message.metadata["composer_content"]);
        if (!normalized.ok) {
            error = normalized.error;
            return false;
        }
        std::unordered_map<std::string, json> descriptors;
        if (message.content_parts.is_array()) {
            for (const auto& part : message.content_parts) {
                if (part.is_object() && part.contains("attachment") && part["attachment"].is_object()) {
                    const auto& record = part["attachment"];
                    descriptors.emplace(record.value("id", std::string{}), record);
                }
            }
        }
        if (message.metadata.contains("attachments") && message.metadata["attachments"].is_array()) {
            for (const auto& record : message.metadata["attachments"]) {
                if (record.is_object()) descriptors.emplace(record.value("id", std::string{}), record);
            }
        }
        std::unordered_map<std::string, json> replacements;
        for (auto& part : normalized.content["parts"]) {
            if (part.value("type", std::string{}) != "attachment") continue;
            const auto original_id = part.value("id", std::string{});
            const auto descriptor = descriptors.find(original_id);
            const auto origin = descriptor == descriptors.end()
                ? source_session : descriptor->second.value("session_id", source_session);
            const auto cache_key = origin + "\n" + original_id;
            auto found = copied.find(cache_key);
            if (found == copied.end()) {
                const auto source = load_attachment(project_dir, origin, original_id, &error);
                if (!source) return false;
                const auto copy = copy_attachment_to_session(project_dir, target_session, *source, error);
                if (!copy) return false;
                found = copied.emplace(cache_key, attachment_to_json(*copy)).first;
            }
            replacements.emplace(original_id, found->second);
            part["id"] = found->second["id"];
        }
        json verified = json::array();
        for (const auto& replacement : replacements) verified.push_back(replacement.second);
        if (!resolve_composer_content_attachments(normalized.content, verified, error)) return false;
        message.metadata["composer_content"] = std::move(normalized.content);
        if (message.content_parts.is_array()) {
            for (auto& part : message.content_parts) {
                if (!part.is_object() || !part.contains("attachment") || !part["attachment"].is_object()) continue;
                const auto found = replacements.find(part["attachment"].value("id", std::string{}));
                if (found != replacements.end()) part["attachment"] = found->second;
            }
        }
        if (!replacements.empty()) {
            // Existing unrelated attachment descriptors remain compatible.
            json records = message.metadata.value("attachments", json::array());
            if (!records.is_array()) records = json::array();
            for (auto& record : records) {
                if (!record.is_object()) continue;
                const auto found = replacements.find(record.value("id", std::string{}));
                if (found != replacements.end()) record = found->second;
            }
            for (const auto& replacement : replacements) {
                bool present = false;
                for (const auto& record : records) {
                    if (record.is_object() && record.value("id", std::string{}) ==
                        replacement.second.value("id", std::string{})) present = true;
                }
                if (!present) records.push_back(replacement.second);
            }
            message.metadata["attachments"] = std::move(records);
        }
    }
    return true;
}

} // namespace acecode
