#pragma once
#include "tui/input/ports.hpp"
#include "tui/commands/command_registry.hpp"
#include "agent/agent_loop.hpp"
#include <utility>

namespace acecode::tui {
// Transitional assembly binding: callbacks are the existing main-owned pipeline.
// B-11 replaces this adapter with TuiSubmitter and the shared command factory.
class TuiInputTurnBinding final : public ITurnSubmitter {
public:
    TuiInputTurnBinding(AgentLoop& agent, std::function<void(const UserInput&)> submit,
        std::function<void()> before_first)
        : agent_(agent), submit_(std::move(submit)), before_first_(std::move(before_first)) {}
    std::string cwd() const override { return agent_.cwd(); }
    void cancel() override { agent_.cancel(); }
    void submit_shell(const std::string& command) override { agent_.submit_shell(command); }
    void before_first_turn() override { before_first_(); }
    void submit_input(const UserInput& input) override { submit_(input); }
    void submit_text(const std::string& text, const std::string& display_text = {}) override {
        UserInput input; input.text = text; input.display_text = display_text; submit_(input);
    }
private:
    AgentLoop& agent_;
    std::function<void(const UserInput&)> submit_;
    std::function<void()> before_first_;
};
class TuiInputCommandBinding final : public ICommandContextFactory {
public:
    explicit TuiInputCommandBinding(std::function<CommandContext(bool)> make) : make_(std::move(make)) {}
    CommandContext make(bool track_usage) override { return make_(track_usage); }
private:
    std::function<CommandContext(bool)> make_;
};
}
