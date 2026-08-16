#pragma once

#include "../provider/llm_provider.hpp"
#include "../tool/diff_utils.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace acecode {

struct TurnNetDiffFile {
    std::string file;
    int additions = 0;
    int deletions = 0;
    std::vector<DiffHunk> hunks;
};

struct TurnNetDiffRecord {
    std::string user_message_uuid;
    bool complete = true;
    std::vector<TurnNetDiffFile> files;
    std::vector<std::string> errors;
};

nlohmann::json encode_turn_net_diff(const TurnNetDiffRecord& record);
std::optional<TurnNetDiffRecord> decode_turn_net_diff(const nlohmann::json& value);

ChatMessage make_turn_net_diff_message(const TurnNetDiffRecord& record,
                                       const std::string& timestamp);
bool is_turn_net_diff_message(const ChatMessage& msg);
std::string turn_net_diff_user_message_uuid(const ChatMessage& msg);

} // namespace acecode
