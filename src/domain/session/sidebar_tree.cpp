#include "sidebar_tree.hpp"

#include "utils/atomic_file.hpp"
#include "utils/paths.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <queue>
#include <set>
#include <string_view>
#include <unordered_map>
#include <utility>

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

bool move_sidebar_nodes(SidebarTree& tree, json& layout,
                        const std::vector<std::string>& sources, const std::string& target,
                        const std::string& placement, std::string& error) {
    error.clear();
    const std::set<std::string> selected(sources.begin(), sources.end());
    if (sources.empty() || selected.size() != sources.size() || selected.count(target)) {
        error = "SIDEBAR_TARGET_CHANGED";
        return false;
    }
    for (const auto& source : sources) {
        const auto found = tree.nodes.find(source);
        if (found == tree.nodes.end() || selected.count(found->second.parent)) {
            error = "SIDEBAR_TARGET_CHANGED";
            return false;
        }
    }
    auto next_tree = tree;
    auto next_layout = layout;
    auto ordered = sources;
    if (placement == "after") std::reverse(ordered.begin(), ordered.end());
    auto receiver = target;
    for (const auto& source : ordered) {
        if (!move_sidebar_node(next_tree, next_layout, source, receiver, placement, error)) return false;
        // The first insertion can turn a detached scheduled session into a
        // manual group. Remaining insertions must address that new group.
        if (placement == "inside") receiver = next_tree.nodes.at(source).parent;
    }
    tree = std::move(next_tree);
    layout = std::move(next_layout);
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

const SidebarMetadataCache::Directory* SidebarMetadataCache::refresh(const std::string& directory) {
    auto& cache = directories_[directory];
    std::set<std::string> seen;
    std::error_code ec;
    auto iterator = std::filesystem::directory_iterator(path_from_utf8(directory), ec);
    if (ec) return nullptr;
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
    }
    for (auto it = cache.begin(); it != cache.end();) {
        if (!seen.count(it->first)) it = cache.erase(it);
        else ++it;
    }
    return &cache;
}

std::vector<SessionMeta> SidebarMetadataCache::read(const std::vector<std::string>& directories) {
    std::vector<SessionMeta> result;
    for (const auto& directory : directories) {
        const auto* cache = refresh(directory);
        if (!cache) continue;
        for (const auto& [name, entry] : *cache) {
            if (!entry.meta.id.empty()) result.push_back(entry.meta);
        }
    }
    return result;
}

namespace {

// 文件修改时间换成与 updated_at 同格式(UTC、精确到秒)的字符串,好直接比较。
// 多算 2 秒,容忍文件系统时间戳的粒度和取整。
std::string utc_stamp_upper_bound(std::filesystem::file_time_type modified) {
    const auto seconds = static_cast<std::time_t>(
        SessionStorage::file_time_ticks_to_unix_ms(modified.time_since_epoch().count()) / 1000 + 2);
    std::tm tm_buf{};
#ifdef _WIN32
    gmtime_s(&tm_buf, &seconds);
#else
    gmtime_r(&seconds, &tm_buf);
#endif
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                  tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                  tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
    return buffer;
}

// 一次「最近会话」查询的入选状态。候选只记指针(指向缓存条目和 scopes,本次调用
// 内有效):目录多、会话上千时,整条复制比挑出最新几条贵得多。
struct RecentSelection {
    struct Candidate {
        const std::string* stamp;
        const SessionMeta* meta;
        const std::string* workspace_hash;
    };
    std::size_t limit;
    const std::function<bool(const SessionMeta&, const std::string&)>& accept;
    std::vector<Candidate> candidates;
    std::set<std::pair<std::string_view, std::string_view>> seen;
    // 目前最新的 limit 个时间戳,最旧的在堆顶。
    std::priority_queue<std::string_view, std::vector<std::string_view>,
                        std::greater<std::string_view>> newest;

    void consider(const SessionMeta& meta, const SessionDirectoryScope& scope) {
        static const std::string no_workspace_hash;
        if (meta.id.empty() || meta.archived || !meta.parent_session_id.empty()) return;
        if (scope.no_workspace_only && !meta.no_workspace) return;
        const auto& hash = meta.no_workspace ? no_workspace_hash : scope.workspace_hash;
        if (accept && !accept(meta, hash)) return;
        if (!seen.emplace(hash, meta.id).second) return;
        const auto& stamp = meta.updated_at.empty() ? meta.created_at : meta.updated_at;
        candidates.push_back({&stamp, &meta, &hash});
        newest.push(stamp);
        if (newest.size() > limit) newest.pop();
    }

