#include "tui/input/mouse_router.hpp"
#include "tui/terminal_key_event.hpp"
#include "tui/input/input_trace.hpp"
#include "utils/logger.hpp"
#include "tui/composer/clipboard_keys.hpp"
#include "tui/chat_file_link.hpp"
#include "tui/model/status_line.hpp"
#include "tui/chat_scroll.hpp"
#include "tui/drag_scroll.hpp"
#include "tui/vertical_scroll.hpp"
#include "platform/open_url.hpp"

using ftxui::Event;
using ftxui::Mouse;
using ftxui::Box;

namespace acecode::tui {
InputDisposition handle_mouse(TuiInputContext& context, const Event& event) {
    auto& state = context.state;
    auto& screen = context.screen;
    auto& viewport = context.viewport;
    auto& chat_box = context.viewport.chat_box;
    auto& scrollbar_box = context.geometry.scrollbar_box;
    auto& sidebar_content_box = context.geometry.sidebar_content_box;
    auto& sidebar_viewport_box = context.geometry.sidebar_viewport_box;
    auto& sidebar_scrollbar_box = context.geometry.sidebar_scrollbar_box;
    auto& chat_link_regions = context.geometry.chat_link_regions;
    auto& message_line_counts = context.viewport.message_line_counts;
    auto& message_spacer_rows_after = context.viewport.message_spacer_rows_after;
    if (event.is_mouse()) {
        auto mouse_event = event; // FTXUI exposes mouse() only on mutable Event.
        auto& mouse = mouse_event.mouse();

        // link-hover-tooltip (add-tui-hyperlinks 5.3): 任何按键按下
        // (点击/中键/滚轮)立即隐藏气泡 —— 点击即离开,气泡不再有意义。
        // 不 return,让正常点击流程继续。仅无按键 Moved 才更新悬停状态。
        if (mouse.motion == Mouse::Pressed) {
            std::lock_guard<std::mutex> lk(state.mu);
            if (!state.hover_link_href.empty() ||
                state.hover_link_visible) {
                state.hover_link_href.clear();
                state.hover_link_visible = false;
            }
        }

        // mouse-selection-copy / clipboard-paste: right-click copies the
        // current FTXUI selection to the system clipboard. With no
        // selection, terminal mouse tracking prevents the host context menu
        // on many Linux terminals, so use the click as an explicit
        // clipboard paste.
        if (auto result = tui::handle_clipboard_right_click(context, event);
                result != InputDisposition::Continue) return result;

        // TUI chat file links: exact reflected link hits take priority over
        // scrollbar and drag-selection startup. Non-local links fall
        // through so the existing selection path remains unchanged.
        if (mouse.button == Mouse::Left &&
            mouse.motion == Mouse::Pressed) {
            if (const auto href =
                    chat_link_regions.href_at(mouse.x, mouse.y)) {
                // add-tui-hyperlinks 5.1: 网页链接(http/https)在浏览器打开,
                // 本地文件链接才走 open_tui_chat_file_link(系统文件管理器)。
                // 两者语义不同:前者是远程 URL,后者是本地路径,不能混用。
                acecode::tui::TuiChatFileLinkResult opened{};
                const bool is_http_link =
                    acecode::is_openable_http_url(*href);
                if (is_http_link) {
                    const auto browser =
                        acecode::open_url_in_browser(*href);
                    opened.handled = true;
                    opened.ok = browser.ok;
                    opened.error = browser.error;
                } else {
                    opened = acecode::tui::open_tui_chat_file_link(
                        *href,
                        context.turn.cwd());
                }
                if (opened.handled) {
                    const std::string status_msg =
                        opened.ok
                            ? (is_http_link
                                   ? "Opened link in browser"
                                   : "Opened file location in system file manager")
                            : (is_http_link ? "Unable to open link: "
                                            : "Unable to open file location: ") +
                                  (opened.error.empty()
                                       ? std::string("unknown error")
                                       : opened.error);
                    {
                        std::lock_guard<std::mutex> lk(state.mu);
                        tui::set_transient_status_line_locked(
                            state,
                            status_msg);
                    }
                    screen.post_event(Event::Custom);
                    return InputDisposition::Consumed;
                }
            }
        }

        auto reflected_box_rows = [](const Box& box) {
            return box.IsEmpty() ? 0 : box.y_max - box.y_min + 1;
        };

        // Ctrl+O expanded sidebar: its scrollbar has priority over the chat
        // scrollbar and drag-selection state. The panel is non-selectable,
        // so consuming the press does not suppress a valid text selection.
        if (mouse.button == Mouse::Left && mouse.motion == Mouse::Pressed &&
            sidebar_scrollbar_box.Contain(mouse.x, mouse.y)) {
            std::lock_guard<std::mutex> lk(state.mu);
            const int content_rows =
                reflected_box_rows(sidebar_content_box);
            const int viewport_rows =
                reflected_box_rows(sidebar_viewport_box);
            if (state.transcript_expanded &&
                acecode::tui::vertical_max_scroll_top_row(
                    content_rows, viewport_rows) > 0) {
                const int track_height =
                    reflected_box_rows(sidebar_scrollbar_box);
                const auto geometry =
                    acecode::tui::vertical_scrollbar_thumb_geometry(
                        sidebar_scrollbar_box.y_min,
                        track_height,
                        content_rows,
                        viewport_rows,
                        state.sidebar_scroll_top_row);
                state.sidebar_scrollbar_dragging = true;
                state.sidebar_scrollbar_grab_offset_2x =
                    acecode::tui::vertical_scrollbar_grab_offset_2x(
                        mouse.y, geometry);
                state.sidebar_scroll_top_row =
                    acecode::tui::vertical_scrollbar_y_to_top_row_with_grab(
                        mouse.y,
                        sidebar_scrollbar_box.y_min,
                        geometry,
                        state.sidebar_scrollbar_grab_offset_2x);
                screen.post_event(Event::Custom);
                return InputDisposition::Consumed;
            }
        }

        // draggable-thick-scrollbar: 左键 Pressed 落在滚动条列时,优先
        // 进入"拖滚动条"分支,跳过下面的 drag-select 启动。两态互斥 ——
        // 一次按下不会同时开始选区拖拽和滚动条拖拽。
        if (mouse.button == Mouse::Left && mouse.motion == Mouse::Pressed &&
            scrollbar_box.Contain(mouse.x, mouse.y)) {
            std::lock_guard<std::mutex> lk(state.mu);
            viewport.sync_from_layout(state);
            // 快照 message_line_counts —— 流式输出追加新行时,拖动期间的
            // y → line 映射继续按按下瞬间的几何走,不被指针下扯走。
            state.drag_scrollbar_snapshot = message_line_counts;
            state.drag_scrollbar_phase = TuiState::DragScrollbarPhase::Dragging;
            const bool was_follow_tail = state.chat_follow_tail;
            // 一旦用户主动操纵滚动条,就视为离开尾巴跟随;松手时不自动恢复。
            state.chat_follow_tail = false;
            int track_height = scrollbar_box.y_max - scrollbar_box.y_min + 1;
            const int snapshot_count =
                static_cast<int>(state.drag_scrollbar_snapshot.size());
            const int viewport_rows = viewport.rows();
            const int max_top = acecode::tui::chat_max_scroll_top_row(
                state.drag_scrollbar_snapshot, snapshot_count,
                viewport_rows, message_spacer_rows_after);
            const int current_top = was_follow_tail
                ? max_top
                : state.chat_scroll_top_row;
            const auto geometry =
                acecode::tui::chat_scrollbar_thumb_geometry(
                    scrollbar_box.y_min, track_height,
                    state.drag_scrollbar_snapshot, snapshot_count,
                    viewport_rows, current_top,
                    message_spacer_rows_after);
            state.drag_scrollbar_grab_offset_2x =
                acecode::tui::chat_scrollbar_grab_offset_2x(mouse.y,
                                                            geometry);
            const int previous_top = state.chat_scroll_top_row;
            state.chat_scroll_top_row =
                acecode::tui::chat_scrollbar_y_to_top_row_with_grab(
                    mouse.y, scrollbar_box.y_min, geometry,
                    state.drag_scrollbar_grab_offset_2x);
            auto [idx, off] = acecode::tui::chat_focus_from_display_row(
                state.drag_scrollbar_snapshot, snapshot_count,
                state.chat_scroll_top_row,
                message_spacer_rows_after);
            if (idx >= 0) {
                state.chat_focus_index = idx;
                state.chat_line_offset = off;
                // 拖到底自然恢复 follow_tail;中间位置保持手动滚动。
                state.chat_follow_tail = state.chat_scroll_top_row >= max_top;
            }
            ACECODE_INPUT_TRACE(
            LOG_DEBUG("[scrollbar] pressed mouse=(" +
                      std::to_string(mouse.x) + "," +
                      std::to_string(mouse.y) + ") track=" +
                      tui::box_for_log(scrollbar_box) +
                      " track_height=" + std::to_string(track_height) +
                      " viewport_rows=" + std::to_string(viewport_rows) +
                      " messages=" + std::to_string(snapshot_count) +
                      " was_follow_tail=" +
                      std::string(was_follow_tail ? "1" : "0") +
                      " current_top=" + std::to_string(current_top) +
                      " previous_top=" + std::to_string(previous_top) +
                      " new_top=" +
                      std::to_string(state.chat_scroll_top_row) +
                      " max_top=" + std::to_string(max_top) +
                      " grab2x=" +
                      std::to_string(
                          state.drag_scrollbar_grab_offset_2x) +
                      " geometry=" +
                      tui::scrollbar_geometry_for_log(geometry) +
                      " focus=" + std::to_string(state.chat_focus_index) +
                      " offset=" +
                      std::to_string(state.chat_line_offset) +
                      " follow_tail=" +
                      std::string(state.chat_follow_tail ? "1" : "0"));
            );
            screen.post_event(Event::Custom);
            return InputDisposition::Consumed;
        }

        // drag-autoscroll: 跟踪左键按下/拖动/释放, 驱动 anim_thread 在用户
        // 把鼠标拖到 chat_box 顶部/底部时自动滚动并补偿 selection 坐标.
        // 不 return true, 让事件继续流向 FTXUI 的 HandleSelection 走原本的
        // 选区更新. Release/Moved 不做 chat_box.Contain 限制, 因为用户常常
        // 会把鼠标拖到窗口外松开, 此时我们仍要把状态机拉回 Idle.
        if (mouse.button == Mouse::Left) {
            if (mouse.motion == Mouse::Pressed) {
                // draggable-thick-scrollbar: 落在滚动条列上的 Pressed 已经
                // 在上一个分支被吞掉,这里只剩内容区的左键按下 → 启动选区拖拽。
                if (chat_box.Contain(mouse.x, mouse.y) &&
                    !scrollbar_box.Contain(mouse.x, mouse.y)) {
                    std::lock_guard<std::mutex> lk(state.mu);
                    ACECODE_INPUT_TRACE(
                    LOG_DEBUG("[drag-select] pressed start mouse=(" +
                              std::to_string(mouse.x) + "," +
                              std::to_string(mouse.y) + ") chat_box=" +
                              tui::box_for_log(chat_box) + " scrollbar_box=" +
                              tui::box_for_log(scrollbar_box) + " focus=" +
                              std::to_string(state.chat_focus_index) +
                              " offset=" +
                              std::to_string(state.chat_line_offset) +
                              " previous_phase=" +
                              tui::drag_phase_for_log(state.drag_phase));
                    );
                    state.drag_left_pressed = true;
                    state.last_mouse_x = mouse.x;
                    state.last_mouse_y = mouse.y;
                    state.drag_phase = drag_scroll::Phase::Dragging;
                    state.last_drag_scroll_at = {};
                }
            } else if (mouse.motion == Mouse::Released) {
                std::lock_guard<std::mutex> lk(state.mu);
                ACECODE_INPUT_TRACE(
                if (state.drag_scrollbar_phase ==
                    TuiState::DragScrollbarPhase::Dragging) {
                    LOG_DEBUG("[scrollbar] released mouse=(" +
                              std::to_string(mouse.x) + "," +
                              std::to_string(mouse.y) + ") track=" +
                              tui::box_for_log(scrollbar_box) + " top=" +
                              std::to_string(state.chat_scroll_top_row) +
                              " grab2x=" +
                              std::to_string(
                                  state.drag_scrollbar_grab_offset_2x) +
                              " focus=" +
                              std::to_string(state.chat_focus_index) +
                              " offset=" +
                              std::to_string(state.chat_line_offset) +
                              " follow_tail=" +
                              std::string(state.chat_follow_tail ? "1"
                                                                  : "0"));
                }
                LOG_DEBUG("[drag-select] released mouse=(" +
                          std::to_string(mouse.x) + "," +
                          std::to_string(mouse.y) + ") phase=" +
                          tui::drag_phase_for_log(state.drag_phase) +
                          " left_pressed=" +
                          std::to_string(state.drag_left_pressed ? 1 : 0) +
                          " focus=" +
                          std::to_string(state.chat_focus_index) +
                          " offset=" +
                          std::to_string(state.chat_line_offset));
                );
                state.drag_left_pressed = false;
                state.drag_phase = drag_scroll::Phase::Idle;
                state.last_drag_scroll_at = {};
                // draggable-thick-scrollbar: 任何左键 Released 都把滚动条
                // 拖拽态拉回 Idle,即使释放点已经离开滚动条列(用户经常
                // 拖出窗口外松开)。chat_follow_tail 不在这里恢复 —— 用户
                // 显式滚到了非尾位置,只在拖到底时由 Pressed 分支重新开启。
                state.drag_scrollbar_phase = TuiState::DragScrollbarPhase::Idle;
                state.drag_scrollbar_snapshot.clear();
                state.drag_scrollbar_grab_offset_2x = 0;
            }
        }
        if (mouse.motion == Mouse::Released) {
            std::lock_guard<std::mutex> lk(state.mu);
            if (state.sidebar_scrollbar_dragging) {
                state.sidebar_scrollbar_dragging = false;
                state.sidebar_scrollbar_grab_offset_2x = 0;
                screen.post_event(Event::Custom);
                return InputDisposition::Consumed;
            }
        }
        // 终端在拖动期间发 Moved 事件, button 字段通常是 None 而不是 Left.
        // 我们靠自己维护的 drag_left_pressed 判断是否处于拖动中.
        if (mouse.motion == Mouse::Moved) {
            std::lock_guard<std::mutex> lk(state.mu);
            if (state.sidebar_scrollbar_dragging) {
                const int content_rows =
                    reflected_box_rows(sidebar_content_box);
                const int viewport_rows =
                    reflected_box_rows(sidebar_viewport_box);
                const int track_height =
                    reflected_box_rows(sidebar_scrollbar_box);
                const auto geometry =
                    acecode::tui::vertical_scrollbar_thumb_geometry(
                        sidebar_scrollbar_box.y_min,
                        track_height,
                        content_rows,
                        viewport_rows,
                        state.sidebar_scroll_top_row);
                state.sidebar_scroll_top_row =
                    acecode::tui::vertical_scrollbar_y_to_top_row_with_grab(
                        mouse.y,
                        sidebar_scrollbar_box.y_min,
                        geometry,
                        state.sidebar_scrollbar_grab_offset_2x);
                screen.post_event(Event::Custom);
                return InputDisposition::Consumed;
            }
            // draggable-thick-scrollbar: 滚动条拖拽优先级高于 drag-select
            // —— 两态互斥,Pressed 分支保证只可能进一态。直接 early return,
            // 不让下面的 drag-autoscroll 分类运行,避免拖滚动条时选区被误激活。
            if (state.drag_scrollbar_phase ==
                TuiState::DragScrollbarPhase::Dragging) {
                int track_height =
                    scrollbar_box.y_max - scrollbar_box.y_min + 1;
                const int snapshot_count =
                    static_cast<int>(state.drag_scrollbar_snapshot.size());
                const int viewport_rows = viewport.rows();
                const int previous_top = state.chat_scroll_top_row;
                const auto geometry =
                    acecode::tui::chat_scrollbar_thumb_geometry(
                        scrollbar_box.y_min, track_height,
                        state.drag_scrollbar_snapshot, snapshot_count,
                        viewport_rows, state.chat_scroll_top_row,
                        message_spacer_rows_after);
                state.chat_scroll_top_row =
                    acecode::tui::chat_scrollbar_y_to_top_row_with_grab(
                        mouse.y, scrollbar_box.y_min, geometry,
                        state.drag_scrollbar_grab_offset_2x);
                auto [idx, off] = acecode::tui::chat_focus_from_display_row(
                    state.drag_scrollbar_snapshot, snapshot_count,
                    state.chat_scroll_top_row,
                    message_spacer_rows_after);
                if (idx >= 0) {
                    state.chat_focus_index = idx;
                    state.chat_line_offset = off;
                    state.chat_follow_tail =
                        state.chat_scroll_top_row >= geometry.max_top_row;
                }
                ACECODE_INPUT_TRACE(
                LOG_DEBUG("[scrollbar] moved mouse=(" +
                          std::to_string(mouse.x) + "," +
                          std::to_string(mouse.y) + ") track=" +
                          tui::box_for_log(scrollbar_box) +
                          " track_height=" +
                          std::to_string(track_height) +
                          " viewport_rows=" +
                          std::to_string(viewport_rows) +
                          " messages=" +
                          std::to_string(snapshot_count) +
                          " top=" + std::to_string(previous_top) +
                          "->" +
                          std::to_string(state.chat_scroll_top_row) +
                          " max_top=" +
                          std::to_string(geometry.max_top_row) +
                          " grab2x=" +
                          std::to_string(
                              state.drag_scrollbar_grab_offset_2x) +
                          " geometry=" +
                          tui::scrollbar_geometry_for_log(geometry) +
                          " focus=" +
                          std::to_string(state.chat_focus_index) +
                          " offset=" +
                          std::to_string(state.chat_line_offset) +
                          " follow_tail=" +
                          std::string(state.chat_follow_tail ? "1" : "0"));
                );
                screen.post_event(Event::Custom);
                return InputDisposition::Consumed;
            }
            if (state.drag_left_pressed) {
#if ACECODE_TUI_INPUT_TRACE
                const auto previous_phase = state.drag_phase;
#endif
                state.last_mouse_x = mouse.x;
                state.last_mouse_y = mouse.y;
                auto new_phase = drag_scroll::classify(
                    mouse.y, chat_box.y_min, chat_box.y_max, true,
                    drag_scroll::Config{});
                bool phase_changed = (new_phase != state.drag_phase);
                state.drag_phase = new_phase;
                ACECODE_INPUT_TRACE(
                LOG_DEBUG("[drag-select] moved mouse=(" +
                          std::to_string(mouse.x) + "," +
                          std::to_string(mouse.y) + ") chat_box=" +
                          tui::box_for_log(chat_box) + " phase=" +
                          tui::drag_phase_for_log(previous_phase) + "->" +
                          tui::drag_phase_for_log(new_phase) +
                          " changed=" +
                          std::to_string(phase_changed ? 1 : 0) +
                          " focus=" +
                          std::to_string(state.chat_focus_index) +
                          " offset=" +
                          std::to_string(state.chat_line_offset));
                );
                // 进入滚动阶段时立即 PostEvent, 让 anim_thread 尽快转到
                // 50ms 间隔 + 跑第一次 tick. 不 PostEvent 也会在下个 300ms
                // 唤醒读到新 phase, 但那个延迟体感很差.
                if (phase_changed &&
                    (new_phase == drag_scroll::Phase::ScrollingUp ||
                     new_phase == drag_scroll::Phase::ScrollingDown)) {
                    screen.post_event(Event::Custom);
                }
            }
            // link-hover-tooltip (add-tui-hyperlinks 5.3): 无按键 Moved
            // 做链接命中检测。拖动中(选区拖拽/滚动条拖拽)不显示气泡,
            // 直接清除。命中同一 href 只刷新指针坐标、不重置计时,避免
            // 指针在链接内微移导致 300ms 永远到不了;命中不同 href 或
            // 移出链接区域则重置/清除。state.mu 由本 Moved 分支入口持有。
            if (state.drag_left_pressed ||
                state.drag_scrollbar_phase ==
                    TuiState::DragScrollbarPhase::Dragging ||
                state.sidebar_scrollbar_dragging) {
                if (!state.hover_link_href.empty() ||
                    state.hover_link_visible) {
                    state.hover_link_href.clear();
                    state.hover_link_visible = false;
                }
            } else {
                const auto hit = chat_link_regions.href_at(
                    mouse.x, mouse.y);
                if (hit) {
                    if (state.hover_link_href != *hit) {
                        state.hover_link_href = *hit;
                        state.hover_link_since =
                            std::chrono::steady_clock::now();
                        state.hover_link_visible = false;
                    }
                    state.hover_link_x = mouse.x;
                    state.hover_link_y = mouse.y;
                } else if (!state.hover_link_href.empty() ||
                           state.hover_link_visible) {
                    state.hover_link_href.clear();
                    state.hover_link_visible = false;
                }
            }
        }

        std::lock_guard<std::mutex> lk(state.mu);
        const bool is_wheel_event = mouse.button == Mouse::WheelUp ||
                                    mouse.button == Mouse::WheelDown;
        constexpr int WHEEL_LINES = 3;
        const bool sidebar_mouse_target =
            state.transcript_expanded &&
            !sidebar_viewport_box.IsEmpty() &&
            sidebar_viewport_box.Contain(mouse.x, mouse.y);
        if (is_wheel_event && sidebar_mouse_target) {
            const int content_rows =
                reflected_box_rows(sidebar_content_box);
            const int viewport_rows =
                reflected_box_rows(sidebar_viewport_box);
            const int delta =
                mouse.button == Mouse::WheelUp
                    ? -WHEEL_LINES
                    : WHEEL_LINES;
            const int before = state.sidebar_scroll_top_row;
            state.sidebar_scroll_top_row =
                acecode::tui::vertical_scroll_top_row_by_lines(
                    before, delta, content_rows, viewport_rows);
            if (state.sidebar_scroll_top_row != before) {
                screen.post_event(Event::Custom);
            }
            return InputDisposition::Consumed;
        }
        const bool chat_mouse_target = acecode::tui::is_chat_mouse_target(
            mouse.x, mouse.y, chat_box.x_min, chat_box.y_min,
            chat_box.x_max, chat_box.y_max, is_wheel_event);
        if (!chat_mouse_target) {
            ACECODE_INPUT_TRACE(
            if (is_wheel_event) {
                LOG_DEBUG("[input] chat wheel ignored outside chat_box " +
                          tui::event_for_log(event) +
                          " chat_box=" + tui::box_for_log(chat_box));
            }
            );
            return InputDisposition::Declined;
        }
        ACECODE_INPUT_TRACE(
        if (is_wheel_event && !chat_box.Contain(mouse.x, mouse.y)) {
            LOG_DEBUG("[input] chat wheel accepted above chat_box for "
                      "terminal origin mismatch " +
                      tui::event_for_log(event) +
                      " chat_box=" + tui::box_for_log(chat_box));
        }
        );

        // 鼠标滚轮按行滚动 (3 行/notch, Win 默认值), 长消息不再被一格掠过。
        if (mouse.button == Mouse::WheelUp) {
            viewport.sync_from_layout(state);
#if ACECODE_TUI_INPUT_TRACE
            const int before_focus = state.chat_focus_index;
            const int before_offset = state.chat_line_offset;
            const bool before_tail = state.chat_follow_tail;
#endif
            const int actual = viewport.scroll_by_lines(state, -WHEEL_LINES);
            ACECODE_INPUT_TRACE(
            LOG_DEBUG("[input] chat wheel up delta=-" +
                      std::to_string(WHEEL_LINES) +
                      " actual=" + std::to_string(actual) +
                      " focus=" + std::to_string(before_focus) +
                      "->" + std::to_string(state.chat_focus_index) +
                      " offset=" + std::to_string(before_offset) +
                      "->" + std::to_string(state.chat_line_offset) +
                      " follow_tail=" +
                      std::string(before_tail ? "1" : "0") +
                      "->" +
                      std::string(state.chat_follow_tail ? "1" : "0"));
            );
            if (actual != 0) {
                screen.post_event(Event::Custom);
            }
            return InputDisposition::Consumed;
        }
        if (mouse.button == Mouse::WheelDown) {
            viewport.sync_from_layout(state);
#if ACECODE_TUI_INPUT_TRACE
            const int before_focus = state.chat_focus_index;
            const int before_offset = state.chat_line_offset;
            const bool before_tail = state.chat_follow_tail;
#endif
            const int actual = viewport.scroll_by_lines(state, WHEEL_LINES);
            ACECODE_INPUT_TRACE(
            LOG_DEBUG("[input] chat wheel down delta=" +
                      std::to_string(WHEEL_LINES) +
                      " actual=" + std::to_string(actual) +
                      " focus=" + std::to_string(before_focus) +
                      "->" + std::to_string(state.chat_focus_index) +
                      " offset=" + std::to_string(before_offset) +
                      "->" + std::to_string(state.chat_line_offset) +
                      " follow_tail=" +
                      std::string(before_tail ? "1" : "0") +
                      "->" +
                      std::string(state.chat_follow_tail ? "1" : "0"));
            );
            if (actual != 0) {
                screen.post_event(Event::Custom);
            }
            return InputDisposition::Consumed;
        }
    }
    return InputDisposition::Continue;
}

}
