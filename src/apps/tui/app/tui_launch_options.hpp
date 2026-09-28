#pragma once
#include "cli/interactive_options.hpp"
#include <string>
namespace acecode::tui {
struct TuiLaunchOptions {
    InteractiveCliOptions cli;
    std::string argv0_dir;
};
}
