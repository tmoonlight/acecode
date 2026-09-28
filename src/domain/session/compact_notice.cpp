#include "compact_notice.hpp"
#include "system_notice.hpp"

namespace acecode {

nlohmann::json make_compact_notice_metadata(const std::string& id,
                                            const std::string& stage,
                                            bool complete,
                                            nlohmann::json params) {
    const std::string code = complete ? "context_compacted"
        : stage == "error" ? "context_compact_failed"
        : stage == "warning" ? "context_compact_warning"
        : stage == "checkpoint" ? "context_checkpoint"
        : "context_compacting";
    return make_system_notice_metadata(code, std::move(params), nlohmann::json{
        {kCompactNoticeFlagKey, true},
        {kCompactNoticeIdKey, id},
        {kCompactNoticeStageKey, stage},
        {kCompactNoticeCompleteKey, complete},
    });
}

std::optional<CompactNotice> decode_compact_notice(
    const nlohmann::json& metadata) {
    if (!metadata.is_object() ||
        !metadata.value(kCompactNoticeFlagKey, false)) {
        return std::nullopt;
    }

    const std::string id = metadata.value(kCompactNoticeIdKey, std::string{});
    const std::string stage = metadata.value(
        kCompactNoticeStageKey, std::string{});
    if (id.empty() || stage.empty()) return std::nullopt;

    CompactNotice notice;
    notice.id = id;
    notice.stage = stage;
    notice.complete = metadata.value(kCompactNoticeCompleteKey, false);
    return notice;
}

std::optional<CompactNotice> decode_compact_notice(
    const ChatMessage& message) {
    return decode_compact_notice(message.metadata);
}

} // namespace acecode
