#include "session_tree_purge.hpp"

#include "session_storage.hpp"
#include "session_user_message_search.hpp"

#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace acecode {

std::vector<std::string> session_tree_delete_order(const std::string& project_dir,
                                                   const std::string& root_id) {
    std::unordered_multimap<std::string, std::string> children;
    for (const auto& meta : SessionStorage::list_session_metadata(project_dir)) {
        if (!meta.parent_session_id.empty()) {
            children.emplace(meta.parent_session_id, meta.id);
        }
    }
    std::vector<std::string> order;
    std::unordered_set<std::string> visited;
    std::function<void(const std::string&)> collect = [&](const std::string& id) {
        if (!visited.insert(id).second) return;
        const auto range = children.equal_range(id);
        for (auto it = range.first; it != range.second; ++it) collect(it->second);
        order.push_back(id);
    };
    collect(root_id);
    return order;
}

bool purge_session_tree(const std::string& project_dir,
                        const std::string& root_id,
                        std::string* error) {
    if (error) error->clear();
    SessionUserMessageIndex search_index(project_dir);
    for (const auto& id : session_tree_delete_order(project_dir, root_id)) {
        std::string step_error;
        if (!search_index.remove_session(id, &step_error) ||
            !SessionStorage::purge_session_files(project_dir, id, &step_error)) {
            if (error) *error = id + ": " + step_error;
            return false;
        }
    }
    return true;
}

}  // namespace acecode
