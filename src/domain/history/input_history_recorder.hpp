#pragma once
#include <string>
#include <vector>
namespace acecode {
struct InputHistoryConfig;
// Records only nonblank, nonadjacent-duplicate input; disk writes are optional.
void record_input_history(std::vector<std::string>& history, const InputHistoryConfig& config,
    const std::string& project_dir, const std::string& entry);
}
