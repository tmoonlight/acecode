#pragma once

#include "ask_question_editor.hpp"
#include "tool/ask_user_question_types.hpp"

#include <optional>
#include <string>
#include <vector>

namespace acecode::tui {

struct AskQuestionConfig {
    int min_visible_rows = 4;
    int selection_feedback_ms = 200;
};

enum class AskQuestionPage {
    Question,
    Summary,
};

enum class AskQuestionEventKind {
    MoveUp,
    MoveDown,
    MoveLeft,
    MoveRight,
    NextPage,
    PreviousPage,
    FocusOption,
    OpenQuestion,
    ToggleFocused,
    ToggleFocusedWithoutSubmit,
    SubmitCurrentSelection,
    SubmitFocused,
    ChooseOption,
    ChooseNumber,
    BeginCustom,
    InsertText,
    Backspace,
    DeleteForward,
    MoveCursorLeft,
    MoveCursorRight,
    MoveCursorUp,
    MoveCursorDown,
    MoveCursorHome,
    MoveCursorEnd,
    MoveCursorTo,
    SelectCursorLeft,
    SelectCursorRight,
    SelectCursorUp,
    SelectCursorDown,
    InsertNewline,
    CutSelection,
    DeleteSelection,
    CopySelection,
    PasteText,
    CopyFocused,
    Escape,
    GlobalCancel,
    SelectionFeedbackElapsed,
    TimeoutElapsed,
    ScrollLines,
    SetScrollOffset,
};

struct AskQuestionEvent {
    AskQuestionEventKind kind;
    int option_index = -1;
    int line_delta = 0;
    std::string text;
    std::size_t cursor_byte = 0;
    // ScrollLines normally only needs a delta. Adapters that know the current
    // viewport may provide the legal upper bound so the pure controller never
    // retains an offset that the renderer must silently clamp later.
    int max_scroll_offset = -1;
    // Mouse dragging extends the editor selection from the press anchor.
    bool extend_selection = false;
};

enum class AskQuestionEffectKind {
    Redraw,
    CopyText,
    CutText,
    BeginSelectionFeedback,
    Complete,
    Cancel,
};

struct AskQuestionEffect {
    AskQuestionEffectKind kind;
    std::string text;
    int duration_ms = 0;
};

struct AskQuestionAnswer {
    std::vector<std::string> selected;
    std::string custom_text;
    bool not_answered = false;
    bool auto_selected = false;
};

struct AskQuestionCompletion {
    bool cancelled = false;
    bool timed_out = false;
    std::vector<AskQuestionAnswer> answers;
};

struct AskQuestionItemSnapshot {
    std::string label;
    std::string description;
    bool recommended = false;
    bool selected = false;
};

struct AskQuestionEditorSnapshot {
    std::string text;
    std::size_t cursor = 0;
    std::optional<std::size_t> selection_anchor;
    bool has_selection = false;
};

struct AskQuestionSnapshot {
    AskQuestionPage page = AskQuestionPage::Question;
    int current_question = 0;
    int total_questions = 0;
    int focused_option = 0;
    int scroll_offset = 0;
    // Manual scrolling suspends focus tracking until the next navigation/edit.
    bool follow_focus = true;
    bool editing_custom = false;
    bool feedback_locked = false;
    bool completed = false;
    bool cancelled = false;
    std::string origin_label;
    std::string question_text;
    std::string question_header;
    std::string custom_text;
    bool custom_selected = false;
    bool multi_select = false;
    AskQuestionEditorSnapshot editor;
    std::vector<AskQuestionItemSnapshot> options;
    std::vector<std::vector<AskQuestionItemSnapshot>> question_options;
    std::vector<std::string> custom_texts;
    std::vector<AskQuestionAnswer> answers;
    // Question text and header per index, aligned with `answers`. The summary
    // page pairs a question with its answer, so the text must travel with the
    // snapshot instead of staying private to the controller.
    std::vector<std::string> question_texts;
    std::vector<std::string> question_headers;
};

// Pure state machine for the TUI AskUserQuestion interaction. Adapters own
// clocks, clipboard and raw terminal events; this class owns only interaction
// semantics and returns the side effects an adapter must perform.
class AskQuestionController {
public:
    AskQuestionController(std::vector<AskQuestion> questions,
                          AskQuestionConfig config,
                          std::string origin_label = {});

    AskQuestionSnapshot snapshot() const;
    bool finished() const { return finished_; }
    std::optional<AskQuestionCompletion> completion() const;
    std::vector<AskQuestionEffect> handle(const AskQuestionEvent& event);

private:
    struct QuestionState {
        std::vector<bool> selected;
        bool custom_selected = false;
        AskQuestionEditor editor;
        int focused_option = 0;
        bool visited = false;
        bool auto_selected = false;
    };

    std::vector<AskQuestion> questions_;
    AskQuestionConfig config_;
    std::string origin_label_;
    std::vector<QuestionState> states_;
    std::vector<int> scroll_offsets_;
    int current_question_ = 0;
    AskQuestionPage page_ = AskQuestionPage::Question;
    int scroll_offset_ = 0;
    bool follow_focus_ = true;
    bool editing_custom_ = false;
    bool feedback_locked_ = false;
    bool finished_ = false;
    bool cancelled_ = false;
    bool timed_out_ = false;

    QuestionState& current_state();
    const QuestionState& current_state() const;
    const AskQuestion& current_question_data() const;
    int custom_option_index() const;
    bool multi_select() const;
    bool has_effective_answer(const QuestionState& state,
                              const AskQuestion& question) const;
    AskQuestionAnswer answer_for(int question_index) const;
    void select_preset(int option_index, bool ensure_only);
    void activate_custom(bool editing);
    void clear_current_selection();
    void advance_after_submit(std::vector<AskQuestionEffect>& effects,
                              bool with_feedback);
    void complete(std::vector<AskQuestionEffect>& effects, bool timed_out);
    void cancel(std::vector<AskQuestionEffect>& effects);
    void enter_question(int index);
    bool submit_current(std::vector<AskQuestionEffect>& effects,
                        bool with_feedback);
    bool handle_editing(const AskQuestionEvent& event,
                        std::vector<AskQuestionEffect>& effects);
};

} // namespace acecode::tui
