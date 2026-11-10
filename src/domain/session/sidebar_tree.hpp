#pragma once

#include "session_storage.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace acecode {

// Presentation relationships only; never modify SessionMeta::parent_session_id.
struct SidebarNode {
    std::string key;
    std::string session_id;
    bool scheduled = false;
    bool group = false;
    std::string loop_id;
    std::string parent;
    std::vector<std::string> children;
};

struct SidebarTree {
    std::map<std::string, SidebarNode> nodes;
    std::vector<std::string> roots;
};

SidebarTree build_sidebar_tree(const nlohmann::json& sessions,
                               const nlohmann::json& layout);
bool move_sidebar_node(SidebarTree& tree, nlohmann::json& layout,
                       const std::string& source, const std::string& target,
                       const std::string& placement, std::string& error);
// All sources move in caller order, or neither tree nor layout is changed.
bool move_sidebar_nodes(SidebarTree& tree, nlohmann::json& layout,
                        const std::vector<std::string>& sources, const std::string& target,
                        const std::string& placement, std::string& error);
nlohmann::json read_sidebar_layout(const std::filesystem::path& path);
bool write_sidebar_layout(const std::filesystem::path& path,
                          const nlohmann::json& layout, std::string& error);

// One project directory to look for sessions in, and the workspace its records
// are reported under.
struct SessionDirectoryScope {
    std::string project_dir;
    std::string workspace_hash;     // Empty for a no-workspace session directory.
    bool no_workspace_only = false; // Keep only records flagged no_workspace.
};

struct RecentSession {
    SessionMeta meta;
    std::string workspace_hash; // Empty when the session belongs to no workspace.
};

// Owned by the Web server; callers serialize access with its sidebar mutex.
// Directory enumeration is cheap relative to repeatedly parsing every meta file.
class SidebarMetadataCache {
public:
    std::vector<SessionMeta> read(const std::vector<std::string>& directories);
    // 这些目录里最近更新的 limit 条顶层、未归档会话(按 updated_at,缺省取
    // created_at)。与 read() 共用同一份缓存:侧栏已经读过的 meta 不再打开;没读过
    // 的按文件修改时间从新到旧补读,读到不可能再排进前 limit 条为止,其余留给
    // read()。只复制选中的那几条。同一个目录列多次只看第一次;accept 返回 false
    // 的记录不占名额。
    std::vector<RecentSession> recent(
        const std::vector<SessionDirectoryScope>& scopes, std::size_t limit,
        const std::function<bool(const SessionMeta&, const std::string& workspace_hash)>& accept = {});
private:
    struct Entry {
        std::filesystem::file_time_type modified;
        std::uintmax_t size = 0;
        SessionMeta meta;
    };
    using Directory = std::map<std::string, Entry>;
    // Re-reads only meta files whose size or mtime changed and forgets deleted
    // ones. Null when the directory cannot be enumerated.
    const Directory* refresh(const std::string& directory);
    std::map<std::string, Directory> directories_;
};

} // namespace acecode
