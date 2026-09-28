#pragma once
#include "tui/app/interactive_options.hpp"
#include <string>
namespace acecode::tui {
struct TuiLaunchOptions {
    InteractiveCliOptions cli;
    std::string argv0_dir;
};
}
