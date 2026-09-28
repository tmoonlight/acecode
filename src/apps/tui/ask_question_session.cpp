#include "ask_question_session.hpp"

#include <algorithm>
#include <cctype>

namespace acecode::tui {

AskQuestionSession::AskQuestionSession(std::vector<AskQuestion> questions,
                                       AskQuestionConfig config,
                                       std::string origin_label,
                                       int timeout_seconds,
                                       TimePoint started_at)
    : controller_(std::move(questions), config, std::move(origin_label)) {
    if (timeout_seconds > 0) {
        timeout_deadline_ = started_at + std::chrono::seconds(timeout_seconds);
    }
}

void AskQuestionSession::observe_effects(
    const std::vector<AskQuestionEffect>& effects, TimePoint now) {
    for (const auto& effect : effects) {
        if (effect.kind == AskQuestionEffectKind::BeginSelectionFeedback) {
            feedback_deadline_ = now +
                std::chrono::milliseconds(std::max(0, effect.duration_ms));
            break;
        }
    }
    if (controller_.finished()) {
        feedback_deadline_.reset();
        timeout_deadline_.reset();
    }
}

std::vector<AskQuestionEffect> AskQuestionSession::dispatch_internal(
    const AskQuestionEvent& event, TimePoint now) {
    auto effects = controller_.handle(event);
    observe_effects(effects, now);
    return effects;
}

std::vector<AskQuestionEffect> AskQuestionSession::dispatch(
    const AskQuestionEvent& event, TimePoint now) {
    if (controller_.finished()) return {};
    if (event.kind != AskQuestionEventKind::Escape &&
        event.kind != AskQuestionEventKind::SetScrollOffset) {
        // The double-Esc rule applies to consecutive Esc presses. Any other
        // interaction breaks that sequence. Layout acknowledgment is a render
        // effect, not a user interaction, and must leave the sequence intact.
        escape_armed_ = false;
        last_escape_.reset();
    }
    if (event.kind == AskQuestionEventKind::SelectionFeedbackElapsed) {
        feedback_deadline_.reset();
    }
    return dispatch_internal(event, now);
}

std::vector<AskQuestionEffect> AskQuestionSession::tick(TimePoint now) {
    if (controller_.finished()) return {};

    // Timeout is an absolute deadline. It wins over a still-running feedback
    // timer, so a slow animation frame cannot extend the configured timeout.
    if (timeout_deadline_.has_value() && now >= *timeout_deadline_) {
        timeout_deadline_.reset();
        feedback_deadline_.reset();
        return dispatch_internal(
            {AskQuestionEventKind::TimeoutElapsed}, now);
    }
    if (feedback_deadline_.has_value() && now >= *feedback_deadline_) {
        feedback_deadline_.reset();
        return dispatch_internal(
            {AskQuestionEventKind::SelectionFeedbackElapsed}, now);
    }
    return {};
}

std::vector<AskQuestionEffect> AskQuestionSession::escape(TimePoint now) {
    if (controller_.finished()) return {};
    // 第一次 Esc 只退出编辑态/清空当前选择；1s 内第二次才取消整个会话。
    constexpr auto kDoubleEscapeWindow = std::chrono::seconds(1);
    if (escape_armed_ && last_escape_.has_value() &&
        now - *last_escape_ <= kDoubleEscapeWindow) {
        escape_armed_ = false;
        last_escape_.reset();
        return dispatch_internal({AskQuestionEventKind::GlobalCancel}, now);
    }
    escape_armed_ = true;
    last_escape_ = now;
    auto effects = dispatch_internal({AskQuestionEventKind::Escape}, now);
    return effects;
}

AskQuestionEvent ask_question_character_event(const std::string& character,
                                              bool editing_custom) {
    if (editing_custom) {
        return {AskQuestionEventKind::InsertText, -1, 0, character};
    }
    if (character.size() == 1 && character[0] >= '1' && character[0] <= '9') {
        return {AskQuestionEventKind::ChooseNumber,
                character[0] - '0', 0, {}};
    }
    return {AskQuestionEventKind::InsertText, -1, 0, character};
}

} // namespace acecode::tui
