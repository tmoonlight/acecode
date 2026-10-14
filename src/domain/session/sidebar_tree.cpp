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
    std::map<std::string, std::string> groups;
    std::map<std::string, std::string> legacy_groups;
    std::set<std::string> known_loops;
    if (!sessions.is_array()) return tree;
    for (const auto& session : sessions) {
        const auto id = field(session, "id");
        if (id.empty()) continue;
        const auto key = "session:" + id;
        records[key] = session;
        const auto loop = field(object_field(session, "loop_execution"), "loop_id");
        tree.nodes[key] = SidebarNode{key, id, !loop.empty(), false, loop, {}, {}};
        if (!loop.empty()) known_loops.insert(loop);
    }
    for (const auto& [key, value] : parents.items()) {
        if (!value.is_string()) continue;
        const auto parent = value.get<std::string>();
        if (parent.rfind("loop:", 0) == 0 && known_loops.count(parent.substr(5))) {
            groups[parent] = parent.substr(5);
        } else if (parent.rfind("session:", 0) == 0 || parent.rfind("group:", 0) == 0) {
            const auto anchor_key = parent.rfind("group:", 0) == 0 ? "session:" + parent.substr(6) : parent;
            const auto anchor = tree.nodes.find(anchor_key);
            if (anchor == tree.nodes.end() || !anchor->second.scheduled) continue;
            const auto group_key = "group:" + anchor->second.session_id;
            groups[group_key] = anchor->second.loop_id;
            if (parent == anchor_key) legacy_groups[anchor_key] = group_key;
        }
    }
    for (auto& [key, node] : tree.nodes) {
        // An explicit empty parent remains detached across future executions.
        if (legacy_groups.count(key)) node.parent = legacy_groups.at(key);
        else if (parents.contains(key)) {
            node.parent = field(parents, key.c_str());
            if (legacy_groups.count(node.parent)) node.parent = legacy_groups.at(node.parent);
            if (!groups.count(node.parent)) node.parent.clear();
        } else if (node.scheduled) {
            node.parent = "loop:" + node.loop_id;
            groups[node.parent] = node.loop_id;
        }
    }
    for (const auto& [key, loop] : groups) {
        tree.nodes[key] = SidebarNode{key, {}, true, true, loop, {}, {}};
    }
    for (const auto& [key, node] : tree.nodes) {
        if (!node.parent.empty()) tree.nodes.at(node.parent).children.push_back(key);
    }
    const auto newer = [&records](const std::string& a, const std::string& b, const char* clock) {
        const auto at = field(records.at(a), clock);
        const auto bt = field(records.at(b), clock);
        return at != bt ? at > bt : a > b;
    };
    for (auto it = tree.nodes.begin(); it != tree.nodes.end();) {
        auto& node = it->second;
        if (node.group) {
            if (node.children.empty()) { it = tree.nodes.erase(it); continue; }
            auto latest = node.children.front();
            for (const auto& child : node.children) {
                if (newer(child, latest, "created_at")) latest = child;
            }
            node.session_id = tree.nodes.at(latest).session_id;
            records[node.key] = records.at(latest);
        }
        if (node.parent.empty()) tree.roots.push_back(node.key);
        ++it;
    }
    const auto sort_keys = [&](std::vector<std::string>& keys, const std::string& parent, const char* clock) {
        std::unordered_map<std::string, std::size_t> ranks;
        auto it = orders.find(parent);
        if (it == orders.end()) {
            for (const auto& [old_key, group_key] : legacy_groups) {
                if (group_key == parent) { it = orders.find(old_key); break; }
            }
        }
        if (it != orders.end() && it->is_array()) {
            for (const auto& raw : *it) if (raw.is_string()) {
                auto key = raw.get<std::string>();
                if (parent.empty() && legacy_groups.count(key)) key = legacy_groups.at(key);
                ranks.emplace(key, ranks.size());
            }
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
    auto parent = placement == "inside" ? target : to.parent;
    if ((placement == "inside" && !to.parent.empty()) || parent == source ||
        (!parent.empty() && (from.group || !from.children.empty()))) {
        error = "SIDEBAR_TWO_LEVELS_ONLY";
        return false;
    }
    if (!parent.empty() && !tree.nodes.at(parent).scheduled) {
        error = "SIDEBAR_SCHEDULED_PARENT_ONLY";
        return false;
    }
    if (placement == "inside" && !to.group) {
        // A detached scheduled session becomes a group header plus its own
        // real session row. The stable group key also migrates legacy layouts.
        parent = "group:" + to.session_id;
        if (!tree.nodes.count(parent)) {
            tree.nodes[parent] = SidebarNode{parent, to.session_id, true, true, to.loop_id, {}, {}};
            std::replace(tree.roots.begin(), tree.roots.end(), target, parent);
        } else erase_key(tree.roots, target);
        tree.nodes.at(target).parent = parent;
        tree.nodes.at(parent).children.push_back(target);
    }
    auto& old_siblings = from.parent.empty() ? tree.roots : tree.nodes.at(from.parent).children;
    erase_key(old_siblings, source);
    auto& siblings = parent.empty() ? tree.roots : tree.nodes.at(parent).children;
    auto position = placement == "inside" ? siblings.end() : std::find(siblings.begin(), siblings.end(), target);
    if (position != siblings.end() && placement == "after") ++position;
    siblings.insert(position, source);
    from.parent = parent;
    if (!layout.is_object()) layout = json::object();
    if (!layout.contains("parents") || !layout["parents"].is_object()) layout["parents"] = json::object();
    if (!layout.contains("orders") || !layout["orders"].is_object()) layout["orders"] = json::object();
    // Archived members may be absent from this tree but still carry a legacy
    // parent key. Normalize those too, or they would reattach a detached anchor.
    for (auto& entry : layout["parents"].items()) {
        if (!entry.value().is_string()) continue;
        const auto old_parent = entry.value().get<std::string>();
        if (old_parent.rfind("session:", 0) != 0) continue;
        const auto group_key = "group:" + old_parent.substr(8);
        if (tree.nodes.count(group_key) && tree.nodes.at(group_key).group) entry.value() = group_key;
    }
    layout["version"] = 2;
    layout["orders"][""] = tree.roots;
    for (const auto& [key, node] : tree.nodes) {
        if (node.group) layout["orders"][key] = node.children;
        else layout["parents"][key] = node.parent;
    }
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
