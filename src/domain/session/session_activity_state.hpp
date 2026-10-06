#pragma once

#include "session_client.hpp"
#include <map>
#include <string>
#include <unordered_set>
#include <utility>

namespace acecode {

// Compact presentation state, updated under the owning dispatcher's mutex.
// It keeps only short display fragments: the streaming tail of the current
// model step (bounded) and each running tool's one-line call preview. No
// transcript, arguments, tool output or service references are retained.
class SessionActivityState {
public:
    void apply(const SessionEvent& event);
    nlohmann::json snapshot() const;
private:
    void enter_phase(const std::string& phase);
    void append_stream(const std::string& phase, const std::string& piece);

    std::uint64_t seq_ = 0;
    bool busy_ = false;
    bool known_ = false;
    std::string turn_id_;
    std::string outcome_;
    std::string phase_;
    std::string label_;
    std::string tool_;
    std::string detail_;
    std::string stream_tail_;
    std::string compact_id_;
    // tool_call_id -> (tool name, call preview)
    std::map<std::string, std::pair<std::string, std::string>> tools_;
    std::unordered_set<std::string> permissions_;
    std::unordered_set<std::string> questions_;
    nlohmann::json transfers_ = nlohmann::json::array();
};

} // namespace acecode
