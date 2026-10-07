#pragma once

#include "llm/llm_provider.hpp"

#include <cstddef>
#include <limits>

namespace acecode {

// Persisted but hidden from transcript clients. These are the only meta records
// deliberately retained in the effective model history.
inline constexpr const char* kRequestContextSnapshot = "request_context_snapshot";
inline constexpr const char* kRequestContextUpdate = "request_context_update";

// Display classification must not depend on whether this version understands
// the payload. Future or damaged internal records still stay hidden.
inline bool is_request_context_metadata(const ChatMessage& message) {
    return message.is_meta &&
        (message.subtype == kRequestContextSnapshot || message.subtype == kRequestContextUpdate);
}

inline bool is_request_context_record(const ChatMessage& message) {
    if (!is_request_context_metadata(message) || message.role != "user" ||
        !message.metadata.is_object()) return false;
    const auto& metadata = message.metadata;
    const auto version = metadata.find("request_context_version");
    if (version == metadata.end() || !version->is_number_integer() || *version != 1) return false;

    // A damaged, future or foreign record must stay ordinary hidden metadata;
    // it cannot reach the request builder's typed reads or state merging.
    const auto state = metadata.find("context_state");
    if (state != metadata.end()) {
        if (!state->is_object()) return false;
        for (const auto& value : *state) {
            if (!value.is_string()) return false;
        }
    }
    const auto skills = metadata.find("skills");
    if (skills != metadata.end() && !skills->is_string()) return false;
    const auto catalog = metadata.find("skills_catalog_key");
    if (catalog != metadata.end() && !catalog->is_string()) return false;
    const auto bytes = metadata.find("project_rules_bytes");
    if (bytes != metadata.end() &&
        (!bytes->is_number_integer() || *bytes < 0 ||
         *bytes > (std::numeric_limits<std::size_t>::max)())) return false;
    const auto memory = metadata.find("memory_active");
    if (memory != metadata.end() && !memory->is_boolean()) return false;
    const auto transcript = metadata.find("transcript_only");
    if (transcript != metadata.end() && (!transcript->is_boolean() || transcript->get<bool>())) return false;
    return true;
}

inline bool is_request_context_snapshot(const ChatMessage& message) {
    return is_request_context_record(message) && message.subtype == kRequestContextSnapshot;
}

} // namespace acecode