    // 修改时间为 modified 的文件,还有没有可能排进前 limit 条。
    bool may_rank(std::filesystem::file_time_type modified) const {
        return newest.size() < limit || utc_stamp_upper_bound(modified) >= newest.top();
    }
};

} // namespace

std::vector<RecentSession> SidebarMetadataCache::recent(
    const std::vector<SessionDirectoryScope>& scopes, std::size_t limit,
    const std::function<bool(const SessionMeta&, const std::string&)>& accept) {
    if (limit == 0) return {};
    // 还没读过(或读过之后变了)的 meta 文件。先不打开,见下面的补读。
    struct Unread {
        std::filesystem::file_time_type modified;
        std::uintmax_t size = 0;
        std::filesystem::path path;
        std::string name;
        Directory* directory = nullptr;
        const SessionDirectoryScope* scope = nullptr;
    };
    RecentSelection selection{limit, accept};
    std::vector<Unread> unread;
    std::set<std::string_view> visited;
    for (const auto& scope : scopes) {
        // 每个目录只处理一次:候选记的是指向缓存条目的指针,同一目录再处理一遍
        // 可能删掉条目。新条目插入 std::map 不会挪动已有节点。
        if (!visited.insert(scope.project_dir).second) continue;
        auto& cache = directories_[scope.project_dir];
        std::set<std::string> present;
        std::error_code ec;
        auto iterator = std::filesystem::directory_iterator(path_from_utf8(scope.project_dir), ec);
        if (ec) continue;
        for (const auto& file : iterator) {
            auto name = path_to_utf8(file.path().filename());
            if (name.size() < 10 || name.compare(name.size() - 10, 10, ".meta.json") != 0) continue;
            const auto modified = file.last_write_time(ec);
            if (ec) { ec.clear(); continue; }
            const auto size = file.file_size(ec);
            if (ec) { ec.clear(); continue; }
            present.insert(name);
            const auto found = cache.find(name);
            if (found != cache.end() && found->second.modified == modified && found->second.size == size) {
                selection.consider(found->second.meta, scope);
                continue;
            }
            if (found != cache.end()) cache.erase(found);
            unread.push_back({modified, size, file.path(), std::move(name), &cache, &scope});
        }
        for (auto it = cache.begin(); it != cache.end();) {
            if (!present.count(it->first)) it = cache.erase(it);
            else ++it;
        }
    }
    // 补读:按文件修改时间从新到旧,读到剩下的文件不可能再排进前 limit 条为止。
    // 依据是 meta 的修改时间不会早于它里面记的 updated_at(写入 updated_at 的那一刻
    // 文件就被改了),所以修改时间比当前第 limit 名还旧的文件,内容只会更旧。
    // 这样冷启动时不必把几千个旧会话的 meta 全部解析一遍;没读的条目留给 read()。
    std::sort(unread.begin(), unread.end(), [](const Unread& a, const Unread& b) {
        return a.modified > b.modified;
    });
    for (auto& file : unread) {
        if (!selection.may_rank(file.modified)) break;
        auto meta = SessionStorage::read_meta(path_to_utf8(file.path));
        const auto inserted = file.directory->insert_or_assign(
            std::move(file.name), Entry{file.modified, file.size, std::move(meta)});
        selection.consider(inserted.first->second.meta, *file.scope);
    }

    auto& candidates = selection.candidates;
    const auto keep = std::min(limit, candidates.size());
    std::partial_sort(candidates.begin(), candidates.begin() + static_cast<std::ptrdiff_t>(keep),
                      candidates.end(),
                      [](const RecentSelection::Candidate& a, const RecentSelection::Candidate& b) {
                          if (*a.stamp != *b.stamp) return *a.stamp > *b.stamp;
                          return a.meta->id > b.meta->id;
                      });
    std::vector<RecentSession> result;
    result.reserve(keep);
    for (std::size_t index = 0; index < keep; ++index) {
        result.push_back({*candidates[index].meta, *candidates[index].workspace_hash});
    }
    return result;
}

} // namespace acecode
