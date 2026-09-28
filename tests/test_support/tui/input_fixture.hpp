#pragma once
#include "tui/input/tui_input_context.hpp"
#include "tui/commands/command_registry.hpp"
#include "test_support/tui/fake_screen_port.hpp"
#include "test_support/agent_loop/characterization_fixture.hpp"
#include "utils/utf8_path.hpp"
#include <stdexcept>

namespace acecode::tui::test_support {
struct FakeInputTurn final : ITurnSubmitter {
    explicit FakeInputTurn(std::string cwd) : cwd_(std::move(cwd)) {}
    std::string cwd() const override { return cwd_; }
    void cancel() override { ++cancellations; }
    void submit_shell(const std::string& command) override { shells.push_back(command); }
    void before_first_turn() override { ++before_calls; if (before) before(); }
    void submit_input(const UserInput& input) override { inputs.push_back(input); }
    void submit_text(const std::string& text, const std::string& display = {}) override {
        UserInput input; input.text = text; input.display_text = display; submit_input(input);
    }
    std::string cwd_;
    int cancellations = 0, before_calls = 0;
    std::vector<std::string> shells;
    std::vector<UserInput> inputs;
    std::function<void()> before;
};
struct FakeInputCommands final : ICommandContextFactory {
    CommandContext make(bool) override {
        throw std::logic_error("Command context is not installed in this fixture");
    }
};
struct FakeClipboard final : IClipboard {
    ClipboardTextReadResult read_text() override {
        ++text_reads; if (on_read_text) on_read_text(); return text;
    }
    ClipboardImageReadResult read_image() override { ++image_reads; return image; }
    ClipboardTextWriteResult write_text(const std::string& value) override {
        written.push_back(value); if (on_write_text) on_write_text(); return write;
    }
    void write_osc52(const std::string& value) override { osc52.push_back(value); }
    int text_reads = 0, image_reads = 0;
    ClipboardTextReadResult text{ClipboardTextReadResult::Status::Success, "clipboard text", {}};
    ClipboardImageReadResult image;
    ClipboardTextWriteResult write{ClipboardTextWriteResult::Status::Success, {}};
    std::vector<std::string> written, osc52;
    std::function<void()> on_read_text, on_write_text;
};
struct InputHarness {
    acecode_test::characterization::Isolation isolation;
    acecode_test::characterization::TemporaryDirectory workspace;
    const std::string cwd = path_to_utf8(workspace.path);
    TuiState state;
    FakeScreenPort screen;
    ChatViewport viewport;
    FrameGeometry geometry;
    CommandRegistry commands;
    FakeInputTurn turn{cwd};
    FakeInputCommands command_contexts;
    FakeClipboard clipboard;
    AppConfig config;
    PermissionManager permissions;
    SessionManager session;
    std::atomic<bool> auth_done{true};
    std::atomic<std::int64_t> last_key{0};
    TuiInputContext context{state, screen, viewport, geometry, commands, turn,
        command_contexts, clipboard, config, permissions, session, auth_done, cwd, last_key, {}};
    InputHarness() {
        config.input_history.enabled = false;
        viewport.chat_box = {0, 79, 0, 19};
    }
};
}
