#include "prompt_environment.hpp"

#include "terminal_runtime.hpp"
#include "toolchains.hpp"

namespace acecode::environment {

SystemPromptEnvironment prompt_environment() {
    SystemPromptEnvironment env;
    if (auto t = terminal().current()) {
        env.terminal_family = terminal_family_name(t->family);
        env.terminal_program = t->program;
    }
    env.toolchains = applied_toolchain_dirs();
    return env;
}

}  // namespace acecode::environment
