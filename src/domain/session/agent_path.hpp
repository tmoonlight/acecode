#pragma once

// Canonical agent paths for the mesh swarm mode, ported from Codex
// codex-rs/protocol/src/agent_path.rs. `/root` is the tree root and every
// child appends one task name. Codex's reserved `/morpheus` path is not
// supported. Error texts match Codex so model-facing errors stay familiar.

#include <optional>
#include <string>
#include <utility>

namespace acecode::mesh {

class AgentPath {
public:
    static constexpr const char* kRoot = "/root";

    static AgentPath root();
    // Absolute paths only; nullopt with *error on rejection.
    static std::optional<AgentPath> parse(const std::string& path,
                                          std::string* error = nullptr);

    const std::string& str() const { return path_; }
    bool is_root() const { return path_ == kRoot; }
    // Last segment; "root" for the root path.
    std::string name() const;
    // Number of segments below root (root = 0).
    int depth() const;
    // Parent path; nullopt for the root.
    std::optional<AgentPath> parent() const;
    // True when this path equals prefix or lies below it.
    bool has_prefix(const std::string& prefix) const;

    std::optional<AgentPath> join(const std::string& agent_name,
                                  std::string* error = nullptr) const;
    // `/root...` is canonical; other references resolve below this path.
    std::optional<AgentPath> resolve(const std::string& reference,
                                     std::string* error = nullptr) const;

    bool operator==(const AgentPath& other) const { return path_ == other.path_; }
    bool operator!=(const AgentPath& other) const { return path_ != other.path_; }
    bool operator<(const AgentPath& other) const { return path_ < other.path_; }

private:
    explicit AgentPath(std::string path) : path_(std::move(path)) {}
    std::string path_;
};

// Empty when valid; otherwise the Codex error text.
std::string validate_agent_name(const std::string& agent_name);

} // namespace acecode::mesh
