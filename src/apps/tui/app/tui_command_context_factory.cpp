#include "tui/app/tui_command_context_factory.hpp"
#include "tui/slash_command_usage.hpp"
#include <algorithm>
#include <limits>
namespace acecode::tui {
TuiCommandContextFactory::TuiCommandContextFactory(TuiState& state, AgentLoop& agent,
    SessionModelBinding& binding, AppConfig& config, TokenTracker& tracker,
    PermissionManager& permissions, IScreenPort& screen, SessionManager& session,
    McpManager& mcp, ToolExecutor& tools, SkillRegistry& skills, MemoryRegistry& memory,
    CommandRegistry& commands, const std::string& cwd, ITurnSubmitter& submitter,
    SubagentHost* subagents, const std::unique_ptr<IFullScreenSurfaces>& surfaces, std::function<void()> publish_config)
    : state_(state), agent_(agent), binding_(binding), config_(config), tracker_(tracker),
      permissions_(permissions), screen_(screen), session_(session), mcp_(mcp), tools_(tools),
      skills_(skills), memory_(memory), commands_(commands), cwd_(cwd), submitter_(submitter),
      subagents_(subagents), surfaces_(surfaces), publish_config_(std::move(publish_config)) {}
void TuiCommandContextFactory::record_usage(const std::string& name) {
    const auto write_result = record_tui_slash_command_use(name);
    std::lock_guard<std::mutex> usage_lock(state_.mu);
    auto& cached = state_.slash_command_usage_counts[name];
    if (cached < (std::numeric_limits<std::uint64_t>::max)()) ++cached;
    cached = std::max(cached, write_result.count);
}
CommandContext TuiCommandContextFactory::make(bool track_command_usage) {
    const auto ref = lifetime_.ref(*this);
    CommandContext context{state_, agent_, &binding_, config_, tracker_, permissions_};
    context.request_exit = [ref] {
        ref.with([](TuiCommandContextFactory& owner) { owner.screen_.exit(); });
    };
    context.on_command_completed = publish_config_;
    context.session_manager = &session_;
    context.post_event = [ref] {
        ref.with([](TuiCommandContextFactory& owner) { owner.screen_.post_event(ftxui::Event::Custom); });
    };
    context.mcp_manager = &mcp_;
    context.tools = &tools_;
    context.skills = &skills_;
    context.memory = &memory_;
    context.command_registry = &commands_;
    context.cwd = cwd_;
    context.subagent_host = subagents_;
    context.submit_user_input = [ref](const UserInput& input) {
        ref.with([&](TuiCommandContextFactory& owner) { owner.submitter_.submit_input(input); });
    };
    if (track_command_usage) {
        context.on_command_recognized = [ref](const std::string& name) {
            ref.with([&](TuiCommandContextFactory& owner) { owner.record_usage(name); });
        };
        if (surfaces_) context.open_settings_surface = [ref](const std::string& tab, std::string& error) {
            bool opened = false;
            ref.with([&](TuiCommandContextFactory& owner) { opened = owner.surfaces_->open_settings(tab, error); });
            return opened;
        };
        if (surfaces_) context.open_management_surface = [ref](const std::string& tab, std::string& error) {
            bool opened = false;
            ref.with([&](TuiCommandContextFactory& owner) { opened = owner.surfaces_->open_management(tab, error); });
            return opened;
        };
    }
    return context;
}
}
