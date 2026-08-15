#pragma once

#include "../provider/llm_provider.hpp"
#include "../session/session_trajectory.hpp"

#include <cstddef>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace acecode::web {

struct LegacyTrajectoryPage {
    std::vector<nlohmann::json> records;
    std::size_t next_after = 0;
    bool has_more = false;
    std::size_t total = 0;
    std::vector<std::string> missing_capabilities;
};

// Projects only facts present in the canonical transcript. recorded_records
// are used for stable-id/tool-call/turn de-duplication when an old session has
// since accumulated a precise trajectory suffix.
LegacyTrajectoryPage project_legacy_trajectory(
    const std::vector<ChatMessage>& messages,
    const std::vector<SessionTrajectoryRecord>& recorded_records,
    std::size_t after = 0,
    std::size_t limit = kSessionTrajectoryDefaultPageSize);

} // namespace acecode::web
