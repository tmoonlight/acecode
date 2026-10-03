#include "agent_path.hpp"

#include <utility>

namespace acecode::mesh {

namespace {

constexpr const char* kRootSegment = "root";

bool fail(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return false;
}

bool validate_name(const std::string& name, std::string* error) {
    const std::string message = validate_agent_name(name);
    if (message.empty()) return true;
    return fail(error, message);
}

bool validate_absolute(const std::string& path, std::string* error) {
    const std::string prefix_error =
        "absolute agent paths must start with `/root` or be `/morpheus`";
    if (path.empty() || path.front() != '/') return fail(error, prefix_error);
    const std::string stripped = path.substr(1);
    const auto first_slash = stripped.find('/');
    const std::string head = stripped.substr(0, first_slash);
    if (head != kRootSegment) return fail(error, prefix_error);
    if (!stripped.empty() && stripped.back() == '/') {
        return fail(error, "absolute agent path must not end with `/`");
    }
    if (first_slash == std::string::npos) return true;
    std::size_t start = first_slash + 1;
    while (true) {
        const auto next = stripped.find('/', start);
        if (!validate_name(stripped.substr(start, next - start), error)) return false;
        if (next == std::string::npos) return true;
        start = next + 1;
    }
}

bool validate_relative(const std::string& reference, std::string* error) {
    if (!reference.empty() && reference.back() == '/') {
        return fail(error, "relative agent path must not end with `/`");
    }
    std::size_t start = 0;
    while (true) {
        const auto next = reference.find('/', start);
        if (!validate_name(reference.substr(start, next - start), error)) return false;
        if (next == std::string::npos) return true;
        start = next + 1;
    }
}

} // namespace

std::string validate_agent_name(const std::string& agent_name) {
    if (agent_name.empty()) return "agent_name must not be empty";
    if (agent_name == kRootSegment) return "agent_name `root` is reserved";
    if (agent_name == "." || agent_name == "..") {
        return "agent_name `" + agent_name + "` is reserved";
    }
    if (agent_name.find('/') != std::string::npos) {
        return "agent_name must not contain `/`";
    }
    for (const char ch : agent_name) {
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_';
        if (!ok) {
            return "agent_name must use only lowercase letters, digits, and underscores";
        }
    }
    return {};
}

AgentPath AgentPath::root() {
    return AgentPath(kRoot);
}

std::optional<AgentPath> AgentPath::parse(const std::string& path, std::string* error) {
    if (!validate_absolute(path, error)) return std::nullopt;
    return AgentPath(path);
}

std::string AgentPath::name() const {
    if (is_root()) return kRootSegment;
    const auto slash = path_.rfind('/');
    const std::string segment = slash == std::string::npos ? path_ : path_.substr(slash + 1);
    return segment.empty() ? std::string(kRootSegment) : segment;
}

int AgentPath::depth() const {
    int depth = 0;
    for (std::size_t i = 1; i < path_.size(); ++i) {
        if (path_[i] == '/') ++depth;
    }
    return depth;
}

std::optional<AgentPath> AgentPath::parent() const {
    if (is_root()) return std::nullopt;
    const auto slash = path_.rfind('/');
    if (slash == std::string::npos || slash == 0) return std::nullopt;
    return AgentPath(path_.substr(0, slash));
}

bool AgentPath::has_prefix(const std::string& prefix) const {
    if (prefix.empty()) return true;
    if (path_.compare(0, prefix.size(), prefix) != 0) return false;
    return path_.size() == prefix.size() || path_[prefix.size()] == '/';
}

std::optional<AgentPath> AgentPath::join(const std::string& agent_name,
                                         std::string* error) const {
    if (!validate_name(agent_name, error)) return std::nullopt;
    return parse(path_ + "/" + agent_name, error);
}

std::optional<AgentPath> AgentPath::resolve(const std::string& reference,
                                            std::string* error) const {
    if (reference.empty()) {
        fail(error, "agent path must not be empty");
        return std::nullopt;
    }
    if (reference == kRoot) return root();
    if (reference.front() == '/') return parse(reference, error);
    if (!validate_relative(reference, error)) return std::nullopt;
    return parse(path_ + "/" + reference, error);
}

} // namespace acecode::mesh
