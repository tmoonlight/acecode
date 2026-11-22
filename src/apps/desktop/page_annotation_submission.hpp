#pragma once

#include <algorithm>
#include <string>
#include <nlohmann/json.hpp>

namespace acecode::desktop {

inline bool annotation_submission_records(const nlohmann::json& command,
                                          const nlohmann::json& records,
                                          nlohmann::json& selected,
                                          std::string& error) {
    selected = nlohmann::json::array();
    if (!command.is_object() || !command.contains("record_ids") ||
        !command["record_ids"].is_array() || command["record_ids"].empty() ||
        command["record_ids"].size() > 1024) {
        error = "Invalid annotation membership";
        return false;
    }
    for (const auto& id : command["record_ids"]) {
        if (!id.is_string()) { error = "Invalid annotation identity"; return false; }
        const auto found = std::find_if(records.begin(), records.end(),
            [&id](const auto& record) { return record["id"] == id; });
        if (found == records.end() || std::any_of(selected.begin(), selected.end(),
            [&id](const auto& record) { return record["id"] == id; })) {
            error = "Unknown or duplicate annotation";
            return false;
        }
        selected.push_back(*found);
    }
    return true;
}

} // namespace acecode::desktop
