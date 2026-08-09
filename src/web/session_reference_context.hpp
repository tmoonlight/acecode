#pragma once

#include "../provider/llm_provider.hpp"

#include <cstddef>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace acecode::web {

inline constexpr std::size_t kMaxSessionReferences = 5;
inline constexpr std::size_t kMaxSessionReferenceIdBytes = 256;
inline constexpr std::size_t kMaxSessionReferenceLabelBytes = 512;
inline constexpr std::size_t kMaxSessionReferenceTranscriptBytes = 24 * 1024;
inline constexpr std::size_t kMaxSessionReferencePromptBytes = 64 * 1024;

struct SessionReferenceDescriptor {
    std::string session_id;
    std::string workspace_hash;
    bool no_workspace = false;
    std::string title;
    std::string workspace_name;
};

struct ParsedSessionReferences {
    bool ok = true;
    std::string error;
    std::vector<SessionReferenceDescriptor> references;
};

struct ResolvedSessionReference {
    SessionReferenceDescriptor descriptor;
    std::vector<ChatMessage> messages;
};

struct SessionReferencePromptContext {
    nlohmann::json meta = nlohmann::json::array();
    std::string prompt;
};

ParsedSessionReferences parse_session_reference_descriptors(
    const nlohmann::json& value);

SessionReferencePromptContext build_session_reference_prompt_context(
    const std::vector<ResolvedSessionReference>& references);

std::string build_session_reference_augmented_prompt(
    const SessionReferencePromptContext& context,
    const std::string& user_text);

} // namespace acecode::web
