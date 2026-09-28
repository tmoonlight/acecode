#include "input_history_recorder.hpp"
#include "input_history_store.hpp"
#include "config/config.hpp"
#include "session/session_storage.hpp"
#include <cctype>

namespace acecode {

void record_input_history(std::vector<std::string>& history, const InputHistoryConfig& config,
    const std::string& working_dir, const std::string& entry) {
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
            SessionStorage::get_project_dir(working_dir));
        InputHistoryStore::append(path, entry, config.max_entries);
    }
}

} // namespace acecode
