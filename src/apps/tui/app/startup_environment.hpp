#pragma once
#include <string>
#include "session/session_storage.hpp"
namespace acecode { struct InteractiveCliOptions; struct WorktreeSessionInfo; }
namespace acecode::tui {
struct StartupEnvironment {
    std::string working_dir;
    WorktreeSessionInfo worktree;
    std::string worktree_banner;
};
bool initialize_tui_startup_environment(std::string& working_dir,
    const InteractiveCliOptions& cli, WorktreeSessionInfo& startup_worktree,
    std::string& startup_worktree_banner);
}
