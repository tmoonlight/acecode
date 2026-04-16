#pragma once

#include "permissions.hpp"

#include <string>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <functional>
#include <atomic>
#include <thread>

namespace acecode {

struct TuiState {
    struct Message {
        std::string role;
        std::string content;
        bool is_tool = false;
    };

    std::vector<Message> conversation;
    std::string input_text;
    bool is_waiting = false;
    std::string current_thinking_phrase = "Thinking";
    std::string status_line; // for auth/provider status
    std::string token_status; // for token usage display

    // Input history for up/down navigation
    std::vector<std::string> input_history;
    int history_index = -1; // -1 = not browsing history
    std::string saved_input; // saved current input when entering history

    // Pending message queue
    std::vector<std::string> pending_queue;

    // Tool confirmation state
    bool confirm_pending = false;
    std::string confirm_tool_name;
    std::string confirm_tool_args;
    PermissionResult confirm_result = PermissionResult::Deny;
    std::condition_variable confirm_cv;

    // Resume session picker state
    struct ResumeItem {
        std::string id;
        std::string display; // formatted display line
    };
    bool resume_picker_active = false;
    std::vector<ResumeItem> resume_items;
    int resume_selected = 0; // currently highlighted index
    std::function<void(const std::string& session_id)> resume_callback;

    int chat_focus_index = -1;
    bool chat_follow_tail = true;
    bool ctrl_c_armed = false;
    std::chrono::steady_clock::time_point last_ctrl_c_time{};

    // Async compact state
    bool is_compacting = false;                       // protected by mu
    std::atomic<bool> compact_abort_requested{false};  // cross-thread abort signal
    std::thread compact_thread;                        // background compaction thread

    std::mutex mu;
};

} // namespace acecode
