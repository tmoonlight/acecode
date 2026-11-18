#pragma once

#include "session/annotation_submission.hpp"
#include "utils/sha256.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>

namespace acecode::web {

inline bool parse_annotation_submission_identity(
    const nlohmann::json& payload, bool ordinary_message,
    AnnotationSubmissionIdentity& identity, std::string& error) {
    if (!payload.contains("annotation_batch_id")) return true;
    if (!ordinary_message) {
        error = "annotation batches require ordinary message submission";
        return false;
    }
    const auto& batch_id = payload["annotation_batch_id"];
    if (!batch_id.is_string()) {
        error = "invalid annotation_batch_id";
        return false;
    }
    const auto candidate = batch_id.get<std::string>();
    if (candidate.empty() || candidate.size() > 128 ||
        !std::all_of(candidate.begin(), candidate.end(), [](unsigned char character) {
            return (character >= 'a' && character <= 'z') ||
                   (character >= 'A' && character <= 'Z') ||
                   (character >= '0' && character <= '9') ||
                   character == '-' || character == '_';
        })) {
        error = "invalid annotation_batch_id";
        return false;
    }
    for (auto field = payload.begin(); field != payload.end(); ++field) {
        if (field.key() != "annotation_batch_id" && field.key() != "text" &&
            field.key() != "attachments" && field.key() != "client_message_id") {
            error = "unsupported annotation batch field: " + field.key();
            return false;
        }
    }
    if (!payload.contains("text") || !payload["text"].is_string() ||
        payload["text"].get_ref<const std::string&>().empty() ||
        (payload.contains("attachments") && !payload["attachments"].is_array()) ||
        (payload.contains("client_message_id") &&
         (!payload["client_message_id"].is_string() ||
          payload["client_message_id"].get_ref<const std::string&>().empty() ||
          payload["client_message_id"].get_ref<const std::string&>().size() > 256))) {
        error = "invalid annotation batch payload";
        return false;
    }
    identity = {candidate, sha256_hex(payload.dump())};
    return true;
}

} // namespace acecode::web
