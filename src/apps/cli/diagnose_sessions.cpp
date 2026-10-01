#include "diagnose_sessions.hpp"
#include "session/session_data_diagnostics.hpp"
#include "utils/paths.hpp"
#include "utils/utf8_path.hpp"

#include <csignal>
#include <filesystem>
#include <ostream>

namespace acecode::cli {
namespace {
volatile std::sig_atomic_t cancelled = 0;
void interrupt_diagnostics(int) { cancelled = 1; }

struct SignalScope {
    using Handler = void (*)(int);
    Handler previous = std::signal(SIGINT, interrupt_diagnostics);
    ~SignalScope() { std::signal(SIGINT, previous); }
};
}

int diagnose_sessions(std::ostream& output) {
    cancelled = 0;
    SignalScope signal;
    const auto projects = path_to_utf8(path_from_utf8(get_acecode_dir()) / "projects");
    desktop::WorkspaceRegistry registry;
    registry.scan(projects);
    const auto report = diagnose_session_data(projects, registry.list(), [] { return cancelled != 0; });
    output << report.dump(2) << '\n';
    return report.value("cancelled", false) ? 130 : 0;
}

} // namespace acecode::cli
