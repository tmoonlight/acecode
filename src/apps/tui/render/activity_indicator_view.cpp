#include "tui/render/activity_indicator_view.hpp"
#include "tui/theme_palette.hpp"
#include "tui/text_style.hpp"
#include <algorithm>
#include <utility>
#include <ftxui/screen/string.hpp>
using namespace ftxui;
#include "tui/tool_progress.hpp"
#include "tui/compact_animation.hpp"
#include "tui/thinking_animation.hpp"
#include "tui/thinking_heartbeat.hpp"
#include "tui/render/status_chips.hpp"
#include "tui/model/mcp_sidebar_model.hpp"

namespace acecode::tui {
ActivityIndicatorView render_activity_indicator_view(const TuiState& state,
    bool conhost_compat_layout, bool show_regular_sidebar, int anim_tick) {
    // -- Thinking indicator / tool progress --
    // Priority: if a tool is streaming output, show the live tool-progress
    // element instead of the thinking animation (the tool is the more
    // specific "in-progress" signal).
    Element thinking_element = emptyElement();
    if (!conhost_compat_layout && state.tool_running) {
        thinking_element = render_tool_progress(state);
    } else if (!conhost_compat_layout && state.is_waiting) {
        if (state.is_compacting) {
            const auto compact_now = std::chrono::steady_clock::now();
            const auto compact_origin =
                state.compact_animation_start_time.time_since_epoch().count() != 0
                    ? state.compact_animation_start_time
                    : compact_now;
            const long long compact_elapsed_ms = std::max<long long>(
                0, std::chrono::duration_cast<std::chrono::milliseconds>(
                       compact_now - compact_origin).count());
            const std::vector<std::string> compact_glyphs =
                ftxui::Utf8ToGlyphs("Compacting conversation...");
            const auto compact_frame = tui::make_compact_animation_frame(
                compact_glyphs.size(), compact_elapsed_ms);
            Elements compact_chars;
            for (std::size_t i = 0; i < compact_glyphs.size(); ++i) {
                const bool highlighted =
                    compact_frame.highlighted_background[i];
                Element glyph = text(compact_glyphs[i]) |
                    color(highlighted
                              ? tui::theme().ui.selection_fg
                              : tui::theme().ui.text_primary);
                if (highlighted) {
                    glyph = glyph | bgcolor(tui::theme().ui.selection_bg);
                }
                compact_chars.push_back(std::move(glyph));
            }
            thinking_element = hbox({
                text(" \xE2\x97\x8F ") | color(tui::theme().ui.accent),
                hbox(std::move(compact_chars)),
            });
        } else {
        const auto thinking_now = std::chrono::steady_clock::now();
        const auto animation_origin =
            state.thinking_start_time.time_since_epoch().count() != 0
                ? state.thinking_start_time
                : std::chrono::steady_clock::time_point{};
        const long long animation_elapsed_ms = std::max<long long>(
            0, std::chrono::duration_cast<std::chrono::milliseconds>(
                   thinking_now - animation_origin).count());

        // smooth-tui-thinking-animation:短语和固定三个点共用一条基于真实
        // elapsed time 的方向性流光。黄色尾迹接亮白核心,前沿自然回落到灰色;
        // 自适应采样只改变帧密度,漏帧时也会直接回到正确 phase。
        const std::vector<std::string> thinking_glyphs =
            ftxui::Utf8ToGlyphs(state.current_thinking_phrase + "...");
        const auto animation_frame = tui::make_thinking_animation_frame(
            thinking_glyphs.size(), animation_elapsed_ms);
        Elements chars;
        for (std::size_t i = 0; i < thinking_glyphs.size(); ++i) {
            const auto& highlight = animation_frame.glyph_highlights[i];
            const Color warm_color = Color::Interpolate(
                highlight.warm,
                tui::theme().ui.text_dim,
                tui::theme().ui.accent);
            const Color glyph_color = Color::Interpolate(
                highlight.white,
                warm_color,
                Color::White);
            chars.push_back(text(thinking_glyphs[i]) | color(glyph_color));
        }

        // inline-thinking-heartbeat:动画短语右侧挂 "[Ns · ↓ X tokens]" 数据段。
        // 秒数/token 的刷新搭 anim_tick 动画循环的便车,零额外重绘成本。
        // thinking_start_time 停在 time_point{} 原点时跳过(未打点时秒数会是
        // 天文数字,先例见 on_message 处的同款防御)。
        Element heartbeat = emptyElement();
        if (state.thinking_start_time.time_since_epoch().count() != 0) {
            const long hb_secs = static_cast<long>(animation_elapsed_ms / 1000);
            const long long hb_ms = animation_elapsed_ms;
            heartbeat = text("  " + tui::format_thinking_heartbeat(
                                        hb_secs, hb_ms,
                                        state.turn_completion_tokens_confirmed,
                                        state.streaming_output_chars))
                | dim | color(tui::theme().ui.accent_alt);
        }
        thinking_element = hbox({
            text(" \xE2\x97\x8F ") | color(tui::theme().ui.accent),
            hbox(std::move(chars)),
            heartbeat,
        });
        }
    }

    Element mcp_loading_element = emptyElement();
    if (!show_regular_sidebar && tui::mcp_sidebar_has_loading(state)) {
        mcp_loading_element = hbox({
            text(" i ") | bold | color(Color::White),
            tui::render_white_shimmer_text("MCP loading", anim_tick),
        });
    }

    return {std::move(thinking_element), std::move(mcp_loading_element)};
}

}
