#include "trajectory_legacy_projection.hpp"

#include "message_payload.hpp"
#include "../session/compact_checkpoint.hpp"
#include "../session/session_rewind.hpp"
#include "../session/tool_result_storage.hpp"
#include "../session/turn_timing.hpp"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace acecode::web {
namespace {

struct RecordedCoverage {
    std::unordered_set<std::string> message_ids;
    std::unordered_set<std::string> tool_call_ids;
    std::unordered_set<std::string> turn_ids;
};

RecordedCoverage collect_recorded_coverage(
    const std::vector<SessionTrajectoryRecord>& records) {
    RecordedCoverage coverage;
    for (const auto& record : records) {
        const auto& payload = record.payload;
        if (!payload.is_object()) continue;
        if (record.type == "message") {
            const std::string id = payload.value("id", std::string{});
            if (!id.empty()) coverage.message_ids.insert(id);
        } else if (record.type == "model_response") {
            const std::string id = payload.value("message_id", std::string{});
            if (!id.empty()) coverage.message_ids.insert(id);
            if (auto calls = payload.find("tool_calls");
                calls != payload.end() && calls->is_array()) {
                for (const auto& call : *calls) {
                    if (!call.is_object()) continue;
                    const std::string call_id =
                        call.value("id", std::string{});
                    if (!call_id.empty()) {
                        coverage.tool_call_ids.insert(call_id);
                    }
                }
            }
        } else if (record.type == "tool_start" ||
                   record.type == "tool_end") {
            const std::string call_id =
                payload.value("tool_call_id", std::string{});
            if (!call_id.empty()) coverage.tool_call_ids.insert(call_id);
        } else if (record.type == "turn_start" ||
                   record.type == "turn_end") {
            const std::string turn_id =
                payload.value("turn_id", std::string{});
            if (!turn_id.empty()) coverage.turn_ids.insert(turn_id);
        }
    }
    return coverage;
}

std::unordered_map<std::string, TurnTimingRecord> collect_turn_timings(
    const std::vector<ChatMessage>& messages) {
    std::unordered_map<std::string, TurnTimingRecord> timings;
    for (const auto& message : messages) {
        if (!is_turn_timing_message(message)) continue;
        auto timing = decode_turn_timing(
            message.metadata.value("turn_timing", nlohmann::json{}));
        if (timing) timings[timing->user_message_uuid] = *timing;
    }
    return timings;
}

nlohmann::json make_legacy_record(std::size_t index,
                                  std::string type,
                                  nlohmann::json payload,
                                  nlohmann::json timestamp_ms = nullptr) {
    return nlohmann::json{
        {"schema_version", kSessionTrajectorySchemaVersion},
        {"sequence", nullptr},
        {"legacy_index", index},
        {"timestamp_ms", std::move(timestamp_ms)},
        {"type", std::move(type)},
        {"source", "legacy"},
        {"payload", std::move(payload)},
    };
}

bool assistant_calls_are_covered(const ChatMessage& message,
                                 const RecordedCoverage& coverage) {
    if (!message.tool_calls.is_array() || message.tool_calls.empty()) {
        return false;
    }
    bool saw_call = false;
    for (const auto& call : message.tool_calls) {
        if (!call.is_object()) return false;
        std::string id = call.value("id", std::string{});
        if (id.empty()) return false;
        saw_call = true;
        if (!coverage.tool_call_ids.count(id)) return false;
    }
    return saw_call;
}

} // namespace

LegacyTrajectoryPage project_legacy_trajectory(
    const std::vector<ChatMessage>& messages,
    const std::vector<SessionTrajectoryRecord>& recorded_records,
    std::size_t after,
    std::size_t limit) {
    LegacyTrajectoryPage page;
    page.missing_capabilities = {
        "model_request",
        "model_step_timing",
        "ttft",
        "tool_timing",
        "tool_schema",
    };
    limit = std::clamp<std::size_t>(
        limit == 0 ? kSessionTrajectoryDefaultPageSize : limit,
        1,
        kSessionTrajectoryMaxPageSize);

    const RecordedCoverage coverage =
        collect_recorded_coverage(recorded_records);
    const auto timings = collect_turn_timings(messages);
    std::vector<nlohmann::json> projected;
    projected.reserve(messages.size());

    for (const auto& message : messages) {
        if (is_file_checkpoint_message(message) ||
            is_compact_checkpoint_message(message) ||
            is_content_replacement_message(message)) {
            continue;
        }
        if (is_turn_timing_message(message)) {
            auto timing = decode_turn_timing(
                message.metadata.value("turn_timing", nlohmann::json{}));
            if (!timing || coverage.turn_ids.count(timing->user_message_uuid)) {
                continue;
            }
            nlohmann::json payload = encode_turn_timing(*timing);
            payload["turn_id"] = timing->user_message_uuid;
            payload["outcome"] = timing->status;
            projected.push_back(make_legacy_record(
                projected.size(), "legacy_turn_end", std::move(payload),
                timing->completed_at_ms > 0
                    ? nlohmann::json(timing->completed_at_ms)
                    : nlohmann::json(nullptr)));
            continue;
        }

        const std::string message_id = compute_message_id(message);
        if (coverage.message_ids.count(message_id)) continue;
        if (message.role == "tool" && !message.tool_call_id.empty() &&
            coverage.tool_call_ids.count(message.tool_call_id)) {
            continue;
        }
        if (message.role == "assistant" && message.content.empty() &&
            assistant_calls_are_covered(message, coverage)) {
            continue;
        }

        nlohmann::json payload = chat_message_to_payload_json(message);
        if (!message.timestamp.empty()) {
            payload["timestamp_text"] = message.timestamp;
        }
        nlohmann::json timestamp_ms = nullptr;
        std::string type = "legacy_message";
        if (message.role == "user") {
            const auto timing = timings.find(message.uuid);
            if (timing != timings.end() && timing->second.started_at_ms > 0) {
                timestamp_ms = timing->second.started_at_ms;
            }
            type = is_hidden_goal_context_message(message) || message.is_meta
                ? "legacy_context"
                : "legacy_user_message";
        } else if (message.role == "assistant") {
            payload["message_id"] = message_id;
            type = "legacy_model_response";
        } else if (message.role == "tool") {
            payload["output"] = message.content;
            type = "legacy_tool_result";
        } else if (message.role == "system" || message.is_meta) {
            type = "legacy_context";
        }
        projected.push_back(make_legacy_record(
            projected.size(), std::move(type), std::move(payload),
            std::move(timestamp_ms)));
    }

    page.total = projected.size();
    const std::size_t begin = std::min(after, projected.size());
    const std::size_t end = std::min(projected.size(), begin + limit);
    page.records.insert(
        page.records.end(),
        projected.begin() + static_cast<std::ptrdiff_t>(begin),
        projected.begin() + static_cast<std::ptrdiff_t>(end));
    page.next_after = end;
    page.has_more = end < projected.size();
    if (projected.empty()) page.missing_capabilities.clear();
    return page;
}

} // namespace acecode::web
