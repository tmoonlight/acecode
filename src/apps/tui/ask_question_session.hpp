#pragma once

#include "ask_question_controller.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace acecode::tui {

// AskQuestionController 的 TUI 适配层。它负责把真实世界的 deadline、
// 反馈锁定和双 Esc 边界收敛成控制器事件；不持有 TuiState、ScreenInteractive
// 或系统剪贴板，因此可以在不启动终端的情况下测试。
class AskQuestionSession {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    AskQuestionSession(std::vector<AskQuestion> questions,
                       AskQuestionConfig config = {},
                       std::string origin_label = {},
                       int timeout_seconds = 0,
                       TimePoint started_at = Clock::now());

    const AskQuestionController& controller() const { return controller_; }
    AskQuestionSnapshot snapshot() const { return controller_.snapshot(); }
    std::optional<AskQuestionCompletion> completion() const {
        return controller_.completion();
    }
    bool finished() const { return controller_.finished(); }
    bool timeout_enabled() const { return timeout_deadline_.has_value(); }
    std::optional<TimePoint> timeout_deadline() const { return timeout_deadline_; }
    std::optional<TimePoint> feedback_deadline() const { return feedback_deadline_; }

    std::vector<AskQuestionEffect> dispatch(const AskQuestionEvent& event,
                                            TimePoint now = Clock::now());
    std::vector<AskQuestionEffect> tick(TimePoint now = Clock::now());

    // 第一次 Esc 只退出编辑态/清空当前选择；1s 内第二次才取消整个会话。
    std::vector<AskQuestionEffect> escape(TimePoint now = Clock::now());
    bool escape_armed() const { return escape_armed_; }

private:
    std::vector<AskQuestionEffect> dispatch_internal(
        const AskQuestionEvent& event, TimePoint now);
    void observe_effects(const std::vector<AskQuestionEffect>& effects,
                         TimePoint now);

    AskQuestionController controller_;
    std::optional<TimePoint> timeout_deadline_;
    std::optional<TimePoint> feedback_deadline_;
    std::optional<TimePoint> last_escape_;
    bool escape_armed_ = false;
};

// 将 TUI 的单个可打印字符转换为领域事件。编辑态由 session/controller
// 决定它是普通文本还是快捷键；调用方只需先判断 snapshot.editing_custom。
AskQuestionEvent ask_question_character_event(const std::string& character,
                                              bool editing_custom);

} // namespace acecode::tui
