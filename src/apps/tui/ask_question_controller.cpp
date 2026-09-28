#include "ask_question_controller.hpp"

#include <algorithm>

namespace acecode::tui {

AskQuestionController::AskQuestionController(std::vector<AskQuestion> questions,
                                             AskQuestionConfig config,
                                             std::string origin_label)
    : questions_(std::move(questions)),
      config_(config),
      origin_label_(std::move(origin_label)) {
    states_.reserve(questions_.size());
    scroll_offsets_.assign(questions_.size(), 0);
    for (const auto& question : questions_) {
        QuestionState state;
        state.selected.assign(question.options.size(), false);
        state.visited = true;
        states_.push_back(std::move(state));
    }
    if (questions_.empty()) {
        finished_ = true;
        cancelled_ = true;
    }
}

AskQuestionController::QuestionState& AskQuestionController::current_state() {
    return states_[static_cast<std::size_t>(current_question_)];
}

const AskQuestionController::QuestionState&
AskQuestionController::current_state() const {
    return states_[static_cast<std::size_t>(current_question_)];
}

const AskQuestion& AskQuestionController::current_question_data() const {
    return questions_[static_cast<std::size_t>(current_question_)];
}

int AskQuestionController::custom_option_index() const {
    return static_cast<int>(current_question_data().options.size());
}

bool AskQuestionController::multi_select() const {
    return current_question_data().multi_select;
}

bool AskQuestionController::has_effective_answer(
    const QuestionState& state, const AskQuestion& question) const {
    if (state.custom_selected && !state.editor.text().empty()) return true;
    return std::any_of(state.selected.begin(), state.selected.end(),
                       [](bool selected) { return selected; });
}

AskQuestionAnswer AskQuestionController::answer_for(int question_index) const {
    const auto& question = questions_[static_cast<std::size_t>(question_index)];
    const auto& state = states_[static_cast<std::size_t>(question_index)];
    AskQuestionAnswer answer;
    for (std::size_t i = 0; i < state.selected.size(); ++i) {
        if (state.selected[i]) answer.selected.push_back(question.options[i].label);
    }
    if (state.custom_selected && !state.editor.text().empty()) {
        answer.custom_text = state.editor.text();
    }
    answer.not_answered = answer.selected.empty() && answer.custom_text.empty();
    answer.auto_selected = state.auto_selected;
    return answer;
}

AskQuestionSnapshot AskQuestionController::snapshot() const {
    AskQuestionSnapshot result;
    result.page = page_;
    result.current_question = current_question_;
    result.total_questions = static_cast<int>(questions_.size());
    result.scroll_offset = scroll_offset_;
    result.follow_focus = follow_focus_;
    result.editing_custom = editing_custom_;
    result.feedback_locked = feedback_locked_;
    result.completed = finished_;
    result.cancelled = cancelled_;
    result.origin_label = origin_label_;
    result.answers.reserve(questions_.size());
    result.question_options.reserve(questions_.size());
    result.custom_texts.reserve(questions_.size());
    result.question_texts.reserve(questions_.size());
    result.question_headers.reserve(questions_.size());
    for (int i = 0; i < static_cast<int>(questions_.size()); ++i) {
        const auto& question = questions_[static_cast<std::size_t>(i)];
        const auto& state = states_[static_cast<std::size_t>(i)];
        std::vector<AskQuestionItemSnapshot> items;
        items.reserve(question.options.size());
        for (std::size_t option = 0; option < question.options.size(); ++option) {
            items.push_back({question.options[option].label,
                             question.options[option].description,
                             question.options[option].recommended,
                             state.selected[option]});
        }
        result.question_options.push_back(std::move(items));
        result.custom_texts.push_back(state.editor.text());
        result.answers.push_back(answer_for(i));
        result.question_texts.push_back(question.question);
        result.question_headers.push_back(question.header);
    }
    if (page_ == AskQuestionPage::Summary || questions_.empty()) return result;

    const auto& question = current_question_data();
    const auto& state = current_state();
    result.focused_option = state.focused_option;
    result.multi_select = question.multi_select;
    result.question_text = question.question;
    result.question_header = question.header;
    result.custom_text = state.editor.text();
    result.custom_selected = state.custom_selected;
    result.editor.text = state.editor.text();
    result.editor.cursor = state.editor.cursor();
    result.editor.selection_anchor = state.editor.selection_anchor();
    result.editor.has_selection = state.editor.has_selection();
    for (std::size_t i = 0; i < question.options.size(); ++i) {
        result.options.push_back({question.options[i].label,
                                  question.options[i].description,
                                  question.options[i].recommended,
                                  state.selected[i]});
    }
    return result;
}

std::optional<AskQuestionCompletion> AskQuestionController::completion() const {
    if (!finished_) return std::nullopt;
    AskQuestionCompletion result;
    result.cancelled = cancelled_;
    result.timed_out = timed_out_;
    result.answers.reserve(questions_.size());
    for (int i = 0; i < static_cast<int>(questions_.size()); ++i) {
        result.answers.push_back(answer_for(i));
    }
    return result;
}

void AskQuestionController::select_preset(int option_index, bool ensure_only) {
    auto& state = current_state();
    const int option_count = static_cast<int>(state.selected.size());
    if (option_index < 0 || option_index >= option_count) return;
    state.focused_option = option_index;
    state.auto_selected = false;
    if (multi_select()) {
        state.selected[static_cast<std::size_t>(option_index)] = ensure_only
            ? true
            : !state.selected[static_cast<std::size_t>(option_index)];
        return;
    }
    std::fill(state.selected.begin(), state.selected.end(), false);
    state.selected[static_cast<std::size_t>(option_index)] = true;
    state.custom_selected = false;
    editing_custom_ = false;
}

void AskQuestionController::activate_custom(bool editing) {
    auto& state = current_state();
    state.focused_option = custom_option_index();
    if (!multi_select()) std::fill(state.selected.begin(), state.selected.end(), false);
    state.custom_selected = true;
    state.auto_selected = false;
    editing_custom_ = editing;
}

void AskQuestionController::clear_current_selection() {
    auto& state = current_state();
    std::fill(state.selected.begin(), state.selected.end(), false);
    state.custom_selected = false;
    state.auto_selected = false;
    editing_custom_ = false;
}

void AskQuestionController::enter_question(int index) {
    current_question_ = std::clamp(index, 0,
                                   static_cast<int>(questions_.size()) - 1);
    page_ = AskQuestionPage::Question;
    editing_custom_ = false;
    states_[static_cast<std::size_t>(current_question_)].visited = true;
    scroll_offset_ = scroll_offsets_[static_cast<std::size_t>(current_question_)];
}

void AskQuestionController::advance_after_submit(
    std::vector<AskQuestionEffect>& effects, bool with_feedback) {
    if (with_feedback && config_.selection_feedback_ms > 0) {
        feedback_locked_ = true;
        effects.push_back({AskQuestionEffectKind::BeginSelectionFeedback, {},
                           config_.selection_feedback_ms});
        return;
    }
    if (questions_.size() == 1) {
        complete(effects, false);
    } else if (current_question_ + 1 < static_cast<int>(questions_.size())) {
        enter_question(current_question_ + 1);
    } else {
        page_ = AskQuestionPage::Summary;
        editing_custom_ = false;
        scroll_offsets_[static_cast<std::size_t>(current_question_)] = scroll_offset_;
        scroll_offset_ = 0;
    }
}

bool AskQuestionController::submit_current(
    std::vector<AskQuestionEffect>& effects, bool with_feedback) {
    if (page_ == AskQuestionPage::Summary) {
        complete(effects, false);
        return true;
    }
    auto& state = current_state();
    if (editing_custom_) {
        editing_custom_ = false;
    }
    // Empty active custom answers intentionally yield Not answered unless
    // another selected multi-answer exists.
    state.auto_selected = false;
    advance_after_submit(effects, with_feedback);
    return true;
}

void AskQuestionController::complete(std::vector<AskQuestionEffect>& effects,
                                     bool timed_out) {
    if (finished_) return;
    finished_ = true;
    timed_out_ = timed_out;
    feedback_locked_ = false;
    effects.push_back({AskQuestionEffectKind::Complete, {}});
}

void AskQuestionController::cancel(std::vector<AskQuestionEffect>& effects) {
    if (finished_) return;
    finished_ = true;
    cancelled_ = true;
    feedback_locked_ = false;
    effects.push_back({AskQuestionEffectKind::Cancel, {}});
}

bool AskQuestionController::handle_editing(
    const AskQuestionEvent& event, std::vector<AskQuestionEffect>& effects) {
    auto& editor = current_state().editor;
    switch (event.kind) {
        case AskQuestionEventKind::InsertText:
        case AskQuestionEventKind::PasteText:
            editor.insert(event.text);
            return true;
        case AskQuestionEventKind::ChooseNumber:
            // 数字在编辑态始终是普通文本，不能再次触发选项快捷键。
            editor.insert(std::to_string(event.option_index));
            return true;
        case AskQuestionEventKind::InsertNewline:
            editor.insert("\n");
            return true;
        case AskQuestionEventKind::Backspace: return editor.backspace();
        case AskQuestionEventKind::DeleteForward: return editor.erase_forward();
        case AskQuestionEventKind::MoveCursorLeft: return editor.move_left();
        case AskQuestionEventKind::MoveCursorRight: return editor.move_right();
        case AskQuestionEventKind::MoveCursorUp: return editor.move_up();
        case AskQuestionEventKind::MoveCursorDown: return editor.move_down();
        case AskQuestionEventKind::SelectCursorLeft: return editor.move_left(true);
        case AskQuestionEventKind::SelectCursorRight: return editor.move_right(true);
        case AskQuestionEventKind::SelectCursorUp: return editor.move_up(true);
        case AskQuestionEventKind::SelectCursorDown: return editor.move_down(true);
        case AskQuestionEventKind::MoveCursorHome: editor.move_home(); return true;
        case AskQuestionEventKind::MoveCursorEnd: editor.move_end(); return true;
        case AskQuestionEventKind::MoveCursorTo:
            editor.move_cursor_to(event.cursor_byte, event.extend_selection);
            return true;
        case AskQuestionEventKind::CutSelection:
            if (editor.has_selection()) {
                effects.push_back({AskQuestionEffectKind::CutText,
                                   editor.selected_text()});
            }
            return true;
        case AskQuestionEventKind::DeleteSelection:
            return editor.erase_selection();
        case AskQuestionEventKind::CopySelection:
            if (editor.has_selection()) {
                effects.push_back({AskQuestionEffectKind::CopyText,
                                   editor.selected_text()});
            }
            return true;
        case AskQuestionEventKind::Escape:
            if (editor.text().empty()) current_state().custom_selected = false;
            editing_custom_ = false;
            return true;
        case AskQuestionEventKind::SubmitFocused:
            return submit_current(effects, false);
        default:
            return false;
    }
}

std::vector<AskQuestionEffect> AskQuestionController::handle(
    const AskQuestionEvent& event) {
    std::vector<AskQuestionEffect> effects;
    if (finished_) return effects;
    if (event.kind == AskQuestionEventKind::GlobalCancel) {
        cancel(effects);
        return effects;
    }
    if (event.kind == AskQuestionEventKind::TimeoutElapsed) {
        for (std::size_t qi = 0; qi < questions_.size(); ++qi) {
            auto& state = states_[qi];
            const auto& question = questions_[qi];
            if (state.custom_selected || has_effective_answer(state, question)) {
                // An activated custom row is an explicit answer state even when
                // its draft is empty: timeout must preserve it as Not answered
                // instead of silently replacing it with Recommended.
                continue;
            }
            for (std::size_t oi = 0; oi < question.options.size(); ++oi) {
                if (question.options[oi].recommended) {
                    std::fill(state.selected.begin(), state.selected.end(), false);
                    state.selected[oi] = true;
                    state.auto_selected = true;
                    break;
                }
            }
        }
        complete(effects, true);
        return effects;
    }
    if (feedback_locked_) {
        if (event.kind == AskQuestionEventKind::GlobalCancel) {
            cancel(effects);
        } else if (event.kind == AskQuestionEventKind::Escape &&
                   page_ == AskQuestionPage::Summary) {
            cancel(effects);
        } else if (event.kind == AskQuestionEventKind::SelectionFeedbackElapsed) {
            feedback_locked_ = false;
            advance_after_submit(effects, false);
            effects.push_back({AskQuestionEffectKind::Redraw, {}});
        }
        return effects;
    }
    if (event.kind == AskQuestionEventKind::ScrollLines) {
        follow_focus_ = false;
        scroll_offset_ = std::max(0, scroll_offset_ + event.line_delta);
        if (event.max_scroll_offset >= 0) {
            scroll_offset_ = std::min(scroll_offset_, event.max_scroll_offset);
        }
        if (page_ == AskQuestionPage::Question &&
            current_question_ >= 0 &&
            current_question_ < static_cast<int>(scroll_offsets_.size())) {
            scroll_offsets_[static_cast<std::size_t>(current_question_)] = scroll_offset_;
        }
        effects.push_back({AskQuestionEffectKind::Redraw, {}});
        return effects;
    }
    if (event.kind == AskQuestionEventKind::SetScrollOffset) {
        scroll_offset_ = std::max(0, event.line_delta);
        if (event.max_scroll_offset >= 0) {
            scroll_offset_ = std::min(scroll_offset_, event.max_scroll_offset);
        }
        if (page_ == AskQuestionPage::Question &&
            current_question_ >= 0 &&
            current_question_ < static_cast<int>(scroll_offsets_.size())) {
            scroll_offsets_[static_cast<std::size_t>(current_question_)] = scroll_offset_;
        }
        effects.push_back({AskQuestionEffectKind::Redraw, {}});
        return effects;
    }
    if (event.kind != AskQuestionEventKind::CopyFocused &&
        event.kind != AskQuestionEventKind::CopySelection &&
        event.kind != AskQuestionEventKind::CutSelection) {
        follow_focus_ = true;
    }
    if (page_ == AskQuestionPage::Summary) {
        switch (event.kind) {
            case AskQuestionEventKind::SubmitFocused:
            case AskQuestionEventKind::SubmitCurrentSelection:
                complete(effects, false);
                break;
            case AskQuestionEventKind::OpenQuestion:
                if (event.option_index >= 0 &&
                    event.option_index < static_cast<int>(questions_.size())) {
                    enter_question(event.option_index);
                }
                break;
            case AskQuestionEventKind::Escape:
                cancel(effects);
                break;
            case AskQuestionEventKind::MoveLeft:
            case AskQuestionEventKind::PreviousPage:
                enter_question(static_cast<int>(questions_.size()) - 1);
                break;
            case AskQuestionEventKind::MoveRight:
            case AskQuestionEventKind::NextPage:
                enter_question(0);
                break;
            default:
                return effects;
        }
        effects.push_back({AskQuestionEffectKind::Redraw, {}});
        return effects;
    }
    if (editing_custom_ && handle_editing(event, effects)) {
        effects.push_back({AskQuestionEffectKind::Redraw, {}});
        return effects;
    }

    auto& state = current_state();
    const int custom = custom_option_index();
    const int option_count = custom;
    switch (event.kind) {
        case AskQuestionEventKind::FocusOption:
            state.focused_option = std::clamp(event.option_index, 0, custom);
            break;
        case AskQuestionEventKind::MoveUp:
            state.focused_option = std::max(0, state.focused_option - 1); break;
        case AskQuestionEventKind::MoveDown:
            state.focused_option = std::min(custom, state.focused_option + 1); break;
        case AskQuestionEventKind::MoveLeft:
        case AskQuestionEventKind::PreviousPage:
            if (questions_.size() > 1 && current_question_ > 0) {
                enter_question(current_question_ - 1);
            }
            break;
        case AskQuestionEventKind::MoveRight:
        case AskQuestionEventKind::NextPage:
            if (questions_.size() > 1) {
                if (current_question_ + 1 < static_cast<int>(questions_.size())) {
                    enter_question(current_question_ + 1);
                } else {
                    page_ = AskQuestionPage::Summary;
                }
            }
            break;
        case AskQuestionEventKind::ToggleFocused:
        case AskQuestionEventKind::ToggleFocusedWithoutSubmit:
            if (state.focused_option == custom) {
                if (state.custom_selected &&
                    event.kind == AskQuestionEventKind::ToggleFocusedWithoutSubmit) {
                    // Only this option is toggled; keep the draft and any other
                    // selected answers in a multi-select question.
                    state.custom_selected = false;
                    state.auto_selected = false;
                    editing_custom_ = false;
                } else {
                    activate_custom(true);
                }
            } else if (multi_select()) {
                select_preset(state.focused_option, false);
            } else {
                if (state.selected[static_cast<std::size_t>(state.focused_option)]) {
                    clear_current_selection();
                } else {
                    select_preset(state.focused_option, true);
                }
            }
            break;
        case AskQuestionEventKind::ChooseOption:
            if (event.option_index == custom) {
                activate_custom(true);
            } else if (event.option_index >= 0 && event.option_index < option_count) {
                select_preset(event.option_index, true);
                submit_current(effects, true);
            }
            break;
        case AskQuestionEventKind::ChooseNumber:
            if (event.option_index <= 0) return effects;
            if (event.option_index <= option_count) {
                // 1..N 对应显式选项；N+1 对应自动追加的自定义行。
                const int option = event.option_index - 1;
                select_preset(option, true);
                submit_current(effects, true);
            } else if (event.option_index == custom + 1) {
                activate_custom(true);
            } else {
                // 无对应选项的数字进入自定义编辑，并保留该数字作为首字符。
                activate_custom(true);
                state.editor.insert(std::to_string(event.option_index));
            }
            break;
        case AskQuestionEventKind::BeginCustom:
            activate_custom(true); break;
        case AskQuestionEventKind::InsertText:
        case AskQuestionEventKind::PasteText:
            activate_custom(true);
            state.editor.insert(event.text);
            break;
        case AskQuestionEventKind::SubmitCurrentSelection:
            submit_current(effects, state.focused_option != custom);
            break;
        case AskQuestionEventKind::SubmitFocused:
            if (state.focused_option == custom) {
                if (state.custom_selected) {
                    submit_current(effects, false);
                } else {
                    activate_custom(true);
                }
            } else {
                select_preset(state.focused_option, true);
                submit_current(effects, true);
            }
            break;
        case AskQuestionEventKind::CopyFocused:
            if (state.focused_option == custom) {
                if (state.custom_selected && !state.editor.text().empty()) {
                    effects.push_back({AskQuestionEffectKind::CopyText,
                                       state.editor.text()});
                }
            } else if (state.focused_option >= 0 &&
                       state.focused_option < option_count) {
                const auto& option = current_question_data().options[
                    static_cast<std::size_t>(state.focused_option)];
                effects.push_back({AskQuestionEffectKind::CopyText,
                                   option.label + " " + option.description});
            }
            break;
        case AskQuestionEventKind::Escape:
            clear_current_selection(); break;
        default:
            return effects;
    }
    if (effects.empty() || std::none_of(
            effects.begin(), effects.end(), [](const AskQuestionEffect& effect) {
                return effect.kind == AskQuestionEffectKind::BeginSelectionFeedback;
            })) {
        effects.push_back({AskQuestionEffectKind::Redraw, {}});
    }
    return effects;
}

} // namespace acecode::tui
