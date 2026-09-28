#include "tui/app/tui_app.hpp"
#include "tui/app/tui_services.hpp"
#include "tui/app/tui_screen_host.hpp"
#include "tui/app/tui_submitter.hpp"
#include "tui/app/tui_command_context_factory.hpp"
#include "tui/app/tui_clipboard.hpp"
#include "tui/app/tui_event_router.hpp"
#include "tui/app/full_screen_surfaces.hpp"
#include "tui/app/process_guards.hpp"
#include "tui/composer/input_component.hpp"
#include "tui/render/frame_renderer.hpp"
#include "tui/input/tui_input_context.hpp"
#include "tui/term/terminal_control.hpp"
#include "tui/paste_handler.hpp"
#include "utils/scope_exit.hpp"
#include <ftxui/component/component.hpp>
namespace acecode::tui {
void TuiApp::create_components() {
    auto composer = make_composer_input(state_, geometry_.input_hit_layout);
    clipboard_ = std::make_unique<TuiClipboard>();
    input_context_ = std::make_unique<TuiInputContext>(TuiInputContext{
        state_, *screen_host_, viewport_, geometry_, *commands_, *submitter_, *command_contexts_,
        *clipboard_, services_->config, *permissions_, session_manager_, auth_done_, environment_.working_dir,
        screen_host_->last_keyboard_input_at_ms(), bind(&TuiApp::respond_permission),
    });
    event_router_ = std::make_unique<TuiEventRouter>(*input_context_);
    input_component_ = event_router_->wrap(std::move(composer));
    frame_renderer_ = std::make_unique<TuiFrameRenderer>(state_, *screen_host_, version_str_,
        cwd_display_, viewport_, geometry_, anim_tick_, input_component_, *permissions_,
        options_.cli.dangerous_mode, conhost_compat_layout_, screen_host_->hover_supported());
    auto chat = ftxui::Renderer(input_component_, bind(&TuiApp::render_frame));
    auto surfaces = std::make_unique<FullScreenSurfaces>(state_, *screen_host_, services_->config,
        session_manager_, *agent_loop_, *subagent_host_, *services_->skills, *commands_, *services_->mcp,
        *services_->tools, *services_->hooks, services_->skill_usage.get(), environment_.working_dir,
        std::move(chat), input_component_, bind(&TuiApp::publish_configuration));
    root_ = surfaces->component();
    surfaces_ = std::move(surfaces);
}
ftxui::Element TuiApp::render_frame() {
    auto pacer = screen_host_->redraw_pacer();
    const auto ticket = pacer->begin_frame(monotonic_milliseconds());
    auto frame = frame_renderer_->render();
    screen_host_->post_task([pacer, ticket] {
        pacer->complete_frame(ticket, monotonic_milliseconds());
    });
    return frame;
}
void TuiApp::run_event_loop() {
    console_ctrl_->post_ftxui_setup(*screen_host_);
    flush_terminal_input_buffer();
    write_terminal_control_sequence(kBracketedPasteEnableSeq);
    ScopeExit restore([] {
        write_terminal_control_sequence(kBracketedPasteDisableSeq);
        flush_terminal_input_buffer();
    });
    screen_host_->screen().Loop(root_);
}
}
