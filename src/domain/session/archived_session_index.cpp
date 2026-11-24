#include "archived_session_index.hpp"

#include "session_load_metrics.hpp"
#include "utils/logger.hpp"
#include "utils/state_file.hpp"
#include "utils/utf8_path.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

namespace acecode {
namespace {

namespace fs = std::filesystem;
using nlohmann::json;

constexpr int kIndexVersion = 1;
constexpr const char* kIndexFileName = "archived-sessions.json";
constexpr std::string_view kMetaSuffix = ".meta.json";

struct IndexEntry {
    std::string project; // projects/ 下的目录名
    std::string id;      // meta 文件名去掉 .meta.json
    bool no_workspace = false;
};

// 目录名与会话 id 都要拼回路径,只接受单段名字,挡住损坏或被改过的索引内容。
bool plain_name(const std::string& name) {
    if (name.empty() || name == "." || name == "..") return false;
    return name.find_first_of("/\\:") == std::string::npos;
}

fs::path without_trailing_separator(fs::path path) {
    if (!path.has_filename() && path.has_parent_path()) path = path.parent_path();
    return path;
}

bool is_projects_root(const fs::path& root) {
    return !root.empty() && root.filename() == "projects";
}

struct ProjectLocation {
    fs::path projects_root;
    std::string key;
};

std::optional<ProjectLocation> locate_project_dir(const std::string& project_dir) {
    if (project_dir.empty()) return std::nullopt;
    const auto dir = without_trailing_separator(path_from_utf8(project_dir));
    const auto root = dir.parent_path();
    const auto key = path_to_utf8(dir.filename());
    if (!is_projects_root(root) || !plain_name(key)) return std::nullopt;
    return ProjectLocation{root, key};
}

std::string index_path_for_root(const fs::path& projects_root) {
    return path_to_utf8(projects_root.parent_path() / kIndexFileName);
}

std::optional<std::string> session_stem(const std::string& meta_path) {
    const auto name = path_to_utf8(path_from_utf8(meta_path).filename());
    if (name.size() <= kMetaSuffix.size()) return std::nullopt;
    if (std::string_view(name).substr(name.size() - kMetaSuffix.size()) != kMetaSuffix) {
        return std::nullopt;
    }
    auto stem = name.substr(0, name.size() - kMetaSuffix.size());
    if (!plain_name(stem)) return std::nullopt;
    return stem;
}

std::string string_field(const json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

// 规整成当前版本的结构。版本不认识或结构不对一律当空索引,下次列出时重建。
json normalized(json state) {
    const auto version = state.is_object() ? state.find("version") : state.end();
    if (!state.is_object() || version == state.end() || !version->is_number_integer()
        || version->get<int>() != kIndexVersion) {
        state = json::object();
    }
    json result = {{"version", kIndexVersion},
                   {"scanned_projects", json::object()},
                   {"no_workspace_scanned", false},
                   {"sessions", json::array()}};
    if (const auto it = state.find("scanned_projects"); it != state.end() && it->is_object()) {
        for (const auto& [key, value] : it->items()) {
            if (plain_name(key) && value.is_boolean() && value.get<bool>()) {
                result["scanned_projects"][key] = true;
            }
        }
    }
    if (const auto it = state.find("no_workspace_scanned"); it != state.end() && it->is_boolean()) {
        result["no_workspace_scanned"] = it->get<bool>();
    }
    if (const auto it = state.find("sessions"); it != state.end() && it->is_array()) {
        for (const auto& item : *it) {
            if (!item.is_object()) continue;
            const auto project = string_field(item, "project");
            const auto id = string_field(item, "id");
            if (!plain_name(project) || !plain_name(id)) continue;
            const auto no_workspace = item.find("no_workspace");
            result["sessions"].push_back({
                {"project", project},
                {"id", id},
                {"no_workspace", no_workspace != item.end() && no_workspace->is_boolean()
                                     && no_workspace->get<bool>()},
            });
        }
    }
    return result;
}

json read_index(const std::string& index_path) {
    return normalized(read_state_json_at(index_path));
}

std::vector<IndexEntry> entries_of(const json& state) {
    std::vector<IndexEntry> entries;
    for (const auto& item : state["sessions"]) {
        entries.push_back({item["project"].get<std::string>(), item["id"].get<std::string>(),
                           item["no_workspace"].get<bool>()});
    }
    return entries;
}

bool contains_entry(const json& state, const IndexEntry& entry) {
    for (const auto& item : state["sessions"]) {
        if (item["project"] == entry.project && item["id"] == entry.id) return true;
    }
    return false;
}

void add_entry(json& state, const IndexEntry& entry) {
    if (contains_entry(state, entry)) return;
    state["sessions"].push_back(
        {{"project", entry.project}, {"id", entry.id}, {"no_workspace", entry.no_workspace}});
}

bool remove_entry(json& state, const IndexEntry& entry) {
    auto& sessions = state["sessions"];
    for (auto it = sessions.begin(); it != sessions.end(); ++it) {
        if ((*it)["project"] == entry.project && (*it)["id"] == entry.id) {
            sessions.erase(it);
            return true;
        }
    }
    return false;
}

bool project_scanned(const json& state, const std::string& key) {
    return state["scanned_projects"].contains(key);
}

// 每次写索引时顺手清掉项目目录已经不在的条目与扫描标记(删掉的工作区、跑完就删
// 的测试目录),否则只在列出那个目录时才会检查到它们,索引会越积越多。
// 目录状态拿不准(权限、IO 错误)时保留。返回是否删掉了东西。
bool drop_missing_projects(json& state, const fs::path& projects_root) {
    std::map<std::string, bool> present;
    const auto exists = [&present, &projects_root](const std::string& key) {
        const auto [it, inserted] = present.emplace(key, true);
        if (inserted) {
            std::error_code ec;
            it->second = fs::exists(projects_root / path_from_utf8(key), ec) || ec;
        }
        return it->second;
    };
    bool changed = false;
    json sessions = json::array();
    for (auto& item : state["sessions"]) {
        if (exists(item["project"].get<std::string>())) sessions.push_back(std::move(item));
        else changed = true;
    }
    json scanned = json::object();
    for (const auto& [key, value] : state["scanned_projects"].items()) {
        if (exists(key)) scanned[key] = value;
        else changed = true;
    }
    state["sessions"] = std::move(sessions);
    state["scanned_projects"] = std::move(scanned);
    return changed;
}

enum class EntryState { Archived, Gone, Unknown };

// Gone:meta 已删除,或读出来已不是归档状态;Unknown:文件在但读不出来(比如
// 正被别的进程替换),这次不列也不剔除,下次再看。
EntryState inspect_entry(const fs::path& projects_root, const IndexEntry& entry, SessionMeta* out) {
    const auto meta_path = SessionStorage::meta_path(
        path_to_utf8(projects_root / path_from_utf8(entry.project)), entry.id);
    auto meta = SessionStorage::read_meta(meta_path);
    if (!meta.id.empty()) {
        if (!meta.archived) return EntryState::Gone;
        if (out) *out = std::move(meta);
        return EntryState::Archived;
    }
    std::error_code ec;
    const bool exists = fs::exists(path_from_utf8(meta_path), ec);
    return !exists && !ec ? EntryState::Gone : EntryState::Unknown;
}

void prune(const std::string& index_path, const fs::path& projects_root,
           const std::vector<IndexEntry>& gone) {
    // 在文件锁里再确认一次。同一会话刚被重新归档时:它先落盘的话这里读到
    // archived=true 而保留;后落盘的话它的登记排在这次剔除之后,会重新加回来。
    const auto result = update_state_json_at(index_path, [&gone, &projects_root](json& state) {
        state = normalized(std::move(state));
        bool changed = drop_missing_projects(state, projects_root);
        for (const auto& entry : gone) {
            if (inspect_entry(projects_root, entry, nullptr) != EntryState::Gone) continue;
            changed = remove_entry(state, entry) || changed;
        }
        return changed;
    });
    if (!result.persisted) LOG_WARN("[archived-index] failed to prune " + index_path);
}

template <typename Wanted>
std::vector<SessionMeta> read_indexed(const json& state, const fs::path& projects_root,
                                      const std::string& index_path, const Wanted& wanted) {
    std::vector<SessionMeta> archived;
    std::vector<IndexEntry> gone;
    for (const auto& entry : entries_of(state)) {
        if (!wanted(entry)) continue;
        SessionMeta meta;
        switch (inspect_entry(projects_root, entry, &meta)) {
        case EntryState::Archived: archived.push_back(std::move(meta)); break;
        case EntryState::Gone: gone.push_back(entry); break;
        case EntryState::Unknown: break;
        }
    }
    if (!gone.empty()) prune(index_path, projects_root, gone);
    return archived;
}

struct ScannedArchive {
    IndexEntry entry;
    SessionMeta meta;
};

// 整目录扫描,只留归档的。补建索引与不认识的目录布局都走这里。
std::vector<ScannedArchive> scan_archived(const std::string& project_dir, const std::string& key) {
    SessionLoadTimer timer("archived_index_scan", key);
    std::vector<ScannedArchive> found;
    for (const auto& file : SessionStorage::list_meta_files(project_dir)) {
        auto meta = SessionStorage::read_meta(file.meta_path);
        if (meta.id.empty() || !meta.archived) continue;
        auto stem = session_stem(file.meta_path);
        if (!stem) continue;
        const bool no_workspace = meta.no_workspace;
        found.push_back({{key, std::move(*stem), no_workspace}, std::move(meta)});
    }
    return found;
}

std::vector<SessionMeta> metas_of(std::vector<ScannedArchive> found, bool no_workspace_only) {
    std::vector<SessionMeta> metas;
    for (auto& item : found) {
        if (no_workspace_only && !item.meta.no_workspace) continue;
        metas.push_back(std::move(item.meta));
    }
    return metas;
}

} // namespace

std::string archived_session_index_path(const std::string& project_dir) {
    const auto location = locate_project_dir(project_dir);
    return location ? index_path_for_root(location->projects_root) : std::string{};
}

void note_archived_session_meta(const std::string& meta_path, const SessionMeta& meta) {
    if (!meta.archived) return;
    const auto location = locate_project_dir(path_to_utf8(path_from_utf8(meta_path).parent_path()));
    const auto stem = session_stem(meta_path);
    if (!location || !stem) return;
    const IndexEntry entry{location->key, *stem, meta.no_workspace};
    const auto index_path = index_path_for_root(location->projects_root);
    const auto& root = location->projects_root;
    const auto result = update_state_json_at(index_path, [&entry, &root](json& state) {
        state = normalized(std::move(state));
        if (contains_entry(state, entry)) return false;
        drop_missing_projects(state, root);
        add_entry(state, entry);
        return true;
    });
    if (!result.persisted) LOG_WARN("[archived-index] failed to record " + *stem + " in " + index_path);
}

std::vector<SessionMeta> list_archived_session_metadata(const std::string& project_dir) {
    const auto location = locate_project_dir(project_dir);
    if (!location) return metas_of(scan_archived(project_dir, {}), false);
    const auto index_path = index_path_for_root(location->projects_root);
    const auto& key = location->key;
    const auto& root = location->projects_root;
    auto state = read_index(index_path);
    if (!project_scanned(state, key)) {
        auto found = scan_archived(project_dir, key);
        auto committed = update_state_json_at(index_path, [&found, &key, &root](json& current) {
            current = normalized(std::move(current));
            drop_missing_projects(current, root);
            for (const auto& item : found) add_entry(current, item.entry);
            current["scanned_projects"][key] = true;
            return true;
        });
        if (!committed.persisted) {
            LOG_WARN("[archived-index] failed to record scan of " + project_dir);
            return metas_of(std::move(found), false);
        }
        state = std::move(committed.state);
    }
    return read_indexed(state, location->projects_root, index_path,
                        [&key](const IndexEntry& entry) { return entry.project == key; });
}

std::vector<SessionMeta> list_archived_no_workspace_session_metadata(
    const std::string& projects_root,
    const std::function<std::vector<std::string>()>& no_workspace_project_dirs) {
    const auto root = without_trailing_separator(path_from_utf8(projects_root));
    const auto list_dirs = [&no_workspace_project_dirs] {
        return no_workspace_project_dirs ? no_workspace_project_dirs() : std::vector<std::string>{};
    };
    if (!is_projects_root(root)) {
        std::vector<SessionMeta> metas;
        for (const auto& dir : list_dirs()) {
            for (auto& meta : metas_of(scan_archived(dir, {}), true)) metas.push_back(std::move(meta));
        }
        return metas;
    }
    const auto index_path = index_path_for_root(root);
    auto state = read_index(index_path);
    std::vector<SessionMeta> outside_root;
    if (!state["no_workspace_scanned"].get<bool>()) {
        std::vector<ScannedArchive> found;
        for (const auto& dir : list_dirs()) {
            const auto location = locate_project_dir(dir);
            if (!location || location->projects_root != root) {
                // 不在这个根下的目录没法按目录名找回,不进索引,也就不能标记为已扫描。
                for (auto& meta : metas_of(scan_archived(dir, {}), true)) {
                    outside_root.push_back(std::move(meta));
                }
                continue;
            }
            for (auto& item : scan_archived(dir, location->key)) {
                if (item.meta.no_workspace) found.push_back(std::move(item));
            }
        }
        const bool complete = outside_root.empty();
        auto committed = update_state_json_at(index_path, [&found, complete, &root](json& current) {
            current = normalized(std::move(current));
            drop_missing_projects(current, root);
            for (const auto& item : found) add_entry(current, item.entry);
            if (complete) current["no_workspace_scanned"] = true;
            return true;
        });
        if (!committed.persisted) {
            LOG_WARN("[archived-index] failed to record no-workspace scan in " + index_path);
            auto metas = metas_of(std::move(found), true);
            for (auto& meta : outside_root) metas.push_back(std::move(meta));
            return metas;
        }
        state = std::move(committed.state);
    }
    auto metas = read_indexed(state, root, index_path,
                              [](const IndexEntry& entry) { return entry.no_workspace; });
    std::vector<SessionMeta> result;
    for (auto& meta : metas) {
        if (meta.no_workspace) result.push_back(std::move(meta));
    }
    for (auto& meta : outside_root) result.push_back(std::move(meta));
    return result;
}

} // namespace acecode
