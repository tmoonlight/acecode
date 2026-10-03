#include "mesh_tree_index.hpp"

#include "agent_path.hpp"
#include "utils/atomic_file.hpp"
#include "utils/utf8_path.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>

namespace acecode::mesh {

std::string mesh_tree_index_path(const std::string& project_dir, const std::string& root_id) {
    return path_to_utf8(path_from_utf8(project_dir) / path_from_utf8(root_id) /
                        "mesh_agents.json");
}

std::vector<MeshTreeIndexEntry> read_mesh_tree_index(const std::string& project_dir,
                                                     const std::string& root_id) {
    std::vector<MeshTreeIndexEntry> entries;
    if (project_dir.empty() || root_id.empty()) return entries;
    std::ifstream in(path_from_utf8(mesh_tree_index_path(project_dir, root_id)),
                     std::ios::binary);
    if (!in.is_open()) return entries;
    try {
        const std::string text((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        const auto json = nlohmann::json::parse(text);
        if (!json.is_object() || !json.contains("agents") || !json["agents"].is_array()) {
            return entries;
        }
        for (const auto& item : json["agents"]) {
            if (!item.is_object()) continue;
            MeshTreeIndexEntry entry;
            entry.path = item.value("path", std::string{});
            entry.session_id = item.value("session_id", std::string{});
            const auto path = AgentPath::parse(entry.path);
            if (!path || path->is_root() || entry.session_id.empty()) continue;
            entries.push_back(std::move(entry));
        }
    } catch (...) {
        entries.clear();
    }
    return entries;
}

bool write_mesh_tree_index(const std::string& project_dir, const std::string& root_id,
                           const std::vector<MeshTreeIndexEntry>& entries) {
    if (project_dir.empty() || root_id.empty()) return false;
    nlohmann::json agents = nlohmann::json::array();
    for (const auto& entry : entries) {
        agents.push_back({{"path", entry.path}, {"session_id", entry.session_id}});
    }
    const nlohmann::json json = {{"version", 1}, {"agents", std::move(agents)}};
    const auto path = mesh_tree_index_path(project_dir, root_id);
    std::error_code ec;
    std::filesystem::create_directories(path_from_utf8(path).parent_path(), ec);
    return atomic_write_file(path, json.dump(2) + '\n');
}

} // namespace acecode::mesh
