#pragma once

#include "session_client.hpp"
#include <map>
#include <string>
#include <unordered_set>

namespace acecode {

// Compact presentation state, updated under the owning dispatcher's mutex.
// It retains no transcript, arguments, tool output or service references.
class SessionActivityState {
public:
    void apply(const SessionEvent& event);
    nlohmann::json snapshot() const;
private:
    std::uint64_t seq_ = 0;
    bool busy_ = false;
    bool known_ = false;
    std::string turn_id_;
    std::string outcome_;
    std::string phase_;
    std::string label_;
    std::string tool_;
    std::string compact_id_;
    std::map<std::string, std::string> tools_;
    std::unordered_set<std::string> permissions_;
    std::unordered_set<std::string> questions_;
    nlohmann::json transfers_ = nlohmann::json::array();
};

} // namespace acecode
