#pragma once
#include <optional>
#include <string>
namespace acecode { struct InteractiveCliOptions; }
namespace acecode::cli {
int validate_models_registry_command(const std::string& argv0_dir);
std::optional<int> run_pre_tui_command(const InteractiveCliOptions& cli,
    const std::string& argv0_dir);
}
