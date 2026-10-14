#pragma once

#include "session_storage.hpp"

#include <filesystem>
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
nlohmann::json read_sidebar_layout(const std::filesystem::path& path);
bool write_sidebar_layout(const std::filesystem::path& path,
                          const nlohmann::json& layout, std::string& error);

// Owned by the Web server; callers serialize access with its sidebar mutex.
// Directory enumeration is cheap relative to repeatedly parsing every meta file.
class SidebarMetadataCache {
public:
    std::vector<SessionMeta> read(const std::vector<std::string>& directories);
private:
    struct Entry {
        std::filesystem::file_time_type modified;
        std::uintmax_t size = 0;
        SessionMeta meta;
    };
    std::map<std::string, std::map<std::string, Entry>> directories_;
};

} // namespace acecode
