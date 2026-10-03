#pragma once

// Persistent directory of a mesh agent tree: canonical path -> session id for
// every spawned agent of one root. Stored in the root session's data directory
// (<project_dir>/<root_id>/mesh_agents.json), so purging the root removes it and
// a restarted daemon rebuilds the tree without scanning every session meta.

#include <string>
#include <vector>

namespace acecode::mesh {

struct MeshTreeIndexEntry {
    std::string path;
    std::string session_id;
};

std::string mesh_tree_index_path(const std::string& project_dir, const std::string& root_id);
// Missing or unreadable index -> empty list. Malformed entries are skipped.
std::vector<MeshTreeIndexEntry> read_mesh_tree_index(const std::string& project_dir,
                                                     const std::string& root_id);
bool write_mesh_tree_index(const std::string& project_dir, const std::string& root_id,
                           const std::vector<MeshTreeIndexEntry>& entries);

} // namespace acecode::mesh
