#pragma once
#include <string>
namespace acecode {
struct InteractiveCliOptions;
struct WorktreeSessionInfo;
class SessionManager;
}
namespace acecode::tui {
bool bootstrap_startup_worktree(const InteractiveCliOptions& cli, std::string& working_dir,
    WorktreeSessionInfo& info, std::string& banner);
// The current session is the sole source, including worktrees entered by tools.
void finalize_session_worktree_on_exit(SessionManager& session);
}
