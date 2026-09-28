#pragma once
#include "tui/tui_state.hpp"
#include <atomic>
#include <ftxui/dom/elements.hpp>

namespace acecode::tui {
ftxui::Color token_progress_color(int percent);
ftxui::Color model_load_color(int percent);
ftxui::Element render_model_load_chip();
ftxui::Color status_line_color(const std::string& status_line);
ftxui::Element render_cache_hit_chip(const TuiState& state);
ftxui::Element render_token_usage_chip(const TuiState& state);
ftxui::Element queued_badge();
ftxui::Color mcp_sidebar_state_color(const std::string& state);
ftxui::Element render_white_shimmer_text(const std::string& label, int anim_tick, bool with_dots = true);
ftxui::Element render_pending_queue_block(const TuiState& state, int available_width);
ftxui::Element render_pending_attachment_block(const TuiState& state, int available_width);
extern std::atomic<int> g_model_load_percent;
} // namespace acecode::tui
