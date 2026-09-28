#include "input_history_recorder.hpp"
#include "input_history_store.hpp"
#include "config/config.hpp"
#include <cctype>

namespace acecode {

void record_input_history(std::vector<std::string>& history, const InputHistoryConfig& config,
    const std::string& project_dir, const std::string& entry) {
    auto is_all_space = [](const std::string& s) {
        for (unsigned char c : s) {
            if (!std::isspace(c)) return false;
        }
        return true;
    };
    if (entry.empty() || is_all_space(entry)) return;
    if (!history.empty() && history.back() == entry) return;
    history.push_back(entry);
    if (config.enabled) {
        std::string path = InputHistoryStore::file_path(
            project_dir);
        InputHistoryStore::append(path, entry, config.max_entries);
    }
}

} // namespace acecode
