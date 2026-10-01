#pragma once

#include "tool_executor.hpp"

#include <memory>

namespace acecode {

class MemoryService;

// `memory_read` — read persistent memory from the global scope and the
// session's workspace scope. Read-only; every call rescans disk so entries
// written by another ACECode process are visible. Args are all optional:
//   {}                 → entries of both scopes (scope, name, description, type, updated_at)
//   {scope}            → only that scope (global | workspace | all)
//   {type}             → filter by user | feedback | project | reference
//   {name}             → one entry with full body; workspace first, then global
//   {query}            → case-insensitive substring match with a snippet
// Missing entries return {found:false} rather than an error.
ToolImpl create_memory_read_tool(std::shared_ptr<MemoryService> memory);

} // namespace acecode
