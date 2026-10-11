#include "thread_presentation.hpp"
#include "session/session_client.hpp"
#include "session/session_storage.hpp"
#include "utils/encoding.hpp"

#include <cctype>

namespace acecode::thread_presentation {

using nlohmann::json;

namespace { constexpr std::size_t kSummaryChars = 240; }

std::string trim_and_limit(std::string value,
                           std::size_t max_chars,
                           bool* truncated) {
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    const std::string limited = truncate_utf8_prefix(value, max_chars);
    if (truncated) *truncated = limited.size() < value.size();
    return limited;
}

json thread_summary(const SessionMeta& meta,
                    const SessionInfo* active,
                    bool pinned,
                    int pinned_index) {
    const bool is_active = active != nullptr;
    const bool busy = active && active->busy;
    json out{
        {"threadId", meta.id},
        {"cwd", meta.cwd},
        {"noWorkspace", meta.no_workspace},
        {"title", active && !active->title.empty() ? active->title : meta.title},
        {"summary", trim_and_limit(
            active && !active->summary.empty() ? active->summary : meta.summary,
            kSummaryChars)},
        {"createdAt", active && !active->created_at.empty()
                          ? active->created_at : meta.created_at},
        {"updatedAt", active && !active->updated_at.empty()
                          ? active->updated_at : meta.updated_at},
        {"active", is_active},
        {"busy", busy},
        {"status", busy ? "running" : "idle"},
        {"archived", meta.archived},
        {"pinned", pinned},
        {"parentThreadId", meta.parent_session_id.empty()
                               ? json(nullptr) : json(meta.parent_session_id)},
        {"model", active && !active->model_name.empty()
                      ? active->model_name : meta.model_preset},
    };
    if (pinned_index > 0) out["pinnedIndex"] = pinned_index;
    return out;
}

std::string event_kind_name(SessionEventKind kind) {
    return to_string(kind);
}


json compact_wait_event(const SessionEvent& event) {
    json out{
        {"cursor", std::to_string(event.seq)},
        {"type", event_kind_name(event.kind)},
    };
    if (!event.payload.is_object()) return out;
    for (const char* key : {
             "busy", "outcome", "reason", "request_id", "title"}) {
        if (event.payload.contains(key)) out[key] = event.payload[key];
    }
    if (event.kind == SessionEventKind::Message) {
        bool truncated = false;
        out["role"] = event.payload.value("role", std::string{});
        out["content"] = trim_and_limit(
            event.payload.value("content", std::string{}),
            1200, &truncated);
        out["truncated"] = truncated;
    }
    return out;
}

} // namespace acecode::thread_presentation
