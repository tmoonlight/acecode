#include "memory_types.hpp"

namespace acecode {

std::string memory_type_to_string(MemoryType t) {
    switch (t) {
        case MemoryType::User:      return "user";
        case MemoryType::Feedback:  return "feedback";
        case MemoryType::Project:   return "project";
        case MemoryType::Reference: return "reference";
    }
    return "user";
}

std::optional<MemoryType> parse_memory_type(const std::string& s) {
    if (s == "user")      return MemoryType::User;
    if (s == "feedback")  return MemoryType::Feedback;
    if (s == "project")   return MemoryType::Project;
    if (s == "reference") return MemoryType::Reference;
    return std::nullopt;
}

std::string memory_scope_to_string(MemoryScope s) {
    return s == MemoryScope::Workspace ? "workspace" : "global";
}

std::optional<MemoryScope> parse_memory_scope(const std::string& s) {
    if (s == "global") return MemoryScope::Global;
    if (s == "workspace") return MemoryScope::Workspace;
    return std::nullopt;
}

MemoryScope default_memory_scope_for_type(MemoryType type) {
    return type == MemoryType::Project || type == MemoryType::Reference
        ? MemoryScope::Workspace
        : MemoryScope::Global;
}

} // namespace acecode
