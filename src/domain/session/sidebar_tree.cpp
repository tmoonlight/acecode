#include "sidebar_tree.hpp"

#include "utils/atomic_file.hpp"
#include "utils/paths.hpp"

#include <algorithm>
#include <fstream>
#include <set>
#include <unordered_map>

namespace acecode {
namespace {
using nlohmann::json;

std::string field(const json& object, const char* name) {
    auto it = object.find(name);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

const json& object_field(const json& object, const char* name) {
    static const json empty = json::object();
    auto it = object.find(name);
    return it != object.end() && it->is_object() ? *it : empty;
}

void erase_key(std::vector<std::string>& keys, const std::string& key) {
    keys.erase(std::remove(keys.begin(), keys.end(), key), keys.end());
}

} // namespace

SidebarTree build_sidebar_tree(const json& sessions, const json& layout) {
    SidebarTree tree;
    const auto& parents = object_field(layout, "parents");
    const auto& orders = object_field(layout, "orders");
    std::map<std::string, json> records;
    std::map<std::string, std::vector<std::string>> groups;
    if (!sessions.is_array()) return tree;
    for (const auto& session : sessions) {
        const auto id = field(session, "id");
        if (id.empty()) continue;
        const auto key = "session:" + id;
        records[key] = session;
        const auto loop = field(object_field(session, "loop_execution"), "loop_id");
        tree.nodes[key] = SidebarNode{key, id, !loop.empty(), {}, {}};
        // An explicit empty parent means detached, including across future runs.
        if (!loop.empty() && !parents.contains(key)) groups["loop:" + loop].push_back(key);
    }
    const auto newer = [&records](const std::string& a, const std::string& b, const char* clock) {
        const auto at = field(records.at(a), clock);
        const auto bt = field(records.at(b), clock);
        return at != bt ? at > bt : a > b;
    };
    for (auto& [key, members] : groups) {
        std::sort(members.begin(), members.end(), [&](const auto& a, const auto& b) {
            return newer(a, b, "created_at");
        });
        const auto head = members.front();
        auto node = tree.nodes.at(head);
        tree.nodes.erase(head);
        node.key = key;
        records[key] = records.at(head);
        tree.nodes[key] = std::move(node);
        for (std::size_t i = 1; i < members.size(); ++i) tree.nodes.at(members[i]).parent = key;
    }
    for (auto& [key, node] : tree.nodes) {
        const auto it = parents.find(key);
        if (it != parents.end() && it->is_string()) node.parent = it->get<std::string>();
        const auto parent = tree.nodes.find(node.parent);
        if (node.parent == key || parent == tree.nodes.end() || !parent->second.scheduled) node.parent.clear();
        // Scheduled group keys remain roots, even before their next run arrives.
        if (key.rfind("loop:", 0) == 0) node.parent.clear();
    }
    // Damaged/stale layouts cannot introduce cycles or a third level. A node
    // with dependents stays at the root; missing parents promote their children.
    std::set<std::string> has_children;
    for (const auto& [key, node] : tree.nodes) {
        if (!node.parent.empty()) has_children.insert(node.parent);
    }
    for (auto& [key, node] : tree.nodes) {
        if (has_children.count(key)) node.parent.clear();
    }
    for (auto& [key, node] : tree.nodes) {
        if (!node.parent.empty() && !tree.nodes.at(node.parent).parent.empty()) node.parent.clear();
        if (node.parent.empty()) tree.roots.push_back(key);
        else tree.nodes.at(node.parent).children.push_back(key);
    }
    const auto sort_keys = [&](std::vector<std::string>& keys, const std::string& parent, const char* clock) {
        std::unordered_map<std::string, std::size_t> ranks;
        const auto it = orders.find(parent);
        if (it != orders.end() && it->is_array()) {
            for (const auto& raw : *it) if (raw.is_string()) ranks.emplace(raw.get<std::string>(), ranks.size());
        }
        std::sort(keys.begin(), keys.end(), [&](const auto& a, const auto& b) {
            const auto ar = ranks.find(a), br = ranks.find(b);
            if (ar != ranks.end() && br != ranks.end()) return ar->second < br->second;
            if ((ar == ranks.end()) != (br == ranks.end())) return ar == ranks.end();
            return newer(a, b, clock);
        });
    };
    sort_keys(tree.roots, "", "updated_at");
    for (auto& [key, node] : tree.nodes) sort_keys(node.children, key, "created_at");
    return tree;
}

bool move_sidebar_node(SidebarTree& tree, json& layout,
                       const std::string& source, const std::string& target,
                       const std::string& placement, std::string& error) {
    error.clear();
    if (source == target || !tree.nodes.count(source) || !tree.nodes.count(target)) {
        error = "SIDEBAR_TARGET_CHANGED";
        return false;
    }
    if (placement != "inside" && placement != "before" && placement != "after") {
        error = "SIDEBAR_INVALID_PLACEMENT";
        return false;
    }
    auto& from = tree.nodes.at(source);
    const auto& to = tree.nodes.at(target);
    const auto parent = placement == "inside" ? target : to.parent;
    if ((placement == "inside" && !to.parent.empty()) || parent == source ||
        (!parent.empty() && !from.children.empty())) {
        error = "SIDEBAR_TWO_LEVELS_ONLY";
        return false;
    }
    if (!parent.empty() && !tree.nodes.at(parent).scheduled) {
        error = "SIDEBAR_SCHEDULED_PARENT_ONLY";
        return false;
    }
    auto& old_siblings = from.parent.empty() ? tree.roots : tree.nodes.at(from.parent).children;
    const auto old_parent = from.parent;
    // A one-run automatic group is still a leaf. Moving it into another root
    // detaches that execution, leaving future runs free to form their own group.
    const auto moving_key = !parent.empty() && source.rfind("loop:", 0) == 0
        ? "session:" + from.session_id : source;
    auto moving_node = from;
    erase_key(old_siblings, source);
    auto& siblings = parent.empty() ? tree.roots : tree.nodes.at(parent).children;
    auto position = placement == "inside" ? siblings.end() : std::find(siblings.begin(), siblings.end(), target);
    if (position != siblings.end() && placement == "after") ++position;
    siblings.insert(position, moving_key);
    if (moving_key != source) {
        tree.nodes.erase(source);
        moving_node.key = moving_key;
        moving_node.parent = parent;
        tree.nodes[moving_key] = std::move(moving_node);
    } else {
        from.parent = parent;
    }
    if (!layout.is_object()) layout = json::object();
    if (!layout.contains("parents") || !layout["parents"].is_object()) layout["parents"] = json::object();
    if (!layout.contains("orders") || !layout["orders"].is_object()) layout["orders"] = json::object();
    layout["version"] = 1;
    layout["parents"][moving_key] = parent;
    layout["orders"][old_parent] = old_siblings;
    layout["orders"][parent] = siblings;
    return true;
}

json read_sidebar_layout(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return json::object();
    auto result = json::parse(input, nullptr, false);
    return result.is_object() ? result : json::object();
}

bool write_sidebar_layout(const std::filesystem::path& path, const json& layout, std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) { error = ec.message(); return false; }
    if (atomic_write_file(path_to_utf8(path), layout.dump(2) + "\n", false, true)) return true;
    error = "failed to atomically save sidebar layout";
    return false;
}

std::vector<SessionMeta> SidebarMetadataCache::read(const std::vector<std::string>& directories) {
    std::vector<SessionMeta> result;
    for (const auto& directory : directories) {
        auto& cache = directories_[directory];
        std::set<std::string> seen;
        std::error_code ec;
        auto iterator = std::filesystem::directory_iterator(path_from_utf8(directory), ec);
        if (ec) continue;
        for (const auto& file : iterator) {
            const auto name = path_to_utf8(file.path().filename());
            if (name.size() < 10 || name.compare(name.size() - 10, 10, ".meta.json") != 0) continue;
            const auto modified = file.last_write_time(ec);
            if (ec) { ec.clear(); continue; }
            const auto size = file.file_size(ec);
            if (ec) { ec.clear(); continue; }
            seen.insert(name);
            auto found = cache.find(name);
            if (found == cache.end() || found->second.modified != modified || found->second.size != size) {
                auto meta = SessionStorage::read_meta(path_to_utf8(file.path()));
                cache[name] = Entry{modified, size, std::move(meta)};
            }
            if (!cache.at(name).meta.id.empty()) result.push_back(cache.at(name).meta);
        }
        for (auto it = cache.begin(); it != cache.end();) {
            if (!seen.count(it->first)) it = cache.erase(it);
            else ++it;
        }
    }
    return result;
}

} // namespace acecode
