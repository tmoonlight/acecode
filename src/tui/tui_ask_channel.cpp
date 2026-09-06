#include "tui_ask_channel.hpp"

#include "../tool/ask_user_question_tool.hpp"
#include "../tui_state.hpp"
#include "../utils/logger.hpp"

#include <ftxui/component/screen_interactive.hpp>

#include <chrono>
#include <mutex>
#include <vector>

namespace acecode::tui {

namespace {

// questions_to_payload 的逆向:overlay 渲染吃的是 AskQuestion 结构而不是 JSON。
// 字段名与 daemon 的 wire 契约一致,这里只做形状转换,不做校验 —— 参数校验
// 已经在工具层 validate_ask_user_question_args 里做过了。
std::vector<AskQuestion> questions_from_payload(const nlohmann::json& payload) {
    std::vector<AskQuestion> out;
    if (!payload.is_array()) return out;
    for (const auto& item : payload) {
        if (!item.is_object()) continue;
        AskQuestion q;
        q.question = item.value("text", item.value("id", std::string{}));
        q.header = item.value("header", std::string{});
        q.multi_select = item.value("multiSelect", false);
        if (item.contains("options") && item["options"].is_array()) {
            for (const auto& option : item["options"]) {
                if (!option.is_object()) continue;
                AskOption o;
                o.label = option.value("label", std::string{});
                o.description = option.value("description", std::string{});
                q.options.push_back(std::move(o));
            }
        }
        out.push_back(std::move(q));
    }
    return out;
}

nlohmann::json make_response(bool cancelled,
                             bool timed_out,
                             const nlohmann::json& answers_arr) {
    nlohmann::json out;
    out["cancelled"] = cancelled;
    out["timed_out"] = timed_out;
    out["answers"] = answers_arr;
    return out;
}

// 把 TuiState 的每题双入口状态转成协议 answers[] JSON
// (ask-user-question-dual-entry)。与 daemon 的 React payload 同一规则:
//   exclusive active → 只发 exclusive_text(selected 已空);
//   否则 → selected + supplement_text(非空才发)。
// 被互斥压制的 inactive 文本(独占激活时的补充旧文本)不进 payload。
nlohmann::json answers_from_state(const TuiState& state,
                                  const std::vector<AskQuestion>& questions) {
    nlohmann::json arr = nlohmann::json::array();
    for (std::size_t qi = 0; qi < questions.size(); ++qi) {
        const AskQuestion& q = questions[qi];
        const bool exclusive_active =
            qi < state.ask_exclusive_active.size() &&
            state.ask_exclusive_active[qi];

        std::vector<std::string> selected;
        if (!exclusive_active) {
            if (q.multi_select) {
                if (qi < state.ask_multi_selected_by_question.size()) {
                    const auto& sel = state.ask_multi_selected_by_question[qi];
                    for (std::size_t i = 0; i < sel.size() &&
                                            i < q.options.size(); ++i) {
                        if (sel[i]) selected.push_back(q.options[i].label);
                    }
                }
            } else {
                if (qi < state.ask_selected_options.size()) {
                    const int opt = state.ask_selected_options[qi];
                    if (opt >= 0 && opt < static_cast<int>(q.options.size())) {
                        selected.push_back(q.options[opt].label);
                    }
                }
            }
        }

        nlohmann::json ans = {
            {"question_id", q.question},
            {"selected", selected},
        };
        if (exclusive_active) {
            std::string text = qi < state.ask_exclusive_text.size()
                ? state.ask_exclusive_text[qi]
                : std::string{};
            // 独占文本为空 = 未完成题(离开校验已拦截,理论到不了这里),
            // 防御性不发空字段。
            if (!text.empty()) ans["exclusive_text"] = text;
        } else {
            std::string text = qi < state.ask_supplement_text.size()
                ? state.ask_supplement_text[qi]
                : std::string{};
            if (!text.empty()) ans["supplement_text"] = text;
        }
        arr.push_back(std::move(ans));
    }
    return arr;
}

} // namespace

nlohmann::json ask_via_tui_overlay(TuiState& state,
                                   ftxui::ScreenInteractive& screen,
                                   const nlohmann::json& questions_payload,
                                   const std::atomic<bool>* abort_flag,
                                   int timeout_seconds,
                                   const std::string& origin_label) {
    const std::vector<AskQuestion> questions =
        questions_from_payload(questions_payload);
    std::vector<std::string> question_order;
    question_order.reserve(questions.size());
    for (const auto& q : questions) question_order.push_back(q.question);

    const nlohmann::json empty_answers = nlohmann::json::array();
    if (questions.empty()) {
        return make_response(/*cancelled=*/true, false, empty_answers);
    }
    if (abort_flag && abort_flag->load()) {
        return make_response(/*cancelled=*/true, false, empty_answers);
    }

    {
        std::unique_lock<std::mutex> lk(state.mu);
        // 子代理并发后 overlay 可能被占用(主会话确认 / 另一个子会话的提问)。
        // 占用前排队等空闲;100ms 轮询保证 abort 可打断。
        while (!(abort_flag && abort_flag->load()) &&
               (state.ask_pending || state.confirm_pending)) {
            state.overlay_cv.wait_for(lk, std::chrono::milliseconds(100));
        }
        if (abort_flag && abort_flag->load()) {
            return make_response(/*cancelled=*/true, false, empty_answers);
        }
        state.ask_origin_label = origin_label;
        state.ask_pending = true;
        state.ask_payload_json = questions_payload.dump();
        state.ask_questions = questions;
        state.ask_question_order = question_order;
        // timeout 策略:overlay 顶部渲染静态提示「N 秒无操作将自动选择推荐项」;
        // 0 = 无提示。
        state.ask_timeout_hint_seconds = timeout_seconds > 0 ? timeout_seconds : 0;
        state.ask_result_ok = false;
        state.ask_current_question = 0;
        state.ask_submit_page = false;
        state.ask_submit_focus = 0;
        state.ask_option_focus = 0;
        state.ask_question_option_focus.assign(questions.size(), 0);
        state.ask_answered_questions.assign(questions.size(), false);
        state.ask_selected_options.assign(questions.size(), -1);
        state.ask_multi_selected_by_question.clear();
        state.ask_multi_selected_by_question.reserve(questions.size());
        for (const auto& q : questions) {
            state.ask_multi_selected_by_question.emplace_back(q.options.size(), false);
        }
        state.ask_exclusive_active.assign(questions.size(), false);
        state.ask_exclusive_text.assign(questions.size(), std::string{});
        state.ask_supplement_text.assign(questions.size(), std::string{});
        state.ask_input_target = AskInputTarget::None;
        state.ask_validation_error.clear();
        state.ask_multi_selected.assign(questions[0].options.size(), false);
        state.ask_scroll_offset = 0;
        state.ask_scroll_total_rows = 0;
        state.ask_scroll_visible_rows = 0;
        state.ask_scrollbar_dragging = false;
        state.ask_scroll_to_focus_requested = true;
    }
    screen.PostEvent(ftxui::Event::Custom);

    bool ok = false;
    bool aborted = false;
    bool timed_out = false;
    // answers 组装结果:锁内填充(读 state 需要持锁),锁外 return 使用。
    nlohmann::json answered_json = nlohmann::json::array();
    const bool has_deadline = timeout_seconds > 0;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
    {
        std::unique_lock<std::mutex> lk(state.mu);
        if (has_deadline) {
            // 500ms 粒度轮询 deadline(与 prompter 的 abort 轮询同风格)。
            while (state.ask_pending && !(abort_flag && abort_flag->load())) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    timed_out = true;
                    break;
                }
                state.ask_cv.wait_for(lk, std::chrono::milliseconds(500));
            }
        } else {
            state.ask_cv.wait(lk, [&state, abort_flag] {
                return !state.ask_pending || (abort_flag && abort_flag->load());
            });
        }
        aborted = abort_flag && abort_flag->load();
        ok = state.ask_result_ok;
        // 双入口结构化组装(锁内读状态;answers_from_state 之后不再有
        // 其它线程写这些 ask 字段 —— 工具线程已在本函数中醒来)。
        answered_json = answers_from_state(state, questions);
        // overlay 已关闭 —— 清理残留的临时 navigation 状态,防止下次打开时脏数据。
        state.ask_pending = false;
        state.ask_questions.clear();
        state.ask_question_order.clear();
        state.ask_multi_selected.clear();
        state.ask_multi_selected_by_question.clear();
        state.ask_question_option_focus.clear();
        state.ask_answered_questions.clear();
        state.ask_selected_options.clear();
        state.ask_exclusive_active.clear();
        state.ask_exclusive_text.clear();
        state.ask_supplement_text.clear();
        state.ask_input_target = AskInputTarget::None;
        state.ask_validation_error.clear();
        state.ask_current_question = 0;
        state.ask_submit_page = false;
        state.ask_submit_focus = 0;
        state.ask_option_focus = 0;
        state.ask_scroll_offset = 0;
        state.ask_scroll_total_rows = 0;
        state.ask_scroll_visible_rows = 0;
        state.ask_scrollbar_dragging = false;
        state.ask_scroll_to_focus_requested = false;
        state.ask_origin_label.clear();
        state.ask_timeout_hint_seconds = 0;
        // overlay 释放:唤醒排队占用者(主会话确认 / 其它子会话提问)。
        state.overlay_cv.notify_all();
    }
    screen.PostEvent(ftxui::Event::Custom);

    // 到点前一瞬用户已提交时 ok=true —— 按正常回答处理,用户真实意志优先。
    if (timed_out && !aborted && !ok) {
        return make_response(/*cancelled=*/false, /*timed_out=*/true,
                             empty_answers);
    }
    if (aborted || !ok) {
        LOG_INFO("[AskUserQuestion] declined (aborted=" +
                 std::string(aborted ? "true" : "false") + ")");
        return make_response(/*cancelled=*/true, false, empty_answers);
    }
    return make_response(false, false, answered_json);
}

} // namespace acecode::tui
