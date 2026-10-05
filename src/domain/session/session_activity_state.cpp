#include "session_activity_state.hpp"
#include "inter_agent_message.hpp"
#include "utils/encoding.hpp"

namespace acecode {
namespace {
std::string text(const nlohmann::json& value, const char* key) {
    const auto it = value.find(key);
    return it != value.end() && it->is_string()
        ? truncate_utf8_prefix(it->get_ref<const std::string&>(), 240) : std::string{};
}
bool flag(const nlohmann::json& value, const char* key) {
    const auto it = value.find(key);
    return it != value.end() && it->is_boolean() && it->get<bool>();
}
} // namespace

void SessionActivityState::apply(const SessionEvent& event) {
    seq_ = event.seq;
    const auto& p = event.payload;
    if (!p.is_object()) return;
    const auto finish = [&] {
        const auto outcome = text(p, "outcome");
        if (outcome.empty()) return;
        busy_ = false;
        known_ = true;
        outcome_ = outcome;
        phase_.clear(); label_.clear(); tool_.clear(); tools_.clear();
        permissions_.clear(); questions_.clear();
    };
    switch (event.kind) {
    case SessionEventKind::BusyChanged:
        busy_ = flag(p, "busy"); known_ = true;
        if (busy_) {
            outcome_.clear(); phase_ = "model_waiting";
            label_.clear(); tool_.clear(); tools_.clear();
            permissions_.clear(); questions_.clear();
            turn_id_ = text(p, "turn_id");
        } else finish();
        break;
    case SessionEventKind::Done: finish(); break;
    case SessionEventKind::AgentProgress:
        phase_ = text(p, "phase");
        label_ = text(p, "label");
        tool_ = text(p, "tool");
        break;
    case SessionEventKind::ToolStart: {
        tool_ = text(p, "tool");
        auto id = text(p, "tool_call_id");
        if (id.empty()) id = tool_;
        tools_[id] = tool_;
        break;
    }
    case SessionEventKind::ToolEnd: {
        auto id = text(p, "tool_call_id");
        if (id.empty()) id = text(p, "tool");
        tools_.erase(id);
        tool_ = tools_.empty() ? std::string{} : tools_.rbegin()->second;
        break;
    }
    case SessionEventKind::PermissionRequest:
        permissions_.insert(text(p, "request_id")); break;
    case SessionEventKind::PermissionClosed:
        permissions_.erase(text(p, "request_id")); break;
    case SessionEventKind::QuestionRequest:
        questions_.insert(text(p, "request_id")); break;
    case SessionEventKind::QuestionClosed:
        questions_.erase(text(p, "request_id")); break;
    case SessionEventKind::Message: {
        const auto metadata = p.find("metadata");
        if (metadata == p.end() || !metadata->is_object()) break;
        if (flag(*metadata, "compact_notice_complete")) {
            compact_id_ = text(*metadata, "compact_notice_id");
        }
        const auto envelope = mesh::inter_agent_envelope_from_metadata(*metadata);
        if (envelope) {
            transfers_.push_back({{"seq", event.seq},
                {"sender", envelope->sender}, {"recipient", envelope->recipient},
                {"sender_session_id", envelope->sender_session_id},
                {"type", mesh::inter_agent_message_type_name(envelope->type)}});
            if (transfers_.size() > 16) transfers_.erase(transfers_.begin());
        }
        break;
    }
    default: break;
    }
}

nlohmann::json SessionActivityState::snapshot() const {
    auto phase = phase_;
    if (!permissions_.empty()) phase = "permission_waiting";
    else if (!questions_.empty()) phase = "question_waiting";
    else if (!tools_.empty()) phase = "tool_running";
    return {{"seq", seq_}, {"known", known_}, {"busy", busy_},
        {"turn_id", turn_id_}, {"outcome", outcome_}, {"phase", phase},
        {"label", label_}, {"tool", tool_}, {"compact_id", compact_id_},
        {"transfers", transfers_}};
}
} // namespace acecode
