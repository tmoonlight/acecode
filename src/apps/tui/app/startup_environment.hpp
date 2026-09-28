#pragma once
#include <string>
namespace acecode { struct InteractiveCliOptions; struct WorktreeSessionInfo; }
namespace acecode::tui {
bool initialize_tui_startup_environment(std::string& working_dir,
    const InteractiveCliOptions& cli, WorktreeSessionInfo& startup_worktree,
    std::string& startup_worktree_banner);
}
