#include "turn_net_diff.hpp"

#include "tool_metadata_codec.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace acecode {

namespace {

bool decode_nonnegative_int(const nlohmann::json& value, int& output) {
    if (!value.is_number_integer()) return false;
    if (value.is_number_unsigned()) {
        const auto decoded = value.get<std::uint64_t>();
        if (decoded > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
            return false;
        }
        output = static_cast<int>(decoded);
        return true;
    }
    const auto decoded = value.get<std::int64_t>();
    if (decoded < 0 || decoded > std::numeric_limits<int>::max()) return false;
    output = static_cast<int>(decoded);
    return true;
}

bool validate_hunk_integer_ranges(const nlohmann::json& hunks) {
    if (!hunks.is_array()) return false;
    for (const auto& hunk : hunks) {
        if (!hunk.is_object()) return false;
        int ignored = 0;
        for (const char* key : {"old_start", "old_count", "new_start", "new_count"}) {
            if (!hunk.contains(key) || !decode_nonnegative_int(hunk[key], ignored)) {
                return false;
            }
        }
        if (!hunk.contains("lines") || !hunk["lines"].is_array()) return false;
        for (const auto& line : hunk["lines"]) {
            if (!line.is_object()) return false;
            for (const char* key : {"old_line_no", "new_line_no"}) {
                if (line.contains(key) && !decode_nonnegative_int(line[key], ignored)) {
                    return false;
                }
            }
        }
    }
    return true;
}

} // namespace

nlohmann::json encode_turn_net_diff(const TurnNetDiffRecord& record) {
    nlohmann::json files = nlohmann::json::array();
    for (const auto& file : record.files) {
        files.push_back({
            {"file", file.file},
            {"additions", std::max(0, file.additions)},
            {"deletions", std::max(0, file.deletions)},
            {"hunks", encode_tool_hunks(file.hunks)},
        });
    }

    nlohmann::json errors = nlohmann::json::array();
    for (const auto& error : record.errors) {
        errors.push_back(error);
    }

    return {
        {"user_message_uuid", record.user_message_uuid},
        {"complete", record.complete},
        {"files", std::move(files)},
        {"errors", std::move(errors)},
    };
}

std::optional<TurnNetDiffRecord> decode_turn_net_diff(const nlohmann::json& value) {
    try {
        if (!value.is_object() ||
            !value.contains("user_message_uuid") ||
            !value["user_message_uuid"].is_string() ||
            !value.contains("complete") ||
            !value["complete"].is_boolean() ||
            !value.contains("files") ||
            !value["files"].is_array() ||
            !value.contains("errors") ||
            !value["errors"].is_array()) {
            return std::nullopt;
        }

        TurnNetDiffRecord record;
        record.user_message_uuid = value["user_message_uuid"].get<std::string>();
        record.complete = value["complete"].get<bool>();
        if (record.user_message_uuid.empty()) return std::nullopt;

        for (const auto& item : value["files"]) {
            if (!item.is_object() ||
                !item.contains("file") || !item["file"].is_string() ||
                !item.contains("additions") || !item["additions"].is_number_integer() ||
                !item.contains("deletions") || !item["deletions"].is_number_integer() ||
                !item.contains("hunks")) {
                return std::nullopt;
            }

            TurnNetDiffFile file;
            file.file = item["file"].get<std::string>();
            if (file.file.empty() ||
                !decode_nonnegative_int(item["additions"], file.additions) ||
                !decode_nonnegative_int(item["deletions"], file.deletions) ||
                !validate_hunk_integer_ranges(item["hunks"])) {
                return std::nullopt;
            }
            auto hunks = decode_tool_hunks(item["hunks"]);
            if (!hunks.has_value()) return std::nullopt;
            file.hunks = std::move(*hunks);
            record.files.push_back(std::move(file));
        }

        for (const auto& error : value["errors"]) {
            if (!error.is_string()) return std::nullopt;
            record.errors.push_back(error.get<std::string>());
        }
        return record;
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
}

ChatMessage make_turn_net_diff_message(const TurnNetDiffRecord& record,
                                       const std::string& timestamp) {
    ChatMessage msg;
    msg.role = "system";
    msg.content = "[Turn net diff]";
    msg.timestamp = timestamp;
    msg.metadata = nlohmann::json::object();
    msg.metadata["transcript_only"] = true;
    msg.metadata["turn_net_diff"] = encode_turn_net_diff(record);
    return msg;
}

bool is_turn_net_diff_message(const ChatMessage& msg) {
    if (!msg.metadata.is_object()) return false;
    return decode_turn_net_diff(
               msg.metadata.value("turn_net_diff", nlohmann::json{}))
        .has_value();
}

std::string turn_net_diff_user_message_uuid(const ChatMessage& msg) {
    if (!msg.metadata.is_object()) return {};
    auto record = decode_turn_net_diff(
        msg.metadata.value("turn_net_diff", nlohmann::json{}));
    return record.has_value() ? record->user_message_uuid : std::string{};
}

} // namespace acecode
