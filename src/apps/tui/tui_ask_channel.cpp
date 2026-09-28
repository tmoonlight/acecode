#include "tui_ask_channel.hpp"

#include "tool/ask_user_question_types.hpp"
#include "tool/ask_user_question_tool.hpp"
#include "tui_state.hpp"
#include "ask_question_session.hpp"
#include "utils/logger.hpp"

#include <ftxui/component/screen_interactive.hpp>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <vector>

namespace acecode::tui {

namespace {

// questions_to_payload 的逆向:overlay 渲染吃的是 AskQuestion 结构而不是 JSON。
// 字段名与 daemon 的 wire 契约一致,这里只做形状转换,不做校验 —— 参数校验
// 已经在工具层 validate_ask_user_question_args 里做过了。
std::vector<AskQuestion> questions_from_payload(
    const nlohmann::json& payload, std::vector<std::string>& question_ids) {
    std::vector<AskQuestion> out;
    if (!payload.is_array()) return out;
    for (const auto& item : payload) {
        if (!item.is_object()) continue;
        AskQuestion q;
        q.question = item.value("text", item.value("id", std::string{}));
        // IDs are transport data, independent of display text. Keep them in
        // question order, including when two questions share the same text.
        const auto id = item.value("id", std::string{});
        question_ids.push_back(id.empty() ? q.question : id);
        q.header = item.value("header", std::string{});
        q.multi_select = item.value("multiSelect", false);
        if (item.contains("options") && item["options"].is_array()) {
            for (const auto& option : item["options"]) {
                if (!option.is_object()) continue;
                AskOption o;
                o.label = option.value("label", std::string{});
                o.description = option.value("description", std::string{});
                o.recommended = ask_option_label_has_recommended_suffix(o.label);
                q.options.push_back(std::move(o));
            }
        }
        out.push_back(std::move(q));
    }
    return out;
}

nlohmann::json make_response(bool cancelled,
                             bool timed_out,
                             const std::vector<std::string>& question_order,
                             const std::vector<AskQuestionAnswer>& answers) {
    nlohmann::json out;
    out["cancelled"] = cancelled;
    out["timed_out"] = timed_out;
    nlohmann::json arr = nlohmann::json::array();
    for (std::size_t index = 0;
         index < question_order.size() && index < answers.size(); ++index) {
        const auto& answer = answers[index];
        arr.push_back(nlohmann::json{
            {"question_id", question_order[index]},
            {"selected", answer.selected},
            {"custom_text", answer.custom_text},
            {"not_answered", answer.not_answered},
            {"auto_selected", answer.auto_selected},
        });
    }
    out["answers"] = std::move(arr);
    return out;
}

} // namespace

nlohmann::json ask_via_tui_overlay(TuiState& state,
                                   ftxui::ScreenInteractive& screen,
                                   const nlohmann::json& questions_payload,
                                   const std::atomic<bool>* abort_flag,
                                   int timeout_seconds,
                                   const std::string& origin_label) {
    std::vector<std::string> question_order;
    const std::vector<AskQuestion> questions =
        questions_from_payload(questions_payload, question_order);

    if (questions.empty()) {
        return make_response(/*cancelled=*/true, false, question_order, {});
    }
    if (abort_flag && abort_flag->load()) {
        return make_response(/*cancelled=*/true, false, question_order, {});
    }

    AskQuestionConfig session_config;
    std::shared_ptr<AskQuestionSession> session;
    auto queue_ticket = std::make_shared<TuiState::AskQueueTicket>();
    {
        std::unique_lock<std::mutex> lk(state.mu);
        state.ask_queue.push_back(queue_ticket);
        // 请求按入队顺序获得 overlay。队首之外即使先抢到锁也必须等待，
        // 从而保证并发子任务的 AskUserQuestion 严格 FIFO。
        while (!(abort_flag && abort_flag->load()) &&
               (state.ask_queue.empty() || state.ask_queue.front() != queue_ticket ||
                state.ask_pending || state.ask_session || state.confirm_pending)) {
            state.overlay_cv.wait_for(lk, std::chrono::milliseconds(100));
        }
        if (abort_flag && abort_flag->load()) {
            const auto it = std::find(state.ask_queue.begin(), state.ask_queue.end(),
                                      queue_ticket);
            if (it != state.ask_queue.end()) state.ask_queue.erase(it);
            state.overlay_cv.notify_all();
            return make_response(/*cancelled=*/true, false, question_order, {});
        }
        state.ask_queue.pop_front();
        session_config = state.ask_config;
        session = std::make_shared<AskQuestionSession>(
            questions, session_config, origin_label, timeout_seconds);
        state.ask_session = session;
        state.ask_origin_label = origin_label;
        state.ask_pending = true;
        state.ask_timeout_hint_seconds = timeout_seconds > 0 ? timeout_seconds : 0;
        state.ask_completion_override.reset();
    }
    screen.PostEvent(ftxui::Event::Custom);

    bool ok = false;
    bool aborted = false;
    bool timed_out = false;
    std::vector<AskQuestionAnswer> structured_answers;
    {
        std::unique_lock<std::mutex> lk(state.mu);
        while (state.ask_pending && !session->finished() &&
               !(abort_flag && abort_flag->load())) {
            state.ask_cv.wait_for(lk, std::chrono::milliseconds(100));
            if (session->finished() || (abort_flag && abort_flag->load())) {
                break;
            }
            // The production TUI animation loop ticks the session, but the
            // channel must also enforce its deadline when no screen event loop
            // is running (for example during tests or a temporarily blocked
            // renderer). Keeping the clock at the session boundary preserves
            // the same timeout and feedback semantics in both paths.
            const auto session_effects = session->tick(
                AskQuestionSession::Clock::now());
            if (!session_effects.empty()) {
                screen.PostEvent(ftxui::Event::Custom);
            }
        }

        aborted = abort_flag && abort_flag->load();
        if (aborted && !session->finished()) {
            auto effects = session->dispatch(
                {AskQuestionEventKind::GlobalCancel});
            (void)effects;
        }
        const auto completion = session->completion();
        if (completion.has_value()) {
            ok = !completion->cancelled;
            timed_out = completion->timed_out;
            structured_answers = completion->answers;
        } else if (state.ask_completion_override.has_value()) {
            const auto& override_completion = *state.ask_completion_override;
            ok = !override_completion.cancelled;
            timed_out = override_completion.timed_out;
            structured_answers = override_completion.answers;
        }

        state.ask_pending = false;
        state.ask_session.reset();
        state.ask_completion_override.reset();
        state.ask_origin_label.clear();
        state.ask_timeout_hint_seconds = 0;
        state.overlay_cv.notify_all();
    }
    screen.PostEvent(ftxui::Event::Custom);

    if (timed_out && !aborted && !ok) {
        return make_response(/*cancelled=*/false, /*timed_out=*/true,
                             question_order, structured_answers);
    }
    if (aborted || !ok) {
        LOG_INFO("[AskUserQuestion] declined (aborted=" +
                 std::string(aborted ? "true" : "false") + ")");
        return make_response(/*cancelled=*/true, false, question_order, {});
    }
    return make_response(false, timed_out, question_order, structured_answers);
}

} // namespace acecode::tui
