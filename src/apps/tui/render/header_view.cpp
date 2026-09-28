#include "tui/render/header_view.hpp"
#include "tui/theme_palette.hpp"
#include "tui/text_style.hpp"
#include <algorithm>
#include <utility>
#include <ftxui/screen/string.hpp>
using namespace ftxui;
#include "tui/render/status_chips.hpp"

namespace acecode::tui {
ftxui::Element render_header_view(const TuiState& state,
    const std::string& version_str, const std::string& cwd_display,
    bool conhost_compat_layout, bool show_regular_sidebar, bool hide_regular_sidebar_banner) {
    const bool is_light = acecode::tui::theme().name == "light";
    Element header;
    if (conhost_compat_layout) {
        header = vbox({
            text(version_str) | color(tui::theme().ui.text_muted) | dim,
            state.update_notice.empty()
                ? emptyElement()
                : paragraph(state.update_notice) | color(tui::theme().semantic.warning),
            text(state.status_line) | color(tui::status_line_color(state.status_line)),
            text(cwd_display) | color(tui::theme().ui.accent_alt) | dim,
        }) | bgcolor(is_light ? Color::RGB(225, 235, 245) : Color::RGB(0, 30, 45));
    } else {
        // -- Logo --
        auto logo = vbox({
            text("\xE2\x96\x91\xE2\x96\x88\xE2\x96\x80\xE2\x96\x88\xE2\x96\x91\xE2\x96\x88\xE2\x96\x80\xE2\x96\x80\xE2\x96\x91\xE2\x96\x88\xE2\x96\x80\xE2\x96\x80\xE2"),
            text("\xE2\x96\x91\xE2\x96\x88\xE2\x96\x80\xE2\x96\x88\xE2\x96\x91\xE2\x96\x88\xE2\x96\x91\xE2\x96\x91\xE2\x96\x91\xE2\x96\x88\xE2\x96\x80\xE2\x96\x80\xE2"),
            text("\xE2\x96\x91\xE2\x96\x80\xE2\x96\x91\xE2\x96\x80\xE2\x96\x91\xE2\x96\x80\xE2\x96\x80\xE2\x96\x80\xE2\x96\x91\xE2\x96\x80\xE2\x96\x80\xE2\x96\x80\xE2"),
        }) | color(tui::theme().ui.border) | bold;

        if (show_regular_sidebar && hide_regular_sidebar_banner) {
            header = emptyElement();
        } else if (show_regular_sidebar) {
            header = hbox({
                text("    "),
                logo,
                filler(),
                text("  "),
            }) | bgcolor(is_light ? Color::RGB(225, 235, 245) : Color::RGB(0, 30, 45));
        } else {
            header = hbox({
                text("    "),
                logo,
                filler(),
                vbox({
                    text(version_str) | color(tui::theme().ui.text_muted) | dim,
                    state.update_notice.empty()
                        ? emptyElement()
                        : paragraph(state.update_notice) | color(tui::theme().semantic.warning),
                    text(state.status_line) | color(tui::status_line_color(state.status_line)),
                    text(cwd_display) | color(tui::theme().ui.accent_alt) | dim,
                }),
                text("  "),
            }) | bgcolor(is_light ? Color::RGB(225, 235, 245) : Color::RGB(0, 30, 45));
        }
    }

    return header;
}

}
