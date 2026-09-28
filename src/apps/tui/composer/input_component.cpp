#include "tui/composer/input_component.hpp"
#include <ftxui/component/component.hpp>

namespace acecode::tui {
class ComposerInput final : public ftxui::ComponentBase {
public:
    ComposerInput(TuiState& state, InputTextHitLayout& hit_layout)
        : state_(state), hit_layout_(hit_layout) {}
private:
    ftxui::Element render_text() {
        auto& state = state_;
        auto& input_hit_layout = hit_layout_;
        std::string display_text = state.input_text;
        size_t cursor = state.input_cursor;
        if (cursor > display_text.size()) cursor = display_text.size();
        input_hit_layout.input_value = display_text;
        input_hit_layout.regions.clear();
        if (display_text.empty()) {
            return acecode::tui::render_empty_input_prompt(
                &input_hit_layout.regions);
        }
        return acecode::tui::render_wrapped_input_text(
            display_text,
            cursor,
            &input_hit_layout.regions,
            state.input_selection_anchor);
    }
    ftxui::Element OnRender() override { return render_text() | ftxui::reflect(focus_box_); }
    bool Focusable() const override { return true; }
    bool OnEvent(ftxui::Event event) override {
        if (event.is_mouse() && focus_box_.Contain(event.mouse().x, event.mouse().y)) {
            if (!CaptureMouse(event)) return false;
            TakeFocus();
        }
        return false;
    }
    TuiState& state_;
    InputTextHitLayout& hit_layout_;
    ftxui::Box focus_box_;
};
ftxui::Component make_composer_input(TuiState& state, InputTextHitLayout& hit_layout) {
    return ftxui::Make<ComposerInput>(state, hit_layout);
}

}
